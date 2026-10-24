#include "shinkou/jobs/JobSystem.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace shinkou::jobs {

// Counter keeps its state private even from the nested implementation type, so
// the few operations that need to touch it are funnelled through this shim.
struct JobSystemAccess final {
    static void attach(Counter& counter, JobSystem* system) noexcept {
        counter.owner_.store(system, std::memory_order_release);
    }
    static JobSystem* owner(const Counter& counter) noexcept {
        return counter.owner_.load(std::memory_order_acquire);
    }
    static bool cancelled(const Counter& counter) noexcept {
        return counter.cancelled_.load(std::memory_order_acquire);
    }
    // Decrements and returns true exactly once: when the counter reaches zero.
    static bool complete(Counter& counter) noexcept { return counter.complete(1); }
};

namespace {

// --- platform timing -------------------------------------------------------

// A short pause is cheaper than a yield: it tells the core that this is a spin
// loop so the sibling hyper-thread gets the execution slot, without a syscall.
inline void cpu_relax() noexcept {
#if defined(_MSC_VER)
#if defined(_M_IX86) || defined(_M_X64)
    _mm_pause();
#elif defined(_M_ARM64)
    __yield();
#endif
    _ReadWriteBarrier();
#elif defined(__GNUC__) || defined(__clang__)
#if defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
#else
    std::this_thread::yield();
#endif
}

// --- tuning ----------------------------------------------------------------

// Ring size per worker. Bounded on purpose: a fixed ring means a stolen slot can
// never be recycled underneath a thief, which removes the memory-reclamation
// class of bugs a growable deque would introduce.
constexpr std::size_t kDequeCapacity = 1024;
constexpr std::size_t kChunkTasks = 256;
constexpr std::size_t kTaskBatch = 64;
constexpr std::size_t kLocalFreeTarget = 256;
// Upper bound on tasks moved by one steal transaction.
constexpr std::size_t kStealBatch = 16;
constexpr unsigned kPauseBudget = 32;
// Idle workers spin this long before sleeping: roughly 20 us on a modern core.
// Long enough that a steadily fed pool never pays a wakeup per job, short enough
// that a genuinely idle engine stops burning a core quickly.
constexpr unsigned kHotSpin = 2048;
// Sample the shared work counter once per 64 pauses while spinning.
constexpr unsigned kSpinSampleMask = 255u;

// parallel_for runs on the caller first and only fans out after this much inline
// work: below it a hand-off costs more than the work itself.
constexpr std::int64_t kEscalateAfterNs = 20000;
constexpr unsigned kSpinBudget = 256;
constexpr std::chrono::milliseconds kIdleSlice{1};

struct Task final {
    std::function<void()> fn;
    Counter* counter{nullptr};
    Task* next{nullptr};
};

// A tiny spin lock. Critical sections guarded by it are a handful of pointer
// writes, so a mutex (and its syscall fallback) would cost more than the work.
class SpinLock final {
public:
    SpinLock() noexcept = default;
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    void lock() noexcept {
        unsigned pauses = 0;
        while (flag_.test_and_set(std::memory_order_acquire)) {
            if (pauses < kPauseBudget) {
                cpu_relax();
                ++pauses;
            } else {
                std::this_thread::yield();
                pauses = 0;
            }
        }
    }

    void unlock() noexcept { flag_.clear(std::memory_order_release); }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

// Recycled task storage. Everything is owned by one chunk list, so a task can
// never dangle: the memory outlives every job and is only released with the
// pool itself.
class TaskPool final {
public:
    TaskPool() = default;
    ~TaskPool() { reset(); }

    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;

    // Writes `count` cleared tasks to `out`. Allocates when the free list is dry.
    std::size_t take(Task** out, std::size_t count) {
        std::lock_guard<SpinLock> guard(lock_);
        std::size_t produced = 0;
        while (produced < count) {
            Task* task = freeList_;
            if (task != nullptr) {
                freeList_ = task->next;
            } else {
                task = allocate_locked();
            }
            task->fn = nullptr;
            task->counter = nullptr;
            task->next = nullptr;
            out[produced++] = task;
        }
        return produced;
    }

    void give(Task** tasks, std::size_t count) noexcept {
        std::lock_guard<SpinLock> guard(lock_);
        for (std::size_t i = 0; i < count; ++i) {
            tasks[i]->next = freeList_;
            freeList_ = tasks[i];
        }
    }

private:
    struct Chunk {
        Chunk* previous{nullptr};
        std::size_t capacity{0};
        std::size_t used{0};
        Task* slots{nullptr};
    };

    Task* allocate_locked() {
        if (active_ == nullptr || active_->used >= active_->capacity) grow_locked();
        return &active_->slots[active_->used++];
    }

