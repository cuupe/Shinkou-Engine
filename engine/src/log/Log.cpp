#include "shinkou/log/Log.h"

#if defined(SHINKOU_WITH_SPDLOG) && SHINKOU_WITH_SPDLOG
#include <spdlog/async.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <system_error>
#include <vector>

namespace shinkou::log {
namespace {
struct State {
    std::mutex mutex;
    std::shared_ptr<spdlog::logger> logger;
    std::shared_ptr<spdlog::details::thread_pool> threadPool;
    std::atomic<spdlog::logger*> rawLogger{nullptr};
    LogConfig config{};
    std::filesystem::path filePath{};
    std::string lastError{};
    std::atomic<bool> initialized{false};
    bool ownsThreadPool{false};
};

State& state() { static State value; return value; }

spdlog::level::level_enum to_spdlog_level(Level value) noexcept {
    switch (value) {
    case Level::Trace: return spdlog::level::trace;
    case Level::Debug: return spdlog::level::debug;
    case Level::Info: return spdlog::level::info;
    case Level::Warn: return spdlog::level::warn;
    case Level::Error: return spdlog::level::err;
    case Level::Critical: return spdlog::level::critical;
    case Level::Off: return spdlog::level::off;
    }
    return spdlog::level::info;
}

Level from_spdlog_level(spdlog::level::level_enum value) noexcept {
    switch (value) {
    case spdlog::level::trace: return Level::Trace;
    case spdlog::level::debug: return Level::Debug;
    case spdlog::level::info: return Level::Info;
    case spdlog::level::warn: return Level::Warn;
    case spdlog::level::err: return Level::Error;
    case spdlog::level::critical: return Level::Critical;
    case spdlog::level::off: return Level::Off;
    default: return Level::Info;
    }
}

void on_spdlog_error(const std::string& message) {
    auto& current = state();
    std::unique_lock<std::mutex> lock(current.mutex, std::try_to_lock);
    if (lock.owns_lock()) current.lastError = message;
}
}

namespace detail {
spdlog::logger* raw_logger() noexcept { return state().rawLogger.load(std::memory_order_acquire); }
}

bool initialize(const LogConfig& config) {
    auto& current = state();
    std::lock_guard<std::mutex> lock(current.mutex);
    if (current.initialized.load(std::memory_order_acquire)) return true;
    current.config = config;
    current.lastError.clear();
    current.filePath.clear();
    try {
        std::vector<spdlog::sink_ptr> sinks;
        sinks.reserve(2);
        if (config.console) sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        if (config.file) {
            if (config.fileName.empty()) { current.lastError = "log file name is empty"; return false; }
            std::error_code error;
            std::filesystem::create_directories(config.directory, error);
            if (error) { current.lastError = "cannot create log directory: " + error.message(); return false; }
            current.filePath = config.directory / config.fileName;
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                current.filePath.string(), std::max<std::size_t>(config.maxFileSize, 1024u),
                std::max<std::size_t>(config.maxFiles, 1u), true));
        }
        if (sinks.empty()) sinks.push_back(std::make_shared<spdlog::sinks::null_sink_mt>());

        if (config.asynchronous) {
            current.threadPool = spdlog::thread_pool();
            if (!current.threadPool) {
                spdlog::init_thread_pool(std::max<std::size_t>(config.queueSize, 256u),
                    std::max<std::size_t>(config.workerThreads, 1u));
                current.threadPool = spdlog::thread_pool();
                current.ownsThreadPool = true;
            }
            if (!current.threadPool) { current.lastError = "unable to initialize spdlog thread pool"; return false; }
            const auto overflow = config.overflowPolicy == OverflowPolicy::Block
                ? spdlog::async_overflow_policy::block : spdlog::async_overflow_policy::overrun_oldest;
            current.logger = std::make_shared<spdlog::async_logger>(
                config.loggerName, sinks.begin(), sinks.end(), current.threadPool, overflow);
        } else {
            current.logger = std::make_shared<spdlog::logger>(config.loggerName, sinks.begin(), sinks.end());
        }
        current.logger->set_level(to_spdlog_level(config.level));
        current.logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] [thread %t] %v");
        if (config.flushOnError) current.logger->flush_on(spdlog::level::err);
        spdlog::register_or_replace(current.logger);
        spdlog::set_default_logger(current.logger);
        spdlog::set_error_handler(on_spdlog_error);
        if (config.flushInterval > std::chrono::milliseconds::zero()) spdlog::flush_every(config.flushInterval);
        current.rawLogger.store(current.logger.get(), std::memory_order_release);
        current.initialized.store(true, std::memory_order_release);
        current.logger->info("logging initialized; async={}, queue={}, file={}", config.asynchronous,
            config.queueSize, current.filePath.empty() ? std::string{"disabled"} : current.filePath.string());
        return true;
    } catch (const std::exception& exception) {
        current.lastError = exception.what();
    } catch (...) { current.lastError = "unknown logging initialization failure"; }
    current.rawLogger.store(nullptr, std::memory_order_release);
    current.logger.reset();
    current.threadPool.reset();
    return false;
}

void shutdown() noexcept {
    auto& current = state();
    std::lock_guard<std::mutex> lock(current.mutex);
    if (!current.initialized.load(std::memory_order_acquire)) return;
    try {
        current.rawLogger.store(nullptr, std::memory_order_release);
        if (current.logger) current.logger->flush();
        if (!current.config.loggerName.empty()) spdlog::drop(current.config.loggerName);
        current.logger.reset();
        current.threadPool.reset();
        if (current.ownsThreadPool) spdlog::shutdown();
    } catch (...) { }
    current.initialized.store(false, std::memory_order_release);
    current.ownsThreadPool = false;
}

void flush() noexcept {
    auto* logger = detail::raw_logger();
    if (logger == nullptr) return;
    try { logger->flush(); } catch (...) { }
}
bool initialized() noexcept { return state().initialized.load(std::memory_order_acquire); }
void set_level(Level value) noexcept {
    auto* logger = detail::raw_logger();
    if (logger != nullptr) try { logger->set_level(to_spdlog_level(value)); } catch (...) { }
}
Level level() noexcept {
    auto* logger = detail::raw_logger();
    return logger == nullptr ? Level::Off : from_spdlog_level(logger->level());
}
LogStats stats() {
    auto& current = state();
    std::lock_guard<std::mutex> lock(current.mutex);
    LogStats result;
    result.initialized = current.initialized;
    result.queueCapacity = current.config.asynchronous
        ? std::max<std::size_t>(current.config.queueSize, 256u) : 0;
    result.filePath = current.filePath;
    result.lastError = current.lastError;
    if (current.threadPool) result.queueOverruns = current.threadPool->overrun_counter();
    return result;
}
}
#else
namespace shinkou::log {
bool initialize(const LogConfig&) { return true; }
void shutdown() noexcept {}
void flush() noexcept {}
bool initialized() noexcept { return false; }
void set_level(Level) noexcept {}
Level level() noexcept { return Level::Off; }
LogStats stats() { return {}; }
}
#endif
