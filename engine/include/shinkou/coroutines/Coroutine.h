#pragma once

#include "shinkou/coroutines/Scheduler.h"
#include "shinkou/jobs/JobSystem.h"
#include <chrono>
#include <coroutine>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace shinkou::coroutines {
namespace detail {
struct Wake {
    std::uint32_t slot{0};
    std::uint64_t generation{0};
    std::uint64_t epoch{0};
};
struct Inbox;
struct WaitNode;
struct Notification {
    std::weak_ptr<Inbox> inbox;
    Wake wake;
};
class Subscription final {
  public:
    Subscription();
    ~Subscription();
    Subscription(Subscription &&other) noexcept;
    Subscription &operator=(Subscription &&other) noexcept;
    Subscription(const Subscription &) = delete;
    Subscription &operator=(const Subscription &) = delete;
    std::shared_ptr<WaitState> state;
    std::unique_ptr<WaitNode> node;
};
std::shared_ptr<WaitState> make_wait_state();
bool is_set(const std::shared_ptr<WaitState> &state);
void signal(const std::shared_ptr<WaitState> &state);
void reset(const std::shared_ptr<WaitState> &state);
Subscription subscribe(const std::shared_ptr<WaitState> &state, Notification notification);

struct PromiseBase {
    Scheduler *scheduler{nullptr};
    Wake root{};
    std::coroutine_handle<> continuation{};
    std::exception_ptr exception{};
    std::suspend_always initial_suspend() const noexcept {
        return {};
    }
    struct Final {
        bool await_ready() const noexcept {
            return false;
        }
        template <class P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> handle) const noexcept {
            auto continuation = handle.promise().continuation;
            return continuation ? continuation : std::noop_coroutine();
        }
        void await_resume() const noexcept {
        }
    };
    Final final_suspend() const noexcept {
        return {};
    }
    void unhandled_exception() noexcept {
        exception = std::current_exception();
    }
};

struct Access {
    static Notification suspend(PromiseBase &promise, std::coroutine_handle<> leaf, Phase phase);
    static void frames(PromiseBase &, std::coroutine_handle<>, std::uint64_t, Phase);
    static void delay(PromiseBase &, std::coroutine_handle<>, double, Clock, Phase);
    static void timer(PromiseBase &, Notification, double, Clock, Phase);
    static std::shared_ptr<WaitState> completion(const TaskHandle &);
    static void check_join(const PromiseBase &, const TaskHandle &);
};

template <class T> struct TaskPromise : PromiseBase {
    static_assert(!std::is_reference_v<T>, "Task results must be values");
    std::optional<T> value;
    Task<T> get_return_object() noexcept;
    template <class U> void return_value(U &&result) {
        value.emplace(std::forward<U>(result));
    }
};
template <> struct TaskPromise<void> : PromiseBase {
    Task<void> get_return_object() noexcept;
    void return_void() const noexcept {
    }
};
} // namespace detail

// Move-only, lazy, single-consumer task. Nested co_await uses symmetric transfer;
// no queue allocation or worker handoff is needed for synchronous child tasks.
template <class T> class [[nodiscard]] Task final {
  public:
    using promise_type = detail::TaskPromise<T>;
    using Handle = std::coroutine_handle<promise_type>;
    Task() = default;
    explicit Task(Handle handle) noexcept : handle_(handle) {
    }
    ~Task() {
        if (handle_)
            handle_.destroy();
    }
    Task(Task &&other) noexcept : handle_(std::exchange(other.handle_, {})) {
    }
    Task &operator=(Task &&other) noexcept {
        if (this != &other) {
            if (handle_)
                handle_.destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;
    struct Awaiter {
        Handle handle;
        explicit Awaiter(Handle value) noexcept : handle(value) {
        }
        Awaiter(Awaiter &&other) noexcept : handle(std::exchange(other.handle, {})) {
        }
        Awaiter(const Awaiter &) = delete;
        ~Awaiter() {
            if (handle)
                handle.destroy();
        }
        bool await_ready() const noexcept {
            return !handle || handle.done();
        }
        template <class P> std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent) {
            static_assert(std::is_base_of_v<detail::PromiseBase, P>);
            auto &promise = handle.promise();
            promise.scheduler = parent.promise().scheduler;
            promise.root = parent.promise().root;
            promise.continuation = parent;
            return handle;
        }
        T await_resume() {
            if (!handle)
                throw std::logic_error("cannot await an empty task");
            auto &promise = handle.promise();
            if (promise.exception)
                std::rethrow_exception(promise.exception);
            if constexpr (!std::is_void_v<T>)
                return std::move(*promise.value);
        }
    };
    Awaiter operator co_await() && noexcept {
        return Awaiter{std::exchange(handle_, {})};
    }

  private:
    Handle handle_{};
    friend class Scheduler;
};
namespace detail {
template <class T> Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}
inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}
} // namespace detail

