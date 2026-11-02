#include "shinkou/coroutines/Coroutine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>

namespace shinkou::coroutines {
namespace detail {
struct Inbox {
    std::mutex mutex;
    bool closed{false};
    std::vector<Wake> pending;
};
static void post(const Notification& notification) {
    if (auto inbox = notification.inbox.lock()) {
        std::lock_guard<std::mutex> lock(inbox->mutex);
        if (!inbox->closed) inbox->pending.push_back(notification.wake);
    }
}
struct WaitNode {
    WaitNode* previous{nullptr};
    WaitNode* next{nullptr};
    Notification notification;
    bool linked{false};
};
struct WaitState {
    std::mutex mutex;
    bool signaled{false};
    WaitNode* head{nullptr};
    WaitNode* tail{nullptr};
    void unlink(WaitNode& node) noexcept {
        if (!node.linked) return;
        if (node.previous) node.previous->next = node.next;
        else head = node.next;
        if (node.next) node.next->previous = node.previous;
        else tail = node.previous;
        node.previous = node.next = nullptr;
        node.linked = false;
    }
};
std::shared_ptr<WaitState> make_wait_state() { return std::make_shared<WaitState>(); }
bool is_set(const std::shared_ptr<WaitState>& state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->signaled;
}
void signal(const std::shared_ptr<WaitState>& state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->signaled = true;
    // Lock order is WaitState -> Inbox. The owner always releases Inbox before
    // destroying subscriptions. Adjacent waiters on one owner share one lock.
    std::shared_ptr<Inbox> target;
    std::unique_lock<std::mutex> inboxLock;
    while (auto* node = state->head) {
        auto nextTarget = node->notification.inbox.lock();
        if (nextTarget != target) {
            inboxLock = {};
            target = std::move(nextTarget);
            if (target) inboxLock = std::unique_lock<std::mutex>(target->mutex);
        }
        if (target && !target->closed) target->pending.push_back(node->notification.wake);
        state->unlink(*node);
    }
}
void reset(const std::shared_ptr<WaitState>& state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->signaled = false;
}
Subscription subscribe(const std::shared_ptr<WaitState>& state, Notification notification) {
    Subscription subscription;
    subscription.state = state;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->signaled) {
            subscription.node = std::make_unique<WaitNode>();
            auto& node = *subscription.node;
            node.notification = std::move(notification);
            node.previous = state->tail;
            if (state->tail) state->tail->next = &node;
            else state->head = &node;
            state->tail = &node;
            node.linked = true;
            return subscription;
        }
    }
    post(notification);
    return subscription;
}
Subscription::Subscription() = default;
Subscription::~Subscription() {
    if (state && node) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->unlink(*node);
    }
}
Subscription::Subscription(Subscription&& other) noexcept
    : state(std::move(other.state)), node(std::move(other.node)) {}
Subscription& Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        Subscription previous(std::move(*this));
        state = std::move(other.state);
        node = std::move(other.node);
    }
    return *this;
}

struct Control {
    std::atomic<Status> status{Status::Pending};
    std::atomic<bool> cancelRequested{false};
    std::exception_ptr exception;
    std::mutex completionMutex;
    std::shared_ptr<WaitState> completion;
    Notification cancellation;
};

struct Slot {
    std::uint64_t generation{0};
    std::uint64_t epoch{0};
    std::coroutine_handle<> root{};
    std::coroutine_handle<> leaf{};
    PromiseBase* promise{nullptr};
    std::shared_ptr<Control> control;
    Phase phase{Phase::Update};
    bool queued{false};
};
template<class Key> struct Timed {
    Key deadline{};
    std::uint64_t sequence{0};
    Wake wake{};
    bool operator<(const Timed& other) const noexcept {
        return deadline > other.deadline || (deadline == other.deadline && sequence > other.sequence);
    }
};
// Unlike deque, this retains its storage after the consumer drains a phase.
// A steady next-frame workload therefore does not allocate/free queue blocks.
struct ReadyQueue {
    std::vector<Wake> entries;
    std::size_t head{0};
    std::size_t size() const noexcept { return entries.size() - head; }
    void push_back(Wake wake) { entries.push_back(wake); }
    Wake front() const noexcept { return entries[head]; }
    void pop_front() noexcept {
        if (++head == entries.size()) clear();
    }
    void clear() noexcept { entries.clear(); head = 0; }
    void compact() {
        if (head && head >= entries.size() / 2) {
            entries.erase(entries.begin(), entries.begin() + head);
            head = 0;
        }
    }
};
struct SchedulerImpl {
    SchedulerConfig config;
    std::thread::id owner{std::this_thread::get_id()};
    std::shared_ptr<Inbox> inbox{std::make_shared<Inbox>()};
    std::vector<Wake> messages;
    std::vector<Slot> slots;
    std::vector<std::uint32_t> freeSlots;
    std::array<ReadyQueue, 3> ready;
    std::vector<Wake> nextFrame;
    std::vector<Wake> dueFrame;
    std::vector<Timed<std::uint64_t>> frames;
    std::array<std::vector<Timed<double>>, 2> timers;
    std::array<double, 2> times{};
    std::uint64_t frame{0};
    std::uint64_t sequence{0};
    int phase{-1};
    bool running{false};
    bool clearing{false};
    bool closed{false};
    SchedulerStats stats;
    std::exception_ptr exception;