    void grow_locked() {
        // Header plus slots live in one allocation; the header is padded so the
        // slot array starts on a Task boundary.
        const std::size_t header =
            (sizeof(Chunk) + alignof(Task) - 1) / alignof(Task) * alignof(Task);
        const std::size_t bytes = header + kChunkTasks * sizeof(Task);
        void* memory = ::operator new(bytes);
        auto* chunk = static_cast<Chunk*>(memory);
        chunk->previous = chunks_;
        chunk->capacity = kChunkTasks;
        chunk->used = 0;
        chunk->slots = reinterpret_cast<Task*>(static_cast<unsigned char*>(memory) + header);
        for (std::size_t i = 0; i < kChunkTasks; ++i) new (&chunk->slots[i]) Task();
        chunks_ = chunk;
        active_ = chunk;
    }

    void reset() noexcept {
        for (Chunk* chunk = chunks_; chunk != nullptr;) {
            Chunk* previous = chunk->previous;
            for (std::size_t i = 0; i < chunk->capacity; ++i) chunk->slots[i].~Task();
            ::operator delete(static_cast<void*>(chunk));
            chunk = previous;
        }
        chunks_ = nullptr;
        active_ = nullptr;
        freeList_ = nullptr;
    }

    SpinLock lock_{};
    Chunk* chunks_{nullptr};
    Chunk* active_{nullptr};
    Task* freeList_{nullptr};
};

// A bounded double-ended queue. The owner pushes and pops at the bottom (LIFO,
// which keeps the hottest task in cache); thieves take from the top (FIFO,
// which spreads work fairly).
//
// This uses one spin lock rather than a lock-free ring. A lock-free variant was
// tried first and had to be withdrawn: with a *bounded* ring a thief can be
// overtaken by the owner wrapping around onto the slot it is reading, which
// turns into a double-take (two consumers, one task, a counter that completes
// too early). Making that safe needs hazard pointers or a never-recycled buffer,
// and neither is worth the complexity here: an uncontended spin lock costs a
// few nanoseconds, while a double-take is a silent correctness bug.
class WorkDeque final {
public:
    WorkDeque() : slots_(kDequeCapacity, nullptr) {}

    WorkDeque(const WorkDeque&) = delete;
    WorkDeque& operator=(const WorkDeque&) = delete;

    // Returns false when the ring is full; the caller falls back to the shared
    // overflow queue.
    bool push(Task* task) noexcept {
        std::lock_guard<SpinLock> guard(lock_);
        if (bottom_ - top_ >= kDequeCapacity) return false;
        slots_[bottom_ & kMask] = task;
        ++bottom_;
        return true;
    }

    // Pushes as many tasks as fit under a single lock acquisition. Returns how
    // many were stored; the caller queues the remainder elsewhere. This is the
    // whole point of batch submission: one cache-line transaction instead of N.
    std::size_t push_batch(Task* const* tasks, std::size_t count) noexcept {
        std::lock_guard<SpinLock> guard(lock_);
        const std::size_t used = bottom_ - top_;
        const std::size_t room = used < kDequeCapacity ? kDequeCapacity - used : 0;
        const std::size_t stored = count < room ? count : room;
        for (std::size_t i = 0; i < stored; ++i) {
            slots_[(bottom_ + i) & kMask] = tasks[i];
        }
        bottom_ += stored;
        return stored;
    }

    Task* pop() noexcept {
        std::lock_guard<SpinLock> guard(lock_);
        if (bottom_ == top_) return nullptr;
        --bottom_;
        Task* task = slots_[bottom_ & kMask];
        slots_[bottom_ & kMask] = nullptr;
        return task;
    }

    Task* steal() noexcept {
        std::lock_guard<SpinLock> guard(lock_);
        if (bottom_ == top_) return nullptr;
        Task* task = slots_[top_ & kMask];
        slots_[top_ & kMask] = nullptr;
        ++top_;
        return task;
    }

    // Takes up to `capacity` tasks with one lock acquisition: at most half of
    // what the victim holds, so a thief can never strip a queue and leave the
    // other thieves empty-handed.
    std::size_t steal_batch(Task** out, std::size_t capacity) noexcept {
        std::lock_guard<SpinLock> guard(lock_);
        const std::size_t available = bottom_ - top_;
        if (available == 0) return 0;
        std::size_t take = (available + 1) / 2;
        if (take > capacity) take = capacity;
        for (std::size_t i = 0; i < take; ++i) {
            Task* task = slots_[(top_ + i) & kMask];
            slots_[(top_ + i) & kMask] = nullptr;
            out[i] = task;
        }
        top_ += take;
        return take;
    }

private:
    static constexpr std::size_t kMask = kDequeCapacity - 1;

