#include "shinkou/coroutines/Coroutine.h"

#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

using namespace shinkou::coroutines;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void throws(F&& function) {
    bool caught = false;
    try { function(); } catch (const std::exception&) { caught = true; }
    require(caught, "expected an exception");
}
struct Guard { int& count; ~Guard() { ++count; } };
Task<int> child_value(int value) { co_return value; }
Task<std::unique_ptr<int>> move_value() { co_return std::make_unique<int>(42); }
Task<void> phase_task(Scheduler& scheduler, std::vector<int>& trace) {
    trace.push_back(1);
    co_await yield(Phase::LateUpdate);
    require(scheduler.frame() == 1, "late phase must stay in same frame");
    trace.push_back(2);
    co_await yield(Phase::EndOfFrame);
    trace.push_back(3);
    co_await yield(Phase::Update);
    require(scheduler.frame() == 2, "backwards phase must defer a frame");
    trace.push_back(4);
    co_await next_frame(3);
    require(scheduler.frame() == 5, "frame count mismatch");
    trace.push_back(5);
}
void phases() {
    Scheduler scheduler;
    std::vector<int> trace;
    auto handle = scheduler.spawn(phase_task(scheduler, trace));
    require(trace.empty(), "spawn must be lazy");
    scheduler.begin_frame(0, 0);
    require(scheduler.run(Phase::Update) == 1 && trace == std::vector<int>{1}, "update phase mismatch");
    scheduler.run(Phase::LateUpdate);
    require(trace == std::vector<int>({1, 2}), "late phase mismatch");
    scheduler.run(Phase::EndOfFrame);
    require(trace == std::vector<int>({1, 2, 3}), "end phase mismatch");
    throws([&] { scheduler.run(Phase::Update); });
    scheduler.tick(0);
    require(trace.back() == 4, "next frame did not resume");
    scheduler.tick(0); scheduler.tick(0);
    require(!handle.done(), "frame wait completed too early");
    scheduler.tick(0);
    require(handle.status() == Status::Completed && trace.back() == 5, "phase task did not complete");
}
Task<void> ordered_frame_wait(std::uint64_t count, int value, std::vector<int>& order) {
    co_await next_frame(count);
    order.push_back(value);
}
void frame_queue_order() {
    Scheduler scheduler({1, 1});
    std::vector<int> order;
    scheduler.spawn(ordered_frame_wait(2, 1, order));
    scheduler.tick(0);
    scheduler.spawn(ordered_frame_wait(1, 2, order));
    scheduler.tick(0);
    scheduler.tick(0); // older heap wait precedes the one-frame fast path
    require(order == std::vector<int>{1}, "frame fast path overtook an older deadline");
    scheduler.tick(0);
    require(order == std::vector<int>({1, 2}), "budgeted ready queue lost FIFO order");
    // Stale entries in both frame queues must not resume a recycled slot.
    auto cancelled = scheduler.spawn(ordered_frame_wait(1, 3, order));
    scheduler.tick(0);
    cancelled.cancel();
    scheduler.tick(0);
    scheduler.spawn(ordered_frame_wait(1, 4, order));
    scheduler.tick(0); scheduler.tick(0);
    require(order == std::vector<int>({1, 2, 4}), "stale next-frame ticket resumed a recycled root");
}
Task<void> timer_task(Clock clock, int& completed) { co_await sleep_for(1, clock); ++completed; }
Task<void> zero_delay(int& completed) { co_await sleep_for(0); ++completed; }
void timers() {
    Scheduler scheduler;
    int scaled = 0, unscaled = 0, zero = 0;
    auto a = scheduler.spawn(timer_task(Clock::Scaled, scaled));
    auto b = scheduler.spawn(timer_task(Clock::Unscaled, unscaled));
    scheduler.spawn(zero_delay(zero));
    scheduler.tick(0, 0);
    require(zero == 0, "zero sleep must yield");
    scheduler.tick(0, .5);
    require(zero == 1 && scaled == 0 && unscaled == 0, "paused timer advanced");
    scheduler.tick(0, .5);
    require(b.done() && !a.done() && unscaled == 1, "unscaled clock must progress while paused");
    scheduler.tick(.5, .5); scheduler.tick(.5, .5);
    require(a.done() && scaled == 1, "scaled timer did not finish");
    require(scheduler.stats().timers == 0, "finished timer retained");
    throws([&] { scheduler.tick(-1); });
    throws([&] { scheduler.tick(std::numeric_limits<double>::quiet_NaN()); });
}
Task<void> value_parent(int& result) {
    result = co_await child_value(20);
    result += *(co_await move_value());
}
Task<void> fail_child() { throw std::runtime_error("child failure"); co_return; }
Task<void> catch_child(bool& caught) {
    try { co_await fail_child(); } catch (const std::runtime_error&) { caught = true; }
}
Task<void> fail_parent() { co_await fail_child(); }
Task<void> nested_wait(int& destroyed) {
    Guard guard{destroyed};
    co_await sleep_for(100000);
}
Task<void> nested_parent(int& destroyed) {
    Guard guard{destroyed};
    co_await nested_wait(destroyed);
}
void values_and_errors() {
    Scheduler scheduler;
    int result = 0;
    bool caught = false;
    auto value = scheduler.spawn(value_parent(result));
    scheduler.spawn(catch_child(caught));
    auto failed = scheduler.spawn(fail_parent());
    scheduler.tick(0);
    require(value.done() && result == 62 && caught, "child result or exception propagation failed");
    require(failed.status() == Status::Failed && scheduler.stats().failed == 1, "root failure lost");
    throws([&] { failed.rethrow_if_failed(); });
    require(static_cast<bool>(scheduler.take_exception()), "unobserved error channel is empty");
    require(!scheduler.take_exception(), "error channel must clear on read");
    int destroyed = 0;
    auto cancelled = scheduler.spawn(nested_parent(destroyed));
    scheduler.tick(0);
    cancelled.cancel();
    scheduler.tick(0);
    require(cancelled.status() == Status::Cancelled && destroyed == 2, "nested cancellation did not destroy all frames");
    throws([&] { cancelled.rethrow_if_failed(); });
}
Task<void> event_wait(Event event, int& value) { co_await event; ++value; }
void events_and_generations() {
    Scheduler scheduler;
    Event event;
    int value = 0;
    auto first = scheduler.spawn(event_wait(event, value));
    scheduler.tick(0);
    first.cancel();
    scheduler.tick(0);
    auto replacement = scheduler.spawn(event_wait(event, value));
    // A late signal for the old generation must never resume the new frame twice.
    std::thread signal([event] { event.set(); }); signal.join();
    scheduler.tick(0);
    require(first.status() == Status::Cancelled && replacement.done() && value == 1, "generation or signal-before-wait failed");
    event.reset();
    std::vector<TaskHandle> waiters;
    for (int i = 0; i < 1000; ++i) waiters.push_back(scheduler.spawn(event_wait(event, value)));
    scheduler.tick(0);
    event.set(); event.reset(); // Already registered waiters retain this notification.
    scheduler.tick(0);
    require(value == 1001 && scheduler.stats().active == 0, "event fan-out/reset lost notifications");
    Scheduler other;
    Event shared;
    for (int i = 0; i < 100; ++i) {
        scheduler.spawn(event_wait(shared, value));
        other.spawn(event_wait(shared, value));
        scheduler.tick(0);
        other.tick(0);
    }
    // Alternating inbox destinations exercise broadcast lock transitions.
    shared.set();
    scheduler.tick(0); other.tick(0);
    require(value == 1201 && !scheduler.stats().active && !other.stats().active,
            "shared event lost notifications between different schedulers");
}
Task<void> join_task(TaskHandle task, bool& finished) { co_await task; finished = true; }
Task<void> timeout_task(TaskHandle task, double timeout, bool& completed) { completed = co_await wait_for(task, timeout); }
Task<void> self_join(TaskHandle& self) { co_await self; }
Task<void> any_task(std::vector<TaskHandle> handles, std::size_t& winner) {
    winner = co_await when_any(std::move(handles));
}
void joins_and_timeouts() {
    Scheduler scheduler;
    Event event;
    int count = 0;
    auto target = scheduler.spawn(event_wait(event, count));
    bool joined = false, timed = true, success = false;
    auto join = scheduler.spawn(join_task(target, joined));
    auto timeout = scheduler.spawn(timeout_task(target, 1, timed));
    auto wins = scheduler.spawn(timeout_task(target, 100, success));
    scheduler.tick(0);
    scheduler.tick(1);
    require(timeout.done() && !timed && !target.done(), "timeout must not cancel target");
    event.set();
    scheduler.tick(0); scheduler.tick(0);
    require(join.done() && joined && wins.done() && success && count == 1, "join completion lost");
    TaskHandle self;
    self = scheduler.spawn(self_join(self));
    scheduler.tick(0);
    require(self.status() == Status::Failed, "self join must fail rather than hang");
    // Joining a failed root must propagate through a waiting root.
    bool unused = false;
    auto failedJoin = scheduler.spawn(join_task(self, unused));
    scheduler.tick(0);
    require(failedJoin.status() == Status::Failed && !unused, "join swallowed failure");
    Event a, b;
    auto aTask = scheduler.spawn(event_wait(a, count));
    auto bTask = scheduler.spawn(event_wait(b, count));
    std::size_t winner = 99;
    auto any = scheduler.spawn(any_task({aTask, bTask}, winner));
    scheduler.tick(0);
    b.set(); scheduler.tick(0); scheduler.tick(0);
    require(any.done() && winner == 1 && !aTask.done(), "when_any cancelled or selected the wrong task");
    auto all = scheduler.spawn(when_all({self, aTask}));
    scheduler.tick(0);
    require(!all.done(), "when_all must drain all children after a failure");
    a.set(); scheduler.tick(0); scheduler.tick(0);
    require(all.status() == Status::Failed, "when_all did not propagate the first failure");
    bool condition = false;
    auto predicate = scheduler.spawn(wait_until([&] { return condition; }));
    scheduler.tick(0);
    require(!predicate.done(), "predicate wait completed early");
    condition = true; scheduler.tick(0);
    require(predicate.done(), "predicate wait did not finish");
}
Task<void> count_task(int& count) { ++count; co_return; }
Task<void> spawn_children(Scheduler& scheduler, int& count) {
    std::vector<TaskHandle> children;
    for (int i = 0; i < 2000; ++i) children.push_back(scheduler.spawn(count_task(count)));
    co_await when_all(std::move(children));
}
void budgets_groups_and_growth() {
    Scheduler scheduler({1, 7});
    int count = 0;
    auto parent = scheduler.spawn(spawn_children(scheduler, count));
    scheduler.tick(0);
    require(count == 0, "same-phase spawn must defer to next run");
    for (int i = 0; i < 400 && !parent.done(); ++i) scheduler.tick(0);
    require(count == 2000 && parent.done(), "slot growth or bounded progress failed");
    require(scheduler.stats().budgetExhaustions > 0, "phase budget not enforced");
    Event event;
    TaskHandle grouped;
    {
        TaskGroup group(scheduler);
        grouped = group.spawn(event_wait(event, count));
        scheduler.tick(0);
        require(!group.done(), "pending group reported completion");
    }
    scheduler.tick(0);
    require(grouped.status() == Status::Cancelled, "scope did not request cancellation");
    // Keep one root alive so this exercises compaction rather than clear-on-idle.
    auto keeper = scheduler.spawn(event_wait(event, count));
    scheduler.tick(0);
    // Cancelled long waits must not accumulate an unbounded tombstone heap.
    for (int batch = 0; batch < 8; ++batch) {
        std::vector<TaskHandle> handles;
        for (int i = 0; i < 200; ++i) handles.push_back(scheduler.spawn(timer_task(Clock::Scaled, count)));
        for (int i = 0; i < 30; ++i) scheduler.tick(0);
        for (auto& handle : handles) handle.cancel();
        scheduler.tick(0);
        require(scheduler.stats().timers <= 258, "cancelled timer heap grew without a bound");
    }
    keeper.cancel(); scheduler.tick(0);
    require(scheduler.stats().timers == 0, "idle timer heap did not clear");
}
Task<void> racing_wait(Event event, int& completed, int& destroyed) {
    Guard guard{destroyed};
    co_await event;
    ++completed;
}
void concurrent_notifications() {
    Scheduler scheduler;
    for (int iteration = 0; iteration < 100; ++iteration) {
        Event event;
        int completed = 0, destroyed = 0;
        std::vector<TaskHandle> handles;
        for (int i = 0; i < 64; ++i) handles.push_back(scheduler.spawn(racing_wait(event, completed, destroyed)));
        scheduler.tick(0);
        std::thread setter([event] { event.set(); });
        std::thread canceller([handles] { for (const auto& handle : handles) handle.cancel(); });
        scheduler.tick(0);
        setter.join(); canceller.join();
        scheduler.tick(0);
        require(destroyed == 64, "notification race leaked or double-destroyed a frame");
        int successful = 0;
        for (const auto& handle : handles) {
            require(handle.done(), "notification race left a suspended frame");
            if (handle.status() == Status::Completed) ++successful;
        }
        require(successful == completed, "notification race resumed a task more than once");
        require(scheduler.stats().active == 0, "notification race retained a root");
    }
}
Task<void> background_task(shinkou::jobs::JobSystem& jobs, int& result, std::thread::id owner) {
    auto value = co_await background(jobs, [payload = std::make_unique<int>(42)]() mutable { return std::move(payload); });
    require(std::this_thread::get_id() == owner, "worker resumed coroutine on wrong thread");
    result = *value;
    bool caught = false;
    try { co_await background(jobs, [] { throw std::runtime_error("worker failure"); }); }
    catch (const std::runtime_error&) { caught = true; }
    require(caught, "background exception lost");
}
Task<void> pending_background(shinkou::jobs::JobSystem& jobs, std::atomic<bool>& release, int& completed) {
    co_await background(jobs, [&release] {
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    ++completed;
}
void background_and_shutdown() {
    shinkou::jobs::JobSystem jobs(1);
    Scheduler scheduler;
    int result = 0;
    auto handle = scheduler.spawn(background_task(jobs, result, std::this_thread::get_id()));
    for (int i = 0; i < 10 && !handle.done(); ++i) { scheduler.tick(0); jobs.wait_idle(); }
    handle.rethrow_if_failed();
    require(result == 42, "move-only worker result failed");
    std::atomic<bool> release{false};
    int completed = 0;
    {
        Scheduler dying;
        auto pending = dying.spawn(pending_background(jobs, release, completed));
        dying.tick(0);
        std::thread cancel([pending] { pending.cancel(); }); cancel.join();
        dying.shutdown();
        require(pending.status() == Status::Cancelled, "shutdown did not cancel active task");
        throws([&] { dying.spawn(count_task(completed)); });
    }
    release.store(true, std::memory_order_release);
    jobs.wait_idle();
    require(completed == 0, "background completion resumed a destroyed scheduler");
    std::atomic<bool> rejected{false};
    std::thread other([&] {
        try { scheduler.tick(0); } catch (const std::logic_error&) { rejected = true; }
    });
    other.join();
    require(rejected.load(), "scheduler accepted a non-owner thread");
}
Task<void> invalid_delay() { co_await sleep_for(-1); }
Task<void> bad_reentry(Scheduler& scheduler) { scheduler.tick(0); co_return; }
void validation() {
    Scheduler scheduler;
    throws([&] { scheduler.spawn(Task<void>{}); });
    throws([&] { scheduler.run(Phase::Update); });
    auto delay = scheduler.spawn(invalid_delay());
    auto reentry = scheduler.spawn(bad_reentry(scheduler));
    scheduler.tick(0);
    require(delay.status() == Status::Failed && reentry.status() == Status::Failed, "invalid waits/reentry must become root errors");
    int destroyed = 0;
    auto waiting = scheduler.spawn(nested_wait(destroyed));
    // This root has not started: cancellation must not execute its body.
    waiting.cancel();
    scheduler.tick(0);
    require(waiting.status() == Status::Cancelled && destroyed == 0, "pre-start cancellation executed task body");
}
}
int main() {
    try {
        phases(); frame_queue_order(); timers(); values_and_errors(); events_and_generations();
        joins_and_timeouts(); budgets_groups_and_growth(); concurrent_notifications(); background_and_shutdown(); validation();
        std::cout << "coroutine contracts passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
