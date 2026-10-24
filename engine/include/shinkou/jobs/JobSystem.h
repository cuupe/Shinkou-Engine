#pragma once

// Shinkou Engine - engine-wide task system.
//
// Design summary (details and measurements live in engine/docs/jobs.md):
//   * Every worker owns a bounded, lock-free deque; idle workers steal from the
//     tail of somebody else's deque. Producers that are not workers feed a
//     shared overflow queue instead, so the common path never allocates.
//   * Jobs are recycled through a chunked pool. After the first refill a job
//     submission performs no heap allocation and no global lock.
//   * Blocking waits always *help*: a thread that waits on a counter runs
//     pending jobs while it waits. Nested waits therefore cannot deadlock, and
//     a single-worker system still makes progress.
//
// Contracts (violating these is undefined behaviour):
//   * A Counter must outlive every job attached to it.
//   * A job body must be safe to run on any worker thread, at any time.
//   * No thread may call into a JobSystem while it is being destroyed.
//   * parallel_for bodies must not depend on which thread runs which index; the
//     partitioning is dynamic by design. For a deterministic fallback pass a
//     grain that covers the whole range (the loop then runs serially).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

namespace shinkou::jobs {

class JobSystem;

namespace detail {
// Opaque implementation state. The name is public only so worker records can
// point at it; the layout is defined in JobSystem.cpp and never escapes.
struct JobSystemImpl;
} // namespace detail

// A completion counter for a group of jobs.
//
// Typical use -- submit() reserves the completion itself, so do NOT pre-add:
//     jobs::Counter counter;
//     for (int i = 0; i < 3; ++i) system.submit(work, &counter);
//     system.wait(counter);
//
// add()/complete() are the manual form, for work that is not one submitted job
// (a job that discovers further work, an external signal, and so on). Mixing
// add() with a submit() that carries the same counter double-counts.
//
// `wait()` must not be called from a thread that cannot make progress on its
// own: prefer JobSystem::wait, which is help-aware.
class Counter final {
public:
    Counter() noexcept = default;
    explicit Counter(std::int64_t initial) noexcept : remaining_(initial) {}

    ~Counter() = default;
    Counter(const Counter&) = delete;
    Counter& operator=(const Counter&) = delete;
    Counter(Counter&&) = delete;
    Counter& operator=(Counter&&) = delete;

    // Reserves pending completions for work that will be submitted later (for
    // example a job that discovers new work at runtime). Every added unit must be
    // balanced by either a submit() carrying this counter or an explicit
    // complete() call.
    void add(std::int64_t count = 1) noexcept {
        remaining_.fetch_add(count, std::memory_order_acq_rel);
    }

    // Releases `count` completions and returns true when this call carried the
    // counter to zero. Clamped at zero: over-completing never goes negative and
    // never reports completion twice by accident.
    bool complete(std::int64_t count = 1) noexcept;

    std::int64_t remaining() const noexcept {
        return remaining_.load(std::memory_order_acquire);
    }

    bool done() const noexcept { return remaining() == 0; }

    // Cancellation is advisory and only affects jobs whose body has not started
    // yet: a cancelled job completes its counter without running the body.
    void cancel() noexcept { cancelled_.store(true, std::memory_order_release); }
    bool cancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }

    // Waits for the counter to reach zero using the owning system, which lets
    // the calling thread execute pending jobs instead of sleeping. Falls back to
    // a plain spin when the counter was never attached to a JobSystem.
    void wait();

private:
    friend class JobSystem;
    friend struct JobSystemAccess;

    std::atomic<std::int64_t> remaining_{0};
    std::atomic<bool> cancelled_{false};
    std::atomic<JobSystem*> owner_{nullptr};
};

class JobSystem final {
public:
    using Function = std::function<void()>;

    // workerCount == 0 selects std::thread::hardware_concurrency(); the result is
    // always clamped to [1, kMaxWorkers].
    explicit JobSystem(std::size_t workerCount = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    static constexpr std::size_t kMaxWorkers = 64;

    std::size_t worker_count() const noexcept;
    static std::size_t hardware_worker_count() noexcept;

    // Queues one job. When `done` is non-null it is incremented before the job
    // becomes visible and decremented after the body ran (or was cancelled).
    // submit is safe to call from any thread, including from inside a job.
    void submit(Function fn, Counter* done = nullptr);

    // Queues `count` independent jobs with a handful of queue transactions
    // instead of one per job. Measured on this codebase, a submit() loop costs
    // ~750 ns/job once the workers are actively draining, while batching brings
    // the producer side close to its ~115 ns floor. Prefer this whenever the jobs
    // do not depend on each other being queued separately.
    //
    // `jobs` is read during the call only; the callables are copied into the
    // queue. If `done` is non-null it is incremented by `count` before the batch
    // becomes visible.
    void submit_batch(const Function* jobs, std::size_t count, Counter* done = nullptr);

    // Queues the same callable `count` times with one transaction. This is the
    // shape parallel_for needs for its helper tasks (N identical chunk jobs), and
    // it avoids materialising an array of N callables just to hand them over.
    void submit_batch(const Function& job, std::size_t count, Counter* done = nullptr);

    // Splits [0, count) into `grain`-sized chunks and processes them on the pool.
    // Blocks until done and rethrows the first exception thrown by the body.
    void parallel_for_fn(std::size_t count, std::size_t grain,
                         const std::function<void(std::size_t)>& fn);

    template <class Body>
    void parallel_for(std::size_t count, std::size_t grain, Body&& body) {
        static_assert(std::is_invocable_v<Body&, std::size_t>,
                      "parallel_for body must be callable as void(std::size_t)");
        parallel_for_fn(count, grain,
                        std::function<void(std::size_t)>(std::forward<Body>(body)));
    }

    // Blocks until every job attached to `counter` completed. While waiting the
    // calling thread runs pending jobs. Exceptions are not rethrown here; use
    // take_exception() if the jobs were submitted with submit().
    void wait(Counter& counter);

    // Runs at most `maxJobs` pending jobs on the calling thread and returns how
    // many it executed. Intended for frame budgeting in a main loop.
    std::size_t run_pending(std::size_t maxJobs = (std::numeric_limits<std::size_t>::max)());

    // Blocks until no job is queued or running anywhere in the system.
    void wait_idle();

    bool is_worker_thread() const noexcept { return worker_thread_of() == this; }

    // Returns and clears the first exception captured from a job body. Jobs
    // submitted through submit() have no other error channel, so long-running
    // systems should poll this. parallel_for() handles its own exceptions.
    std::exception_ptr take_exception() noexcept;

private:
    // Returns `this` when the calling thread is one of this system's workers.
    const JobSystem* worker_thread_of() const noexcept;

    void parallel_for_chunk(std::size_t count, std::size_t grain,
                            const std::function<void(std::size_t)>& fn,
                            std::atomic<std::size_t>& next);
    bool parallel_for_one_chunk(std::size_t count, std::size_t grain,
                                const std::function<void(std::size_t)>& fn,
                                std::atomic<std::size_t>& next);

    std::unique_ptr<detail::JobSystemImpl> impl_;
};

} // namespace shinkou::jobs