    SpinLock lock_{};
    std::size_t top_{0};
    std::size_t bottom_{0};
    std::vector<Task*> slots_;
};

struct Worker final {
    detail::JobSystemImpl* system{nullptr};
    std::thread thread{};
    WorkDeque deque{};
    Task* freeList{nullptr};
    std::size_t freeCount{0};
    std::size_t index{0};
    // Tasks taken from a victim in one transaction, owned by this worker until
    // executed. Keeps the number of steals (and therefore the number of cache
    // lines touched on other workers' deques) down when the queue is deep.
    Task* buffer[kStealBatch]{};
    std::size_t bufferCount{0};
};

// Worker identity is thread-local and validated against the owning system so a
// thread that touches two systems simply reports "not a worker" instead of
// corrupting the wrong pool.
thread_local Worker* tls_worker = nullptr;
thread_local const void* tls_system = nullptr;

// Per-thread task magazine for producer threads that are not workers. Same
// purpose as the worker magazine: keep the shared pool lock off the hot path.
//
// The owner is identified by a *globally unique* id rather than by address: a
// destroyed system releases its pool, and a later system can easily be allocated
// at the same address, at which point an address-keyed cache would hand out
// dangling task pointers.
struct ProducerCache final {
    std::uint64_t systemId{0};
    Task* freeList{nullptr};
    std::size_t count{0};
};

thread_local ProducerCache tls_producer{};

std::atomic<std::uint64_t> g_systemSequence{0};

} // namespace

struct detail::JobSystemImpl final {
    JobSystemImpl(std::size_t requestedWorkers, JobSystem* owner);
    ~JobSystemImpl();

    JobSystemImpl(const JobSystemImpl&) = delete;
    JobSystemImpl& operator=(const JobSystemImpl&) = delete;

    using Function = JobSystem::Function;
    void submit(Function fn, Counter* done);
    void submit_batch(const Function* jobs, std::size_t count, Counter* done);
    void submit_batch(const Function& job, std::size_t count, Counter* done);
    void enqueue_batch(Task** batch, std::size_t count, Counter* done, Worker* worker);
    void wait(Counter& counter);
    std::size_t run_pending(std::size_t maxJobs);
    void wait_idle();
    std::exception_ptr take_exception() noexcept;

    Worker* current_worker() noexcept {
        return tls_system == static_cast<const void*>(this) ? tls_worker : nullptr;
    }

    Task* take_task(Worker* self);
    bool try_run_one();
    void execute(Task* task);
    Task* steal_any(Worker* self);
    std::size_t steal_batch_any(Worker* self, Task** out);
    Task* pop_overflow() noexcept;
    void push_overflow(Task* task);
    void push_overflow_batch(Task** tasks, std::size_t count);
    void wake_one();
    void wake_all();
    void record_exception(std::exception_ptr error);
    void drain_all();
    Task* acquire_task();
    void recycle_task(Task* task);
    void flush_local(Worker& worker);
    ProducerCache& producer_cache() noexcept;
    void flush_producer(ProducerCache& cache);
    void worker_main(Worker& worker);

    JobSystem* owner_{nullptr};
    // Process-unique identity. Thread-local magazines key on this instead of the
    // address so a recycled heap block can never alias a dead system.
    std::uint64_t systemId_{0};
    // Declared before workers_ so the pool outlives them (members are destroyed
    // in reverse declaration order).
    TaskPool pool_{};
    std::size_t workerCount_{0};
    std::vector<std::unique_ptr<Worker>> workers_{};

    SpinLock overflowLock_{};
    Task* overflowHead_{nullptr};

    std::mutex sleepMutex_{};
    std::condition_variable sleepCv_{};
    std::atomic<int> sleeping_{0};
    std::atomic<bool> notifyPending_{false};
    std::atomic<std::uint32_t> submitCursor_{0};
    std::atomic<int> waiters_{0};
    std::atomic<std::size_t> queued_{0};
    std::atomic<std::size_t> active_{0};
    // Tasks claimed by a worker but parked in its local steal buffer. Counted so
    // wait_idle() cannot return while a buffer still holds work.
    std::atomic<std::size_t> buffered_{0};
    std::atomic<std::uint32_t> stealCursor_{0};
    std::atomic<bool> stopping_{false};

    std::mutex exceptionMutex_{};
    std::exception_ptr exception_{};
};

detail::JobSystemImpl::JobSystemImpl(std::size_t requestedWorkers, JobSystem* owner) : owner_(owner) {
    systemId_ = g_systemSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    std::size_t count = requestedWorkers == 0 ? JobSystem::hardware_worker_count() : requestedWorkers;
    count = (std::max)(std::size_t{1}, (std::min)(count, JobSystem::kMaxWorkers));
    workerCount_ = count;

    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto worker = std::make_unique<Worker>();
        worker->system = this;
        worker->index = i;
        workers_.push_back(std::move(worker));
    }

