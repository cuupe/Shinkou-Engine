#include "shinkou/MathParallel.h"

#include <algorithm>
#include <limits>

namespace shinkou::math {

ParallelExecutor::ParallelExecutor(ParallelConfig config) {
    defaultGrain_ = std::max<std::size_t>(1, config.grainSize);
    if (!config.enabled) return;
    const auto hardware = std::thread::hardware_concurrency();
    const std::size_t requestedUncapped = config.workerCount != 0
        ? config.workerCount
        : hardware > 1 ? std::min<std::size_t>(8, static_cast<std::size_t>(hardware - 1)) : 0;
    const std::size_t requested = std::min<std::size_t>(requestedUncapped, 64);
    workers_.reserve(requested);
    pendingJobs_.reserve(std::max<std::size_t>(64, requested * 4));
    try {
        for (std::size_t index = 0; index < requested; ++index) workers_.emplace_back(&ParallelExecutor::worker_loop, this, index);
    } catch (...) {
        shutdown();
        throw;
    }
}

ParallelExecutor::~ParallelExecutor() {
    shutdown();
}

void ParallelExecutor::shutdown() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    workAvailable_.notify_all();
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
}

bool ParallelExecutor::is_shutdown() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopping_;
}

void ParallelExecutor::run_erased(std::size_t count, std::size_t grain, TaskFunction function, void* context) {
    const std::size_t effectiveGrain = grain == 0 ? defaultGrain_ : grain;
    if (count == 0 || !function) return;
    bool serial = workers_.empty() || count < effectiveGrain;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        serial = serial || stopping_;
    }
    if (serial) {
        function(context, 0, count);
        return;
    }

    Job job(function, context, count, std::max<std::size_t>(1, effectiveGrain), workers_.size() + 1);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            serial = true;
        } else {
            for (std::size_t index = 0; index < workers_.size(); ++index) pendingJobs_.push_back(&job);
        }
    }
    if (serial) {
        function(context, 0, count);
        return;
    }
    workAvailable_.notify_all();

    // The submitting thread also participates. This makes nested submissions
    // safe: a worker waiting on a child job can execute that child job itself.
    execute_job(job);

    std::unique_lock<std::mutex> lock(mutex_);
    workComplete_.wait(lock, [&job] { return job.remainingWorkers.load(std::memory_order_acquire) == 0; });
    const std::exception_ptr exception = job.exception;
    lock.unlock();
    if (exception) std::rethrow_exception(exception);
}

void ParallelExecutor::execute_job(Job& job) noexcept {
    while (!job.failed.load(std::memory_order_acquire)) {
        const std::size_t begin = job.next.fetch_add(job.grain, std::memory_order_relaxed);
        if (begin >= job.count) break;
        const std::size_t end = std::min(job.count, begin + job.grain);
        try {
            job.function(job.context, begin, end);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!job.failed.exchange(true, std::memory_order_acq_rel)) job.exception = std::current_exception();
            break;
        }
    }
    if (job.remainingWorkers.fetch_sub(1, std::memory_order_acq_rel) == 1) workComplete_.notify_all();
}

void ParallelExecutor::worker_loop(std::size_t workerIndex) {
    static_cast<void>(workerIndex);
    for (;;) {
        Job* job = nullptr;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            workAvailable_.wait(lock, [this] { return stopping_ || !pendingJobs_.empty(); });
            if (stopping_) return;
            job = pendingJobs_.back();
            pendingJobs_.pop_back();
        }
        if (job) execute_job(*job);
    }
}

}