struct FrameAwaiter {
    std::uint64_t count;
    Phase phase;
    bool await_ready() const noexcept {
        return false;
    }
    template <class P> void await_suspend(std::coroutine_handle<P> handle) const {
        detail::Access::frames(handle.promise(), handle, count, phase);
    }
    void await_resume() const noexcept {
    }
};
inline FrameAwaiter next_frame(std::uint64_t count = 1, Phase phase = Phase::Update) {
    return {count, phase};
}
// A later phase can resume in this frame. An earlier/equal phase yields to the
// following frame, so an accidental yield loop cannot monopolize one run().
inline FrameAwaiter yield(Phase phase = Phase::Update) {
    return {0, phase};
}

struct DelayAwaiter {
    double seconds;
    Clock clock;
    Phase phase;
    bool await_ready() const noexcept {
        return false;
    }
    template <class P> void await_suspend(std::coroutine_handle<P> handle) const {
        detail::Access::delay(handle.promise(), handle, seconds, clock, phase);
    }
    void await_resume() const noexcept {
    }
};
inline DelayAwaiter sleep_for(double seconds, Clock clock = Clock::Scaled,
                              Phase phase = Phase::Update) {
    return {seconds, clock, phase};
}
template <class Rep, class Period>
DelayAwaiter sleep_for(std::chrono::duration<Rep, Period> duration, Clock clock = Clock::Scaled,
                       Phase phase = Phase::Update) {
    return sleep_for(std::chrono::duration<double>(duration).count(), clock, phase);
}

struct CompletionAwaiter {
    TaskHandle task;
    detail::Subscription subscription;
    bool await_ready() const noexcept {
        return task.done();
    }
    template <class P> void await_suspend(std::coroutine_handle<P> handle) {
        detail::Access::check_join(handle.promise(), task);
        auto wake = detail::Access::suspend(handle.promise(), handle, Phase::Update);
        subscription = detail::subscribe(detail::Access::completion(task), wake);
    }
    void await_resume() const {
        task.rethrow_if_failed();
    }
};
inline CompletionAwaiter operator co_await(TaskHandle task) {
    return {std::move(task), {}};
}

// Thread-safe manual-reset event. signal-before-wait and multiple waiters are
// supported. Cancelling a waiter unregisters it immediately on frame destruction.
class Event final {
  public:
    explicit Event(bool initiallySet = false) : state_(detail::make_wait_state()) {
        if (initiallySet)
            set();
    }
    void set() const {
        detail::signal(state_);
    }
    void reset() const {
        detail::reset(state_);
    }
    bool is_set() const {
        return detail::is_set(state_);
    }
    struct Awaiter {
        std::shared_ptr<detail::WaitState> state;
        detail::Subscription subscription;
        bool await_ready() const {
            return detail::is_set(state);
        }
        template <class P> void await_suspend(std::coroutine_handle<P> handle) {
            subscription = detail::subscribe(
                state, detail::Access::suspend(handle.promise(), handle, Phase::Update));
        }
        void await_resume() const noexcept {
        }
    };
    Awaiter operator co_await() const {
        return {state_, {}};
    }

  private:
    std::shared_ptr<detail::WaitState> state_;
};

struct TimeoutAwaiter {
    TaskHandle task;
    double seconds;
    Clock clock;
    detail::Subscription subscription;
    bool await_ready() const noexcept {
        return task.done();
    }
    template <class P> void await_suspend(std::coroutine_handle<P> handle) {
        detail::Access::check_join(handle.promise(), task);
        auto wake = detail::Access::suspend(handle.promise(), handle, Phase::Update);
        detail::Access::timer(handle.promise(), wake, seconds, clock, Phase::Update);
        subscription = detail::subscribe(detail::Access::completion(task), wake);
    }
    // Timeout does not cancel the observed task; its owner controls its lifetime.
    bool await_resume() const {
        if (!task.done())
            return false;
        task.rethrow_if_failed();
        return true;
    }
};
inline TimeoutAwaiter wait_for(TaskHandle task, double seconds, Clock clock = Clock::Unscaled) {
    return {std::move(task), seconds, clock, {}};
}