    // If a thread fails to start, tear down the ones that did before unwinding,
    // so a construction failure cannot leak running threads.
    try {
        for (auto& worker : workers_) {
            Worker* raw = worker.get();
            worker->thread = std::thread([this, raw] { worker_main(*raw); });
        }
    } catch (...) {
        stopping_.store(true, std::memory_order_release);
        wake_all();
        for (auto& worker : workers_) {
            if (worker->thread.joinable()) worker->thread.join();
        }
        throw;
    }
}

detail::JobSystemImpl::~JobSystemImpl() {
    stopping_.store(true, std::memory_order_release);
    wake_all();
    for (auto& worker : workers_) {
        if (worker->thread.joinable()) worker->thread.join();
    }
    // Anything left (including work pushed by a job that finished late) runs
    // here, so no counter can be stranded and no task storage is lost.
    drain_all();
    workers_.clear();
}

Task* detail::JobSystemImpl::acquire_task() {
    Worker* worker = current_worker();
    if (worker != nullptr) {
        if (Task* task = worker->freeList) {
            worker->freeList = task->next;
            task->next = nullptr;
            --worker->freeCount;
            return task;
        }
        Task* batch[kTaskBatch];
        const std::size_t count = pool_.take(batch, kTaskBatch);
        for (std::size_t i = 1; i < count; ++i) {
            batch[i]->next = worker->freeList;
            worker->freeList = batch[i];
            ++worker->freeCount;
        }
        return count > 0 ? batch[0] : nullptr;
    }

    // External producer: a per-thread magazine keeps the pool lock out of the
    // per-submission path. Tasks held by a dying thread are simply not recycled
    // (the pool owns the chunks), so there is nothing to leak.
    ProducerCache& cache = producer_cache();
    if (cache.freeList != nullptr) {
        Task* task = cache.freeList;
        cache.freeList = task->next;
        task->next = nullptr;
        --cache.count;
        return task;
    }
    Task* batch[kTaskBatch];
    const std::size_t count = pool_.take(batch, kTaskBatch);
    for (std::size_t i = 1; i < count; ++i) {
        batch[i]->next = cache.freeList;
        cache.freeList = batch[i];
        ++cache.count;
    }
    return count > 0 ? batch[0] : nullptr;
}

void detail::JobSystemImpl::recycle_task(Task* task) {
    // Releasing the callable here is what keeps captured memory from piling up.
    task->fn = nullptr;
    task->counter = nullptr;
    Worker* worker = current_worker();
    if (worker == nullptr) {
        ProducerCache& cache = producer_cache();
        task->next = cache.freeList;
        cache.freeList = task;
        if (++cache.count >= kLocalFreeTarget) flush_producer(cache);
        return;
    }
    task->next = worker->freeList;
    worker->freeList = task;
    if (++worker->freeCount >= kLocalFreeTarget) flush_local(*worker);
}

ProducerCache& detail::JobSystemImpl::producer_cache() noexcept {
    if (tls_producer.systemId != systemId_) {
        // Switching systems (or first use on this thread): drop whatever the
        // previous magazine held. The chunks stay owned by their own pool, so
        // this is a missed reuse, never a leak.
        tls_producer.systemId = systemId_;
        tls_producer.freeList = nullptr;
        tls_producer.count = 0;
    }
    return tls_producer;
}

void detail::JobSystemImpl::flush_producer(ProducerCache& cache) {
    const std::size_t keep = kLocalFreeTarget / 2;
    Task* batch[kTaskBatch];
    std::size_t count = 0;
    while (cache.count > keep && count < kTaskBatch) {
        Task* task = cache.freeList;
        cache.freeList = task->next;
        task->next = nullptr;
        batch[count++] = task;
        --cache.count;
    }
    if (count > 0) pool_.give(batch, count);
}

void detail::JobSystemImpl::flush_local(Worker& worker) {
    const std::size_t keep = kLocalFreeTarget / 2;
    Task* batch[kTaskBatch];
    std::size_t count = 0;
    while (worker.freeCount > keep && count < kTaskBatch) {
        Task* task = worker.freeList;
        worker.freeList = task->next;
        task->next = nullptr;
        batch[count++] = task;
        --worker.freeCount;
    }
    if (count > 0) pool_.give(batch, count);
}

void detail::JobSystemImpl::submit(Function fn, Counter* done) {
    Task* task = acquire_task();
    task->fn = std::move(fn);
    task->counter = done;
    task->next = nullptr;

    if (done != nullptr) {
        // Attach and increment before publishing: a waiter that observes a zero
        // count must be able to trust that nothing else is coming.
        JobSystemAccess::attach(*done, owner_);
        done->add(1);
    }

    queued_.fetch_add(1, std::memory_order_relaxed);
    // Spread external submissions across worker deques (round robin). Funnelling
    // every producer into one shared queue makes all idle workers fight over a
    // single lock; handing each worker its own inbox removes that contention.
    Worker* worker = current_worker();
    bool placed = false;
    if (worker != nullptr) {
        placed = worker->deque.push(task);
    } else if (!workers_.empty()) {
        const std::size_t index =
            submitCursor_.fetch_add(1, std::memory_order_relaxed) % workers_.size();
        placed = workers_[index]->deque.push(task);
    }
    if (!placed) push_overflow(task);

    if (sleeping_.load(std::memory_order_relaxed) > 0 ||
        waiters_.load(std::memory_order_relaxed) > 0) {
        wake_one();
    }
}

