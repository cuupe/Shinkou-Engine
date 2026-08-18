#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>

#if defined(SHINKOU_WITH_SPDLOG) && SHINKOU_WITH_SPDLOG
#include <spdlog/spdlog.h>
#endif

namespace shinkou::log {
enum class Level { Trace, Debug, Info, Warn, Error, Critical, Off };
enum class OverflowPolicy { Block, OverrunOldest };

struct LogConfig {
    std::string loggerName{"shinkou"};
    std::filesystem::path directory{"Saved/Logs"};
    std::string fileName{"Engine.log"};
    Level level{Level::Info};
    bool console{true};
    bool file{true};
    bool asynchronous{true};
    std::size_t queueSize{8192};
    std::size_t workerThreads{1};
    OverflowPolicy overflowPolicy{OverflowPolicy::OverrunOldest};
    std::size_t maxFileSize{16u * 1024u * 1024u};
    std::size_t maxFiles{5};
    std::chrono::milliseconds flushInterval{1000};
    bool flushOnError{true};
};

struct LogStats {
    bool initialized{false};
    std::size_t queueOverruns{0};
    std::size_t queueCapacity{0};
    std::filesystem::path filePath{};
    std::string lastError{};
};

bool initialize(const LogConfig& config = {});
void shutdown() noexcept;
void flush() noexcept;
bool initialized() noexcept;
void set_level(Level level) noexcept;
Level level() noexcept;
LogStats stats();

namespace detail {
#if defined(SHINKOU_WITH_SPDLOG) && SHINKOU_WITH_SPDLOG
spdlog::logger* raw_logger() noexcept;
template <typename... Args>
inline void write(spdlog::level::level_enum severity, spdlog::source_loc location,
                  spdlog::format_string_t<Args...> format, Args&&... args) noexcept {
    auto* logger = raw_logger();
    if (logger == nullptr || !logger->should_log(severity)) return;
    try { logger->log(location, severity, format, std::forward<Args>(args)...); } catch (...) { }
}
#else
template <typename... Args>
inline void write(Level, const char*, int, const char*, Args&&...) noexcept {}
#endif
}
} // namespace shinkou::log

#if defined(SHINKOU_WITH_SPDLOG) && SHINKOU_WITH_SPDLOG
#define SHINKOU_LOG_CALL(severity, ...) \
    ::shinkou::log::detail::write(::spdlog::level::severity, \
        ::spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}, __VA_ARGS__)
#define SHINKOU_LOG_TRACE(...) SHINKOU_LOG_CALL(trace, __VA_ARGS__)
#define SHINKOU_LOG_DEBUG(...) SHINKOU_LOG_CALL(debug, __VA_ARGS__)
#define SHINKOU_LOG_INFO(...) SHINKOU_LOG_CALL(info, __VA_ARGS__)
#define SHINKOU_LOG_WARN(...) SHINKOU_LOG_CALL(warn, __VA_ARGS__)
#define SHINKOU_LOG_ERROR(...) SHINKOU_LOG_CALL(err, __VA_ARGS__)
#define SHINKOU_LOG_CRITICAL(...) SHINKOU_LOG_CALL(critical, __VA_ARGS__)
#else
#define SHINKOU_LOG_TRACE(...) do { } while (false)
#define SHINKOU_LOG_DEBUG(...) do { } while (false)
#define SHINKOU_LOG_INFO(...) do { } while (false)
#define SHINKOU_LOG_WARN(...) do { } while (false)
#define SHINKOU_LOG_ERROR(...) do { } while (false)
#define SHINKOU_LOG_CRITICAL(...) do { } while (false)
#endif