    explicit SchedulerImpl(SchedulerConfig value) : config(value) {
        if (!config.maxResumesPerPhase) throw std::invalid_argument("coroutine phase budget must be positive");
        slots.reserve(config.initialCapacity);
        freeSlots.reserve(config.initialCapacity);
        frames.reserve(config.initialCapacity);
        nextFrame.reserve(config.initialCapacity);
        dueFrame.reserve(config.initialCapacity);
        for (auto& queue : ready) queue.entries.reserve(config.initialCapacity);
        for (auto& heap : timers) heap.reserve(config.initialCapacity);
    }
    void check_owner() const {
        if (owner != std::this_thread::get_id()) throw std::logic_error("coroutine scheduler requires its owner thread");
    }
    void check_idle() const {
        check_owner();
        if (running) throw std::logic_error("coroutine scheduler cannot be pumped or cleared recursively");
    }
    Slot* find(Wake wake) {
        if (wake.slot >= slots.size()) return nullptr;
        auto& slot = slots[wake.slot];
        return slot.root && slot.generation == wake.generation ? &slot : nullptr;
    }
    bool current(Wake wake) {
        auto* slot = find(wake);
        return slot && slot->epoch == wake.epoch;
    }
    static std::size_t phase_index(Phase value) {
        auto index = static_cast<std::size_t>(value);
        if (index >= 3) throw std::invalid_argument("invalid coroutine phase");
        return index;
    }
    static std::size_t clock_index(Clock value) {
        auto index = static_cast<std::size_t>(value);
        if (index >= 2) throw std::invalid_argument("invalid coroutine clock");
        return index;
    }
    void enqueue(Wake wake) {
        auto* slot = find(wake);
        if (!slot || slot->epoch != wake.epoch || slot->queued) return;
        ready[phase_index(slot->phase)].push_back(wake);
        slot->queued = true;
    }
    template<class Key> void promote(std::vector<Timed<Key>>& heap, Key now) {
        while (!heap.empty() && heap.front().deadline <= now) {
            std::pop_heap(heap.begin(), heap.end());
            auto wake = heap.back().wake;
            heap.pop_back();
            enqueue(wake);
        }
    }
    template<class Key> void compact(std::vector<Timed<Key>>& heap) {
        // Stale timeouts carry no pointers. Bound their retained storage even
        // when long sleeps are cancelled or events beat long timeout deadlines.
        if (heap.size() <= stats.active * 2 + 256) return;
        heap.erase(std::remove_if(heap.begin(), heap.end(), [this](const auto& entry) {
            return !current(entry.wake);
        }), heap.end());
        std::make_heap(heap.begin(), heap.end());
    }
    void collect_stale() {
        if (!stats.active) {
            nextFrame.clear();
            dueFrame.clear();
            frames.clear();
            for (auto& heap : timers) heap.clear();
            for (auto& queue : ready) queue.clear();
        } else {
            if (nextFrame.size() > stats.active * 2 + 256)
                nextFrame.erase(std::remove_if(nextFrame.begin(), nextFrame.end(),
                    [this](Wake wake) { return !current(wake); }), nextFrame.end());
            compact(frames);
            for (auto& heap : timers) compact(heap);
        }
    }
    void finish(std::uint32_t index, Status status) {
        auto& slot = slots[index];
        auto control = slot.control;
        auto root = slot.root;
        if (status == Status::Failed) {
            control->exception = slot.promise->exception;
            if (!exception) exception = control->exception;
            ++stats.failed;
        } else if (status == Status::Cancelled) ++stats.cancelled;
        else ++stats.completed;
        slot.root = {};
        slot.leaf = {};
        slot.promise = nullptr;
        slot.control.reset();
        slot.queued = false;
        --stats.active;
        // Run RAII cleanup before publishing completion to joiners.
        root.destroy();
        freeSlots.push_back(index);
        std::shared_ptr<WaitState> completion;
        {
            std::lock_guard<std::mutex> lock(control->completionMutex);
            control->status.store(status, std::memory_order_release);
            completion = control->completion;
        }
        if (completion) signal(completion);
    }
    void drain() {
        messages.clear();
        {
            std::lock_guard<std::mutex> lock(inbox->mutex);
            messages.swap(inbox->pending);
        }
        for (auto wake : messages) {
            auto* slot = find(wake);
            if (!slot) continue;
            if (slot->control->cancelRequested.load(std::memory_order_acquire)) finish(wake.slot, Status::Cancelled);
            else if (wake.epoch) enqueue(wake);
        }
    }
};