void detail::JobSystemImpl::submit_batch(const Function* jobs, std::size_t count, Counter* done) {
    if (jobs == nullptr || count == 0) return;

    if (done != nullptr) {
        // Attach once for the whole batch; every unit is reserved below, before
        // the corresponding tasks are published.
        if (JobSystemAccess::owner(*done) != owner_) JobSystemAccess::attach(*done, owner_);
    }

    Worker* worker = current_worker();
    std::size_t offset = 0;
    while (offset < count) {
        const std::size_t chunk = (std::min)(kTaskBatch, count - offset);

        Task* batch[kTaskBatch];
        for (std::size_t i = 0; i < chunk; ++i) {
            Task* task = acquire_task();
            task->fn = jobs[offset + i];
            task->counter = done;
            task->next = nullptr;
            batch[i] = task;
        }
        enqueue_batch(batch, chunk, done, worker);
        offset += chunk;
    }

    if (sleeping_.load(std::memory_order_relaxed) > 0 ||
        waiters_.load(std::memory_order_relaxed) > 0) {
        wake_one();
    }
}

void detail::JobSystemImpl::submit_batch(const Function& job, std::size_t count, Counter* done) {
    if (count == 0) return;

    if (done != nullptr) {
        if (JobSystemAccess::owner(*done) != owner_) JobSystemAccess::attach(*done, owner_);
    }

    Worker* worker = current_worker();
    std::size_t offset = 0;
    while (offset < count) {
        const std::size_t chunk = (std::min)(kTaskBatch, count - offset);

        Task* batch[kTaskBatch];
        for (std::size_t i = 0; i < chunk; ++i) {
            Task* task = acquire_task();
            task->fn = job; // one callable queued `chunk` times
            task->counter = done;
            task->next = nullptr;
            batch[i] = task;
        }
        enqueue_batch(batch, chunk, done, worker);
        offset += chunk;
    }

    if (sleeping_.load(std::memory_order_relaxed) > 0 ||
        waiters_.load(std::memory_order_relaxed) > 0) {
        wake_one();
    }
}

void detail::JobSystemImpl::enqueue_batch(Task** batch, std::size_t count, Counter* done,
                                          Worker* worker) {
    if (done != nullptr) done->add(static_cast<std::int64_t>(count));
    queued_.fetch_add(count, std::memory_order_relaxed);

    // One lock acquisition for the whole chunk, on both the local and the
    // overflow path. That single transaction is what removes the per-job
    // cache-line fight with the workers.
    std::size_t stored = 0;
    if (worker != nullptr) {
        stored = worker->deque.push_batch(batch, count);
    } else if (!workers_.empty()) {
        const std::size_t target =
            submitCursor_.fetch_add(1, std::memory_order_relaxed) % workers_.size();
        stored = workers_[target]->deque.push_batch(batch, count);
    }
    if (stored < count) push_overflow_batch(batch + stored, count - stored);
}

void detail::JobSystemImpl::push_overflow(Task* task) {
    std::lock_guard<SpinLock> guard(overflowLock_);
    task->next = overflowHead_;
    overflowHead_ = task;
}

void detail::JobSystemImpl::push_overflow_batch(Task** tasks, std::size_t count) {
    std::lock_guard<SpinLock> guard(overflowLock_);
    for (std::size_t i = 0; i < count; ++i) {
        tasks[i]->next = overflowHead_;
        overflowHead_ = tasks[i];
    }
}

Task* detail::JobSystemImpl::pop_overflow() noexcept {
    std::lock_guard<SpinLock> guard(overflowLock_);
    Task* task = overflowHead_;
    if (task != nullptr) {
        overflowHead_ = task->next;
        task->next = nullptr;
    }
    return task;
}

Task* detail::JobSystemImpl::steal_any(Worker* self) {
    const std::size_t count = workers_.size();
    if (count == 0) return nullptr;
    const std::size_t start = stealCursor_.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < count; ++i) {
        Worker* victim = workers_[(start + i) % count].get();
        if (victim == self) continue;
        if (Task* task = victim->deque.steal()) return task;
    }
    return nullptr;
}

