#include "shinkou/log/Log.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <optional>
#include <condition_variable>
#include <mutex>
#include <spdlog/async.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/base_sink.h>

namespace {
class BlockingSink final : public spdlog::sinks::base_sink<std::mutex> {
    std::mutex gate_;
    std::condition_variable wake_;
    bool released_{false};
    void sink_it_(const spdlog::details::log_msg& message) override {
        entered.store(true, std::memory_order_release);
        std::unique_lock<std::mutex> lock(gate_);
        wake_.wait(lock, [&] { return released_; });
        ++received;
        last.assign(message.payload.data(), message.payload.size());
    }
    void flush_() override {}
public:
    std::atomic<bool> entered{false};
    std::size_t received{0};
    std::string last;
    void release() {
        std::lock_guard<std::mutex> lock(gate_);
        released_ = true;
        wake_.notify_all();
    }
};
class ThrowingSink final : public spdlog::sinks::base_sink<std::mutex> {
    void sink_it_(const spdlog::details::log_msg&) override { throw std::runtime_error("intentional sink failure"); }
    void flush_() override {}
};
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void advanced_tests(const std::filesystem::path& directory) {
    namespace log = shinkou::log;
    log::LogConfig config;
    config.console = config.file = false;
    config.asynchronous = false;
    config.flushInterval = std::chrono::milliseconds::zero();
    assert(log::initialize(config));
    int evaluated = 0;
    SHINKOU_LOG_DEBUG("filtered {}", ++evaluated);
    assert(evaluated == 0);
    log::set_level(log::Level::Debug);
    SHINKOU_LOG_DEBUG("enabled {}", ++evaluated);
    assert(evaluated == 1);
    log::set_level(log::Level::Off);
    SHINKOU_LOG_ERROR("off {}", ++evaluated);
    log::shutdown();
    SHINKOU_LOG_ERROR("closed {}", ++evaluated);
    assert(evaluated == 1);

    // No work is queued at level Off: installing the test sink is race-free.
    config.level = log::Level::Off;
    config.asynchronous = true;
    config.queueSize = 256;
    auto blocked = std::make_shared<BlockingSink>();
    assert(log::initialize(config));
    {
        auto lease = log::detail::acquire_logger();
        lease.logger->sinks().push_back(blocked);
    }
    log::set_level(log::Level::Info);
    SHINKOU_LOG_INFO("hold worker");
    while (!blocked->entered.load(std::memory_order_acquire)) std::this_thread::yield();
    for (int i = 0; i < 1000; ++i) SHINKOU_LOG_INFO("overrun {}", i);
    assert(log::stats().queueDepth == 256 && log::stats().queueOverruns == 744);
    blocked->release();
    log::shutdown();
    assert(log::stats().queueOverruns == 744 && blocked->received == 257);
    assert(blocked->last == "overrun 999");
    config.level = log::Level::Info;
    config.asynchronous = false;
    assert(log::initialize(config));
    {
        auto lease = log::detail::acquire_logger();
        lease.logger->sinks().push_back(std::make_shared<ThrowingSink>());
    }
    SHINKOU_LOG_INFO("report sink failure without throwing");
    assert(log::stats().errors > 0 && log::stats().lastError.find("intentional sink failure") != std::string::npos);
    log::shutdown();

    assert(log::initialize(config));
    std::optional<log::detail::LoggerLease> outer;
    outer.emplace(log::detail::acquire_logger());
    assert(outer->logger);
    std::atomic<bool> closed{false};
    std::thread closer([&] { log::shutdown(); closed.store(true, std::memory_order_release); });
    while (log::initialized()) std::this_thread::yield();
    {
        auto nested = log::detail::acquire_logger();
        assert(nested.logger == outer->logger);
        nested.logger->info("protected nested lease during shutdown");
    }
    assert(!closed.load(std::memory_order_acquire));
    outer.reset();
    closer.join();
    assert(closed.load(std::memory_order_acquire));

    // Engine logging must not replace or shut down another spdlog user's pool,
    // default logger, named logger or process-wide error handler.
    spdlog::init_thread_pool(64, 1);
    auto foreignPool = spdlog::thread_pool();
    auto foreignDefault = spdlog::default_logger();
    auto foreignSink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto foreign = std::make_shared<spdlog::async_logger>("foreign", foreignSink, foreignPool);
    spdlog::register_logger(foreign);
    config.asynchronous = true;
    config.queueSize = 256;
    config.workerThreads = 4;
    config.overflowPolicy = log::OverflowPolicy::Block;
    config.file = true;
    config.directory = directory;
    config.fileName = "parallel.log";
    assert(log::initialize(config));
    assert(log::stats().queueCapacity == 256 && log::stats().workerThreads == 4);
    assert(spdlog::thread_pool() == foreignPool && spdlog::default_logger() == foreignDefault);
    std::vector<std::thread> producers;
    for (int producer = 0; producer < 4; ++producer) producers.emplace_back([producer] {
        for (int index = 0; index < 2000; ++index)
            SHINKOU_LOG_INFO("accepted producer={} index={}", producer, index);
    });
    for (auto& producer : producers) producer.join();
    log::shutdown();
    const auto output = read(directory / "parallel.log");
    for (int producer = 0; producer < 4; ++producer)
        for (int index = 0; index < 2000; ++index)
            assert(output.find("accepted producer=" + std::to_string(producer) + " index=" + std::to_string(index) + "\n") != std::string::npos);
    assert(log::stats().queueOverruns == 0 && log::stats().errors == 0);
    assert(spdlog::thread_pool() == foreignPool && spdlog::get("foreign") == foreign);
    foreign->info("still usable after engine shutdown");
    foreign->flush();
    spdlog::drop("foreign");

    // Restart appends by default instead of rotating on every initialization.
    assert(log::initialize(config));
    SHINKOU_LOG_ERROR("session restart marker");
    log::shutdown();
    auto appended = read(directory / "parallel.log");
    assert(appended.find("accepted producer=0 index=0\n") != std::string::npos);
    assert(appended.find("session restart marker") != std::string::npos);
    config.asynchronous = false;
    config.fileName = "rotation.log";
    config.maxFileSize = 1024;
    config.maxFiles = 2;
    assert(log::initialize(config));
    for (int index = 0; index < 100; ++index) SHINKOU_LOG_INFO("rotation {} {}", index, std::string(150, 'x'));
    SHINKOU_LOG_ERROR("last rotation marker");
    log::shutdown();
    assert(std::filesystem::exists(directory / "rotation.1.log"));
    assert(std::filesystem::exists(directory / "rotation.2.log"));
    assert(read(directory / "rotation.log").find("last rotation marker") != std::string::npos);

    config.file = config.console = false;
    config.asynchronous = true;
    config.flushInterval = std::chrono::milliseconds(1);
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> attempts{0};
    producers.clear();
    for (int index = 0; index < 4; ++index) producers.emplace_back([&] {
        while (!stopping.load(std::memory_order_acquire)) {
            SHINKOU_LOG_INFO("lifecycle {}", attempts.fetch_add(1));
            log::set_level(log::Level::Info);
            log::flush();
            (void)log::level();
        }
    });
    for (int cycle = 0; cycle < 40; ++cycle) {
        config.asynchronous = (cycle % 2) != 0;
        assert(log::initialize(config));
        for (int index = 0; index < 100; ++index) SHINKOU_LOG_INFO("owner {}", index);
        log::shutdown();
        assert(!log::initialized());
    }
    stopping.store(true, std::memory_order_release);
    for (auto& producer : producers) producer.join();
    assert(log::level() == log::Level::Off);
    const auto slotsBefore = log::stats().readerSlots;
    assert(log::initialize(config));
    for (int thread = 0; thread < 32; ++thread) {
        std::thread temporary([] { SHINKOU_LOG_INFO("temporary producer"); });
        temporary.join();
    }
    log::shutdown();
    assert(log::stats().readerSlots == slotsBefore);
    config.flushInterval = std::chrono::milliseconds(-1);
    assert(!log::initialize(config));
    assert(!log::stats().lastError.empty() && log::stats().errors > 0);
    config.flushInterval = std::chrono::milliseconds::zero();
    assert(log::initialize(config));
    assert(log::stats().errors == 0);
    log::shutdown();
}
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("shinkou-engine-logging-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);

    shinkou::log::LogConfig config;
    config.directory = directory;
    config.fileName = "test.log";
    config.console = false;
    config.asynchronous = false;
    config.flushInterval = std::chrono::milliseconds::zero();
    config.level = shinkou::log::Level::Trace;
    if (!shinkou::log::initialize(config)) {
        std::cerr << "Logging initialization failed: " << shinkou::log::stats().lastError << '\n';
        return 1;
    }
    assert(shinkou::log::initialized());
    SHINKOU_LOG_INFO("logging test value={}", 42);
    SHINKOU_LOG_ERROR("logging test error");
    shinkou::log::flush();

    const auto stats = shinkou::log::stats();
    assert(stats.initialized);
    assert(stats.filePath == directory / "test.log");
    std::ifstream input(stats.filePath);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    assert(contents.find("logging test value=42") != std::string::npos);
    assert(contents.find("logging test error") != std::string::npos);

    shinkou::log::shutdown();
    assert(!shinkou::log::initialized());
    input.close();

    // Exercise the default asynchronous path. Shutdown must drain queued
    // messages so a normal short-lived engine process retains its diagnostics.
    config = {};
    config.directory = directory;
    config.console = false;
    assert(config.asynchronous);
    if (!shinkou::log::initialize(config)) {
        std::cerr << "Async logging initialization failed: " << shinkou::log::stats().lastError << '\n';
        return 1;
    }
    assert(shinkou::log::initialized());
    assert(shinkou::log::stats().queueCapacity == config.queueSize);
    SHINKOU_LOG_DEBUG("filtered debug message");
    for (int index = 0; index < 128; ++index)
        SHINKOU_LOG_INFO("async default message={}", index);
    SHINKOU_LOG_ERROR("async default error");
    const auto asyncPath = shinkou::log::stats().filePath;
    assert(asyncPath == directory / "Engine.log");
    shinkou::log::shutdown();
    assert(!shinkou::log::initialized());
    std::ifstream asyncInput(asyncPath);
    const std::string asyncContents((std::istreambuf_iterator<char>(asyncInput)), std::istreambuf_iterator<char>());
    assert(asyncContents.find("filtered debug message") == std::string::npos);
    for (int index = 0; index < 128; ++index)
        assert(asyncContents.find("async default message=" + std::to_string(index) + "\n") != std::string::npos);
    assert(asyncContents.find("async default error") != std::string::npos);
    asyncInput.close();
    advanced_tests(directory);
    std::filesystem::remove_all(directory, error);
    return 0;
}