Notification Access::suspend(PromiseBase& promise, std::coroutine_handle<> leaf, Phase phase) {
    if (!promise.scheduler) throw std::logic_error("task must be spawned or awaited by a scheduled task");
    auto& impl = *promise.scheduler->impl_;
    impl.check_owner();
    SchedulerImpl::phase_index(phase);
    auto* slot = impl.find(promise.root);
    if (!slot) throw std::logic_error("coroutine root is no longer alive");
    slot->leaf = leaf;
    slot->phase = phase;
    slot->queued = false;
    ++slot->epoch;
    return {impl.inbox, {promise.root.slot, promise.root.generation, slot->epoch}};
}
void Access::frames(PromiseBase& promise, std::coroutine_handle<> leaf, std::uint64_t count, Phase phase) {
    auto notification = suspend(promise, leaf, phase);
    auto& impl = *promise.scheduler->impl_;
    if (!count && static_cast<int>(phase) > impl.phase) {
        impl.enqueue(notification.wake);
        return;
    }
    count = (std::max)(count, std::uint64_t{1});
    if (count > (std::numeric_limits<std::uint64_t>::max)() - impl.frame)
        throw std::overflow_error("coroutine frame deadline overflow");
    if (count == 1) {
        impl.nextFrame.push_back(notification.wake);
        return;
    }
    impl.frames.push_back({impl.frame + count, impl.sequence++, notification.wake});
    std::push_heap(impl.frames.begin(), impl.frames.end());
}
void Access::timer(PromiseBase& promise, Notification notification, double seconds, Clock clock, Phase phase) {
    if (!std::isfinite(seconds) || seconds < 0) throw std::invalid_argument("coroutine delay must be finite and nonnegative");
    auto& impl = *promise.scheduler->impl_;
    auto index = SchedulerImpl::clock_index(clock);
    const auto deadline = impl.times[index] + seconds;
    if (!std::isfinite(deadline)) throw std::overflow_error("coroutine time deadline overflow");
    (void)phase;
    auto& heap = impl.timers[index];
    heap.push_back({deadline, impl.sequence++, notification.wake});
    std::push_heap(heap.begin(), heap.end());
}
void Access::delay(PromiseBase& promise, std::coroutine_handle<> leaf, double seconds, Clock clock, Phase phase) {
    SchedulerImpl::clock_index(clock);
    if (seconds == 0) { frames(promise, leaf, 1, phase); return; }
    auto notification = suspend(promise, leaf, phase);
    timer(promise, notification, seconds, clock, phase);
}
std::shared_ptr<WaitState> Access::completion(const TaskHandle& task) {
    if (!task.control_) throw std::logic_error("cannot await an empty task handle");
    std::lock_guard<std::mutex> lock(task.control_->completionMutex);
    if (!task.control_->completion) {
        task.control_->completion = make_wait_state();
        if (task.control_->status.load(std::memory_order_acquire) != Status::Pending)
            signal(task.control_->completion);
    }
    return task.control_->completion;
}
void Access::check_join(const PromiseBase& promise, const TaskHandle& task) {
    if (!task.control_) throw std::logic_error("cannot await an empty task handle");
    if (promise.scheduler && task.control_->cancellation.inbox.lock() == promise.scheduler->impl_->inbox &&
        task.control_->cancellation.wake.slot == promise.root.slot &&
        task.control_->cancellation.wake.generation == promise.root.generation)
        throw std::logic_error("a coroutine cannot join its own root");
}
} // namespace detail