std::size_t detail::JobSystemImpl::steal_batch_any(Worker* self, Task** out) {
    const std::size_t count = workers_.size();
    if (count == 0) return 0;
    const std::size_t start = stealCursor_.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < count; ++i) {
        Worker* victim = workers_[(start + i) % count].get();
        if (victim == self) continue;
        const std::size_t stolen = victim->deque.steal_batch(out, kStealBatch);
        if (stolen > 0) return stolen;
    }
    return 0;
}

Task* detail::JobSystemImpl::take_task(Worker* self) {
    if (self == nullptr) {
        // External thread helping out: no local buffer to hold a batch in.
        if (Task* task = steal_any(nullptr)) {
            queued_.fetch_sub(1, std::memory_order_relaxed);
            return task;
        }
        if (Task* task = pop_overflow()) {
            queued_.fetch_sub(1, std::memory_order_relaxed);
            return task;
        }
        return nullptr;
    }

    if (self->bufferCount > 0) {
        buffered_.fetch_sub(1, std::memory_order_relaxed);
        return self->buffer[--self->bufferCount];
    }
    if (Task* task = self->deque.pop()) {
        queued_.fetch_sub(1, std::memory_order_relaxed);
        return task;
    }
    {
        Task* batch[kStealBatch];
        const std::size_t stolen = steal_batch_any(self, batch);
        if (stolen > 0) {
            queued_.fetch_sub(stolen, std::memory_order_relaxed);
            for (std::size_t i = 1; i < stolen; ++i) self->buffer[self->bufferCount++] = batch[i];
            if (stolen > 1) buffered_.fetch_add(stolen - 1, std::memory_order_relaxed);
            return batch[0];
        }
    }
    if (Task* task = pop_overflow()) {
        queued_.fetch_sub(1, std::memory_order_relaxed);
        return task;
    }
    return nullptr;
}

void detail::JobSystemImpl::execute(Task* task) {
    Counter* counter = task->counter;
    const bool run = counter == nullptr || !JobSystemAccess::cancelled(*counter);
    if (run) {
        try {
            task->fn();
        } catch (...) {
            record_exception(std::current_exception());
        }
    }
    recycle_task(task);
    if (counter != nullptr && JobSystemAccess::complete(*counter)) {
        // The waiter may be blocked on a counter, not on queued work.
        if (waiters_.load(std::memory_order_relaxed) > 0) wake_all();
    }
}

