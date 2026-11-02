#include "shinkou/log/Log.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
using Timer = std::chrono::steady_clock;
volatile unsigned source = 17;
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
unsigned expensive_argument() {
    auto result = source;
    for (unsigned i = 0; i < 100; ++i) result = (result ^ i) * 1664525u + 1013904223u;
    return result;
}
double elapsed(Timer::time_point start, std::size_t count) {
    return std::chrono::duration<double, std::nano>(Timer::now() - start).count() / count;
}
}
int main() {
    constexpr std::size_t count = 200000;
    shinkou::log::LogConfig config;
    config.console = config.file = false;
    config.asynchronous = false;
    config.flushInterval = std::chrono::milliseconds::zero();
    if (!shinkou::log::initialize(config)) return 1;
    auto start = Timer::now();
    for (std::size_t i = 0; i < count; ++i) SHINKOU_LOG_DEBUG("disabled {}", i);
    std::printf("filtered: %.2f ns/call\n", elapsed(start, count));
    start = Timer::now();
    for (std::size_t i = 0; i < count; ++i) SHINKOU_LOG_DEBUG("disabled expensive {}", expensive_argument());
    std::printf("filtered expensive argument: %.2f ns/call\n", elapsed(start, count));
    start = Timer::now();
    for (std::size_t i = 0; i < count; ++i) SHINKOU_LOG_INFO("message {}", i);
    std::printf("sync null sink: %.2f ns/call\n", elapsed(start, count));
    shinkou::log::shutdown();

    config.asynchronous = true;
    config.overflowPolicy = shinkou::log::OverflowPolicy::Block;
    if (!shinkou::log::initialize(config)) return 2;
    start = Timer::now();
    for (std::size_t i = 0; i < count; ++i) SHINKOU_LOG_INFO("message {}", i);
    shinkou::log::shutdown();
    std::printf("async block null sink including drain: %.2f ns/message\n", elapsed(start, count));
    if (!shinkou::log::initialize(config)) return 3;
    start = Timer::now();
    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) producers.emplace_back([p] {
        for (std::size_t i = 0; i < count / 4; ++i) SHINKOU_LOG_INFO("producer {} message {}", p, i);
    });
    for (auto& producer : producers) producer.join();
    shinkou::log::shutdown();
    std::printf("async four producers including drain: %.2f ns/message\n", elapsed(start, count));
    return 0;
}
