#include "shinkou/ecs/ChunkPipeline.h"

namespace shinkou::ecs {

void ChunkPipeline::require_idle() const {
    if (running_.load(std::memory_order_acquire))
        throw std::logic_error("cannot mutate a running chunk pipeline");
}

void ChunkPipeline::clear() {
    require_idle();
    nodes_.clear();
    dirty_ = true;
}

void ChunkPipeline::compile() {
    for (auto &node : nodes_) {
        node->successors.clear();
        node->predecessors = 0;
        node->depth = 1;
    }
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        for (std::size_t j = i + 1; j < nodes_.size(); ++j) {
            bool conflict = false;
            for (const auto a : nodes_[i]->accesses)
                for (const auto b : nodes_[j]->accesses)
                    conflict |= a.type == b.type && (a.write || b.write);
            if (!conflict)
                continue;
            nodes_[i]->successors.push_back(j);
            ++nodes_[j]->predecessors;
            nodes_[j]->depth = std::max(nodes_[j]->depth, nodes_[i]->depth + 1);
        }
    }
    std::size_t longestPath = 0;
    for (const auto &node : nodes_)
        longestPath = std::max(longestPath, node->depth);
    parallelSystems_ = longestPath < nodes_.size();
    dirty_ = false;
}

void ChunkPipeline::record_error(std::exception_ptr error) noexcept {
    std::lock_guard<std::mutex> lock(errorMutex_);
    if (!error_)
        error_ = std::move(error);
    failed_.store(true, std::memory_order_release);
}

void ChunkPipeline::dispatch(std::size_t index) {
    try {
        jobs_->submit(nodes_[index]->task, &completion_);
    } catch (...) {
        // Submission allocation failed before publication. Drain the graph
        // inline so no job retains references to a failed run's state.
        record_error(std::current_exception());
        execute(index);
    }
}

void ChunkPipeline::execute(std::size_t index) noexcept {
    auto &node = *nodes_[index];
    if (!failed_.load(std::memory_order_acquire)) {
        try {
            node.kernel(jobs_, *node.chunks);
        } catch (...) {
            record_error(std::current_exception());
        }
    }
    for (const auto next : node.successors) {
        // The final predecessor acquires all previous component writes before
        // publishing the successor. Every node is dispatched exactly once.
        if (nodes_[next]->pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
            dispatch(next);
    }
}

void ChunkPipeline::run(jobs::JobSystem *jobs) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        throw std::logic_error("chunk pipeline is already running");
    struct Reset {
        std::atomic<bool> &running;
        ~Reset() {
            running.store(false, std::memory_order_release);
        }
    } reset{running_};
    if (nodes_.empty())
        return;
    if (dirty_)
        compile();
    for (auto &node : nodes_) {
        node->chunks = &node->query->chunks();
        node->pending.store(node->predecessors, std::memory_order_relaxed);
    }
    // A total dependency order cannot overlap systems. Avoid submitting one
    // task per system in that case; individual kernels can still fan out.
    if (!jobs || !parallelSystems_) {
        for (auto &node : nodes_)
            node->kernel(jobs, *node->chunks);
        return;
    }
    jobs_ = jobs;
    error_ = nullptr;
    failed_.store(false, std::memory_order_relaxed);
    // Keep the join nonzero while roots are still being submitted. A worker
    // can complete a short root before the caller submits the remaining roots.
    completion_.add();
    for (std::size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i]->predecessors == 0)
            dispatch(i);
    completion_.complete();
    jobs->wait(completion_);
    jobs_ = nullptr;
    if (error_)
        std::rethrow_exception(error_);
}

} // namespace shinkou::ecs