bool detail::JobSystemImpl::try_run_one() {
    Task* task = take_task(current_worker());
    if (task == nullptr) return false;
    active_.fetch_add(1, std::memory_order_relaxed);
    execute(task);
    active_.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

void detail::JobSystemImpl::worker_main(Worker& worker) {
    tls_worker = &worker;
    tls_system = static_cast<const void*>(this);

    for (;;) {
        // Fast path: consult the shared counter before touching any queue lock.
        // Idle workers therefore cost a single relaxed load per spin instead of
        // bouncing every deque lock and starving the producers. A non-empty local
        // buffer also counts as work: those tasks are already claimed, so the
        // shared counter may well read zero while this worker still has plenty.
        if (worker.bufferCount > 0 || queued_.load(std::memory_order_acquire) > 0) {
            if (Task* task = take_task(&worker)) {
                active_.fetch_add(1, std::memory_order_relaxed);
                execute(task);
                active_.fetch_sub(1, std::memory_order_relaxed);
                continue;
            }
        }
        if (stopping_.load(std::memory_order_acquire)) break;

        // Stay hot through a burst, then sleep. The long spin is what keeps a
        // steadily fed pool from paying a wakeup per job.
        //
        // The shared counter is sampled every kSpinSample pauses rather than on
        // every iteration: a tight loop of reads from N cores makes the
        // producer's single increment pay a cache-line transfer, which costs far
        // more than the latency the sample rate adds.
        bool foundWork = false;
        for (unsigned spin = 0; spin < kHotSpin; ++spin) {
            if ((spin & kSpinSampleMask) == 0u &&
                queued_.load(std::memory_order_relaxed) > 0) {
                foundWork = true;
                break;
            }
            cpu_relax();
        }
        if (foundWork) continue;

        sleeping_.fetch_add(1, std::memory_order_relaxed);
        // Re-arm notifications for whoever submits next.
        notifyPending_.store(false, std::memory_order_release);
        {
            std::unique_lock<std::mutex> lock(sleepMutex_);
            // A timeout makes a lost wakeup a latency blip instead of a hang.
            sleepCv_.wait_for(lock, kIdleSlice, [this] {
                return stopping_.load(std::memory_order_relaxed) ||
                       queued_.load(std::memory_order_relaxed) > 0;
            });
        }
        notifyPending_.store(false, std::memory_order_release);
        sleeping_.fetch_sub(1, std::memory_order_relaxed);
    }

    tls_worker = nullptr;
    tls_system = nullptr;
}

void detail::JobSystemImpl::wait(Counter& counter) {
    unsigned spins = 0;
    while (counter.remaining() > 0) {
        if (try_run_one()) {
            spins = 0;
            continue;
        }
        if (++spins < kSpinBudget) {
            cpu_relax();
            continue;
        }
        spins = 0;
        waiters_.fetch_add(1, std::memory_order_relaxed);
        {
            std::unique_lock<std::mutex> lock(sleepMutex_);
            sleepCv_.wait_for(lock, kIdleSlice,
                              [&counter] { return counter.remaining() <= 0; });
        }
        waiters_.fetch_sub(1, std::memory_order_relaxed);
    }
}

std::size_t detail::JobSystemImpl::run_pending(std::size_t maxJobs) {
    std::size_t ran = 0;
    while (ran < maxJobs) {
        Task* task = take_task(current_worker());
        if (task == nullptr) break;
        active_.fetch_add(1, std::memory_order_relaxed);
        execute(task);
        active_.fetch_sub(1, std::memory_order_relaxed);
        ++ran;
    }
    return ran;
}

void detail::JobSystemImpl::wait_idle() {
    unsigned spins = 0;
    while (queued_.load(std::memory_order_acquire) > 0 ||
           active_.load(std::memory_order_acquire) > 0 ||
           buffered_.load(std::memory_order_acquire) > 0) {
        if (try_run_one()) {
            spins = 0;
            continue;
        }
        if (++spins < kSpinBudget) {
            cpu_relax();
        } else {
            spins = 0;
            std::this_thread::yield();
        }
    }
}

void detail::JobSystemImpl::drain_all() {
    for (;;) {
        bool progress = false;
        for (auto& worker : workers_) {
            // Buffers first: they are the only place a task can sit that the
            // owner never reaches once it stops taking work.
            while (worker->bufferCount > 0) {
                Task* task = worker->buffer[--worker->bufferCount];
                buffered_.fetch_sub(1, std::memory_order_relaxed);
                active_.fetch_add(1, std::memory_order_relaxed);
                execute(task);
                active_.fetch_sub(1, std::memory_order_relaxed);
                progress = true;
            }
            while (Task* task = worker->deque.pop()) {
                active_.fetch_add(1, std::memory_order_relaxed);
                execute(task);
                active_.fetch_sub(1, std::memory_order_relaxed);
                progress = true;
            }
        }
        while (Task* task = pop_overflow()) {
            active_.fetch_add(1, std::memory_order_relaxed);
            execute(task);
            active_.fetch_sub(1, std::memory_order_relaxed);
            progress = true;
        }
        if (!progress) break;
    }
}

void detail::JobSystemImpl::record_exception(std::exception_ptr error) {
    if (error == nullptr) return;
    std::lock_guard<std::mutex> guard(exceptionMutex_);
    if (exception_ == nullptr) exception_ = std::move(error);
}

std::exception_ptr detail::JobSystemImpl::take_exception() noexcept {
    std::lock_guard<std::mutex> guard(exceptionMutex_);
    std::exception_ptr result = exception_;
    exception_ = nullptr;
    return result;
}

void detail::JobSystemImpl::wake_one() {
    // Coalesced: a submit storm must not turn into one kernel wakeup per job.
    // The flag is re-armed by a worker as it goes back to sleep, so a burst of
    // submissions costs at most one wakeup per sleeping worker.
    if (notifyPending_.exchange(true, std::memory_order_acq_rel)) return;
    {
        std::lock_guard<std::mutex> guard(sleepMutex_);
    }
    sleepCv_.notify_one();
}

void detail::JobSystemImpl::wake_all() {
    {
        std::lock_guard<std::mutex> guard(sleepMutex_);
    }
    sleepCv_.notify_all();
}

// --- public surface --------------------------------------------------------

JobSystem::JobSystem(std::size_t workerCount)
    : impl_(std::make_unique<detail::JobSystemImpl>(workerCount, this)) {}

JobSystem::~JobSystem() = default;

std::size_t JobSystem::worker_count() const noexcept { return impl_->workerCount_; }

std::size_t JobSystem::hardware_worker_count() noexcept {
    const unsigned reported = std::thread::hardware_concurrency();
    if (reported == 0) return 1;
    return (std::min)(static_cast<std::size_t>(reported), kMaxWorkers);
}

const JobSystem* JobSystem::worker_thread_of() const noexcept {
    return impl_ != nullptr && impl_->current_worker() != nullptr ? this : nullptr;
}

void JobSystem::submit(Function fn, Counter* done) { impl_->submit(std::move(fn), done); }

void JobSystem::submit_batch(const Function* jobs, std::size_t count, Counter* done) {
    impl_->submit_batch(jobs, count, done);
}

void JobSystem::submit_batch(const Function& job, std::size_t count, Counter* done) {
    impl_->submit_batch(job, count, done);
}

void JobSystem::wait(Counter& counter) { impl_->wait(counter); }

std::size_t JobSystem::run_pending(std::size_t maxJobs) { return impl_->run_pending(maxJobs); }

void JobSystem::wait_idle() { impl_->wait_idle(); }

std::exception_ptr JobSystem::take_exception() noexcept { return impl_->take_exception(); }

void JobSystem::parallel_for_chunk(std::size_t count, std::size_t grain,
                                   const std::function<void(std::size_t)>& fn,
                                   std::atomic<std::size_t>& next) {
    for (;;) {
        const std::size_t base = next.fetch_add(grain, std::memory_order_relaxed);
        if (base >= count) return;
        const std::size_t end = (std::min)(count, base + grain);
        for (std::size_t index = base; index < end; ++index) fn(index);
    }
}

// Claims and runs the next chunk, or returns false when the range is exhausted.
bool JobSystem::parallel_for_one_chunk(std::size_t count, std::size_t grain,
                                       const std::function<void(std::size_t)>& fn,
                                       std::atomic<std::size_t>& next) {
    const std::size_t base = next.fetch_add(grain, std::memory_order_relaxed);
    if (base >= count) return false;
    const std::size_t end = (std::min)(count, base + grain);
    for (std::size_t index = base; index < end; ++index) fn(index);
    return true;
}

void JobSystem::parallel_for_fn(std::size_t count, std::size_t grain,
                                const std::function<void(std::size_t)>& fn) {
    if (count == 0) return;
    if (grain == 0) grain = 1;
    if (grain > count) grain = count;
    const std::size_t chunks = (count + grain - 1) / grain;

    // A single chunk (or an explicit serial request) stays on the caller: no
    // counter, no tasks, no hand-off latency.
    if (chunks <= 1) {
        // Exceptions propagate straight to the caller: nothing crossed a task
        // boundary, so the system-wide error slot must stay untouched.
        for (std::size_t index = 0; index < count; ++index) fn(index);
        return;
    }

    std::atomic<std::size_t> next{0};

    // Start inline and only escalate once the work proves big enough to pay for a
    // hand-off. A 256-item call that used to spend ~1.6 us on wakeups now just
    // runs; a multi-millisecond call loses at most this budget of parallelism.
    //
    // The clock is sampled sparingly: once after the first chunk and then every
    // fourth, because a steady_clock read costs ~30 ns and a tiny range can be
    // finished well before it is worth asking the time.
    {
        const auto begin = std::chrono::steady_clock::now();
        std::size_t completed = 0;
        for (;;) {
            if (!parallel_for_one_chunk(count, grain, fn, next)) return;
            ++completed;
            if (completed != 1 && (completed & 3u) != 0u) continue;
            const auto elapsed = std::chrono::steady_clock::now() - begin;
            if (elapsed >= std::chrono::nanoseconds(kEscalateAfterNs)) break;
        }
    }

    const std::size_t helpers = (std::min)(chunks - 1, impl_->workerCount_);
    Counter counter;
    if (helpers > 0) {
        // One queue transaction for all helpers, and one callable instead of an
        // array of them: every helper runs the identical chunk loop. The old
        // version built a kMaxWorkers array of std::function just to hand the
        // same lambda over `helpers` times.
        const Function helper = [this, count, grain, &fn, &next] {
            parallel_for_chunk(count, grain, fn, next);
        };
        submit_batch(helper, helpers, &counter);
    }

    // The caller works too, which keeps latency down and guarantees progress
    // even when every worker is busy with somebody else's job.
    //
    // A throw here must not skip the wait below: the helper tasks hold references
    // to this frame's `fn`, `next` and `counter`, so leaving early would have them
    // write through a dead stack frame.
    std::exception_ptr inlineError;
    try {
        parallel_for_chunk(count, grain, fn, next);
    } catch (...) {
        inlineError = std::current_exception();
    }
    wait(counter);

    // Drains the system-wide slot so one failure is reported exactly once.
    std::exception_ptr error = take_exception();
    if (error == nullptr) error = inlineError;
    if (error != nullptr) std::rethrow_exception(error);
}

bool Counter::complete(std::int64_t count) noexcept {
    if (count <= 0) return false;
    std::int64_t current = remaining_.load(std::memory_order_acquire);
    for (;;) {
        if (current <= 0) return false;
        const std::int64_t next = current > count ? current - count : 0;
        if (remaining_.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            return next == 0;
        }
    }
}

void Counter::wait() {
    if (JobSystem* system = JobSystemAccess::owner(*this)) {
        system->wait(*this);
        return;
    }
    // Never attached to a system: the count can still be driven by hand, so a
    // plain spin with yields is the only correct behaviour left.
    while (remaining() > 0) std::this_thread::yield();
}

} // namespace shinkou::jobs