bool TaskHandle::done() const noexcept { return status() != Status::Pending; }
Status TaskHandle::status() const noexcept {
    return control_ ? control_->status.load(std::memory_order_acquire) : Status::Completed;
}
void TaskHandle::cancel() const {
    if (control_ && !done() && !control_->cancelRequested.exchange(true, std::memory_order_acq_rel))
        detail::post(control_->cancellation);
}
void TaskHandle::rethrow_if_failed() const {
    if (!control_) throw std::logic_error("empty coroutine task handle");
    auto value = status();
    if (value == Status::Pending) throw std::logic_error("coroutine task has not completed");
    if (value == Status::Cancelled) throw Cancelled{};
    if (value == Status::Failed) std::rethrow_exception(control_->exception);
}

Scheduler::Scheduler(const SchedulerConfig& config) : impl_(std::make_unique<detail::SchedulerImpl>(config)) {}
Scheduler::~Scheduler() { shutdown(); }
TaskHandle Scheduler::spawn(Task<void> task) {
    auto& impl = *impl_;
    impl.check_owner();
    if (impl.closed) throw std::logic_error("coroutine scheduler is closed");
    if (impl.clearing) throw std::logic_error("cannot spawn while coroutine frames are being cleared");
    if (!task.handle_) throw std::invalid_argument("cannot spawn an empty task");
    auto control = std::make_shared<detail::Control>();
    std::uint32_t index;
    if (impl.freeSlots.empty()) {
        if (impl.slots.size() >= (std::numeric_limits<std::uint32_t>::max)())
            throw std::length_error("too many coroutine slots");
        index = static_cast<std::uint32_t>(impl.slots.size());
        // Reserve recycle storage before ownership is transferred.
        if (impl.freeSlots.capacity() < impl.slots.size() + 1)
            impl.freeSlots.reserve((std::max)(impl.slots.size() + 1, impl.slots.capacity() * 2));
        impl.slots.emplace_back();
    } else { index = impl.freeSlots.back(); impl.freeSlots.pop_back(); }
    auto& slot = impl.slots[index];
    ++slot.generation;
    slot.epoch = 1;
    slot.phase = Phase::Update;
    slot.control = control;
    control->cancellation = {impl.inbox, {index, slot.generation, 0}};
    try { impl.ready[0].push_back({index, slot.generation, slot.epoch}); }
    catch (...) { slot.control.reset(); impl.freeSlots.push_back(index); throw; }
    slot.root = slot.leaf = std::exchange(task.handle_, {});
    slot.promise = &std::coroutine_handle<Task<void>::promise_type>::from_address(slot.root.address()).promise();
    slot.promise->scheduler = this;
    slot.promise->root = {index, slot.generation, 0};
    slot.queued = true;
    ++impl.stats.active;
    ++impl.stats.spawned;
    impl.stats.peakActive = (std::max)(impl.stats.active, impl.stats.peakActive);
    return TaskHandle{std::move(control)};
}
void Scheduler::begin_frame(double scaledDelta, double unscaledDelta) {
    auto& impl = *impl_;
    impl.check_idle();
    if (impl.closed) throw std::logic_error("coroutine scheduler is closed");
    if (!std::isfinite(scaledDelta) || !std::isfinite(unscaledDelta) || scaledDelta < 0 || unscaledDelta < 0)
        throw std::invalid_argument("coroutine frame deltas must be finite and nonnegative");
    if (!std::isfinite(impl.times[0] + scaledDelta) || !std::isfinite(impl.times[1] + unscaledDelta) ||
        impl.frame == (std::numeric_limits<std::uint64_t>::max)())
        throw std::overflow_error("coroutine clock overflow");
    ++impl.frame;
    impl.phase = -1;
    impl.times[0] += scaledDelta;
    impl.times[1] += unscaledDelta;
    impl.promote(impl.frames, impl.frame);
    impl.nextFrame.swap(impl.dueFrame);
    for (auto wake : impl.dueFrame) impl.enqueue(wake);
    impl.dueFrame.clear();
    for (std::size_t i = 0; i < 2; ++i) impl.promote(impl.timers[i], impl.times[i]);
}
std::size_t Scheduler::run(Phase phase, std::size_t budget) {
    auto& impl = *impl_;
    impl.check_idle();
    if (impl.closed) return 0;
    const auto index = detail::SchedulerImpl::phase_index(phase);
    if (!impl.frame || static_cast<int>(phase) < impl.phase) throw std::logic_error("coroutine phases must run in frame order");
    impl.phase = static_cast<int>(phase);
    struct Guard { bool& flag; ~Guard() { flag = false; } } guard{impl.running};
    impl.running = true;
    impl.drain();
    auto& queue = impl.ready[index];
    const auto available = queue.size();
    const auto limit = (std::min)(available, budget ? budget : impl.config.maxResumesPerPhase);
    std::size_t resumed = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        auto wake = queue.front();
        queue.pop_front();
        auto* slot = impl.find(wake);
        if (!slot || slot->epoch != wake.epoch || !slot->queued) continue;
        slot->queued = false;
        if (slot->control->cancelRequested.load(std::memory_order_acquire)) {
            impl.finish(wake.slot, Status::Cancelled);
            continue;
        }
        // Invalidate duplicate event/timeout notifications before resuming.
        ++slot->epoch;
        auto leaf = slot->leaf;
        leaf.resume();
        ++resumed;
        ++impl.stats.resumes;
        // spawn() inside the body may have grown slots, so reacquire the slot.
        slot = impl.find(wake);
        if (slot && slot->root.done())
            impl.finish(wake.slot, slot->promise->exception ? Status::Failed : Status::Completed);
    }
    if (available > limit) ++impl.stats.budgetExhaustions;
    queue.compact();
    impl.collect_stale();
    return resumed;
}
void Scheduler::tick(double delta) { tick(delta, delta); }
void Scheduler::tick(double scaledDelta, double unscaledDelta) {
    begin_frame(scaledDelta, unscaledDelta);
    run(Phase::Update);
    run(Phase::LateUpdate);
    run(Phase::EndOfFrame);
}
void Scheduler::cancel_all() {
    auto& impl = *impl_;
    impl.check_idle();
    struct Guard {
        detail::SchedulerImpl& impl;
        ~Guard() { impl.running = false; impl.clearing = false; }
    } guard{impl};
    impl.running = impl.clearing = true;
    for (std::uint32_t i = 0; i < impl.slots.size(); ++i)
        if (impl.slots[i].root) impl.finish(i, Status::Cancelled);
    impl.collect_stale();
}
void Scheduler::shutdown() {
    auto& impl = *impl_;
    impl.check_idle();
    if (impl.closed) return;
    impl.closed = true;
    {
        std::lock_guard<std::mutex> lock(impl.inbox->mutex);
        impl.inbox->closed = true;
        impl.inbox->pending.clear();
    }
    cancel_all();
}
bool Scheduler::closed() const { impl_->check_owner(); return impl_->closed; }
std::uint64_t Scheduler::frame() const { impl_->check_owner(); return impl_->frame; }
double Scheduler::time(Clock clock) const { impl_->check_owner(); return impl_->times[detail::SchedulerImpl::clock_index(clock)]; }
SchedulerStats Scheduler::stats() const {
    impl_->check_owner();
    auto result = impl_->stats;
    for (const auto& queue : impl_->ready) result.ready += queue.size();
    result.timers = impl_->nextFrame.size() + impl_->frames.size() + impl_->timers[0].size() + impl_->timers[1].size();
    return result;
}
std::exception_ptr Scheduler::take_exception() {
    impl_->check_owner();
    return std::exchange(impl_->exception, {});
}
TaskGroup::TaskGroup(Scheduler& scheduler) : scheduler_(&scheduler) {}
TaskGroup::~TaskGroup() { cancel(); }
TaskHandle TaskGroup::spawn(Task<void> task) {
    auto handle = scheduler_->spawn(std::move(task));
    try { handles_.push_back(handle); }
    catch (...) { handle.cancel(); throw; }
    return handle;
}
void TaskGroup::cancel() { for (auto& handle : handles_) handle.cancel(); }
bool TaskGroup::done() const {
    return std::all_of(handles_.begin(), handles_.end(), [](const auto& handle) { return handle.done(); });
}
} // namespace shinkou::coroutines
