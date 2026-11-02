#include "shinkou/coroutines/Coroutine.h"
#include <chrono>
#include <cstdio>
#include <vector>

using namespace shinkou::coroutines;
namespace {
using Timer = std::chrono::steady_clock;
Task<void> yielding(std::uint64_t &operations, int frames) {
    for (int i = 0; i < frames; ++i) {
        ++operations;
        co_await next_frame();
    }
}
Task<void> sleeping() {
    co_await sleep_for(100000);
}
Task<void> immediate(std::uint64_t &count) {
    ++count;
    co_return;
}
Task<void> event_waiter(Event event, std::uint64_t& count) {
    co_await event;
    ++count;
}
Task<int> value() {
    co_return 42;
}
Task<void> nesting(std::uint64_t &count, int iterations) {
    for (int i = 0; i < iterations; ++i)
        count += co_await value();
}
double milliseconds(Timer::time_point start) {
    return std::chrono::duration<double, std::milli>(Timer::now() - start).count();
}
} // namespace
int main() {
    constexpr int tasks = 10000, frames = 100;
    Scheduler scheduler({tasks, tasks * 2});
    std::uint64_t operations = 0;
    auto start = Timer::now();
    for (int i = 0; i < tasks; ++i)
        scheduler.spawn(immediate(operations));
    scheduler.tick(0);
    const auto spawnMs = milliseconds(start);
    if (operations != tasks || scheduler.stats().active)
        return 1;
    std::printf("spawn+complete: %d roots, %.3f ms, %.1f ns/root\n", tasks, spawnMs,
                spawnMs * 1e6 / tasks);

    operations = 0;
    for (int i = 0; i < tasks; ++i)
        scheduler.spawn(yielding(operations, frames));
    scheduler.tick(0); // Warm the timer heap before timing steady-state yields.
    start = Timer::now();
    for (int frame = 1; frame < frames; ++frame)
        scheduler.tick(0);
    const auto yieldMs = milliseconds(start);
    if (operations != static_cast<std::uint64_t>(tasks) * frames)
        return 2;
    std::printf("frame wait: %llu resumes, %.3f ms, %.1f ns/resume\n",
                static_cast<unsigned long long>(tasks) * (frames - 1), yieldMs,
                yieldMs * 1e6 / (tasks * (frames - 1)));
    scheduler.tick(0);

    for (int i = 0; i < tasks * 10; ++i)
        scheduler.spawn(sleeping());
    // Start all sleepers before measuring idle ticks.
    for (int i = 0; i < 5; ++i)
        scheduler.tick(0);
    const auto before = scheduler.stats().resumes;
    start = Timer::now();
    for (int i = 0; i < 10000; ++i)
        scheduler.tick(0);
    const auto idleMs = milliseconds(start);
    if (scheduler.stats().resumes != before)
        return 3;
    std::printf("100000 sleeping roots: 10000 idle ticks, %.3f ms, %.1f ns/tick\n", idleMs,
                idleMs * 100);
    scheduler.cancel_all();

    operations = 0;
    scheduler.spawn(nesting(operations, 100000));
    start = Timer::now();
    scheduler.tick(0);
    const auto nestedMs = milliseconds(start);
    if (operations != 4200000 || scheduler.stats().active)
        return 4;
    std::printf("synchronous child task: 100000 awaits, %.3f ms, %.1f ns/await\n", nestedMs,
                nestedMs * 10);
    Event event;
    operations = 0;
    for (int i = 0; i < tasks; ++i) scheduler.spawn(event_waiter(event, operations));
    scheduler.tick(0);
    start = Timer::now();
    event.set();
    scheduler.tick(0);
    const auto eventMs = milliseconds(start);
    if (operations != tasks || scheduler.stats().active) return 5;
    std::printf("event broadcast+complete: %d waiters, %.3f ms, %.1f ns/waiter\n", tasks, eventMs, eventMs * 1e6 / tasks);
    return 0;
}