namespace detail {
template <class R, class F> struct BackgroundState {
    explicit BackgroundState(F&& value) : function(std::move(value)) {}
    F function;
    std::shared_ptr<WaitState> completion{make_wait_state()};
    std::optional<R> value;
    std::exception_ptr exception;
};
template <class F> struct BackgroundState<void, F> {
    explicit BackgroundState(F&& value) : function(std::move(value)) {}
    F function;
    std::shared_ptr<WaitState> completion{make_wait_state()};
    std::exception_ptr exception;
};
} // namespace detail
template <class F> class BackgroundAwaiter {
    using Result = std::invoke_result_t<F &>;
    static_assert(!std::is_reference_v<Result>, "background results must be values");
    jobs::JobSystem *jobs_;
    F function_;
    std::shared_ptr<detail::BackgroundState<Result, F>> state_;
    detail::Subscription subscription_;

  public:
    BackgroundAwaiter(jobs::JobSystem &jobs, F function)
        : jobs_(&jobs), function_(std::move(function)) {
    }
    bool await_ready() const noexcept {
        return false;
    }
    template <class P> void await_suspend(std::coroutine_handle<P> handle) {
        state_ = std::make_shared<detail::BackgroundState<Result, F>>(std::move(function_));
        subscription_ = detail::subscribe(
            state_->completion, detail::Access::suspend(handle.promise(), handle, Phase::Update));
        // The job owns all of its data; it never captures a coroutine frame.
        // One state boxes the move-only callable and its result for JobSystem.
        jobs_->submit([state = state_] {
            try {
                if constexpr (std::is_void_v<Result>)
                    std::invoke(state->function);
                else
                    state->value.emplace(std::invoke(state->function));
            } catch (...) {
                state->exception = std::current_exception();
            }
            detail::signal(state->completion);
        });
    }
    Result await_resume() {
        // Acquire the signal mutex to publish the worker's value/exception.
        (void)detail::is_set(state_->completion);
        if (state_->exception)
            std::rethrow_exception(state_->exception);
        if constexpr (!std::is_void_v<Result>)
            return std::move(*state_->value);
    }
};
template <class F> auto background(jobs::JobSystem &jobs, F &&function) {
    return BackgroundAwaiter<std::decay_t<F>>{jobs, std::forward<F>(function)};
}

// All roots have already been started, so waiting in sequence preserves
// concurrency. Drain every child before propagating the first failure.
inline Task<void> when_all(std::vector<TaskHandle> tasks) {
    std::exception_ptr failure;
    for (auto &task : tasks) {
        try {
            co_await task;
        } catch (...) {
            if (!failure)
                failure = std::current_exception();
        }
    }
    if (failure)
        std::rethrow_exception(failure);
}

struct AnyAwaiter {
    std::vector<TaskHandle> tasks;
    std::vector<detail::Subscription> subscriptions;
    bool await_ready() const {
        if (tasks.empty())
            throw std::invalid_argument("when_any requires at least one task");
        for (const auto &task : tasks) {
            if (!task.valid())
                throw std::invalid_argument("when_any received an empty handle");
        }
        for (const auto &task : tasks)
            if (task.done())
                return true;
        return false;
    }
    template <class P> void await_suspend(std::coroutine_handle<P> handle) {
        subscriptions.reserve(tasks.size());
        for (const auto &task : tasks)
            detail::Access::check_join(handle.promise(), task);
        auto wake = detail::Access::suspend(handle.promise(), handle, Phase::Update);
        for (const auto &task : tasks)
            subscriptions.push_back(detail::subscribe(detail::Access::completion(task), wake));
    }
    // Return the lowest completed index when several complete before the owner
    // pumps. Inspect that handle to propagate its error; other roots keep running.
    std::size_t await_resume() const {
        for (std::size_t i = 0; i < tasks.size(); ++i)
            if (tasks[i].done())
                return i;
        throw std::logic_error("when_any resumed without a completion");
    }
};
inline AnyAwaiter when_any(std::vector<TaskHandle> tasks) {
    return {std::move(tasks), {}};
}

template <class Predicate> Task<void> wait_until(Predicate predicate, Phase phase = Phase::Update) {
    while (!std::invoke(predicate))
        co_await next_frame(1, phase);
}
} // namespace shinkou::coroutines
