#include "shinkou/log/Log.h"

#if defined(SHINKOU_WITH_SPDLOG) && SHINKOU_WITH_SPDLOG
#include <spdlog/async.h>
#include <spdlog/details/periodic_worker.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace shinkou::log {
namespace detail {
alignas(64) std::atomic<int> activeLevel{spdlog::level::off};
}
namespace {
struct alignas(64) Reader {
    std::atomic<spdlog::logger*> protectedLogger{nullptr};
    bool available{false}; // guarded by readersMutex
};
struct Session {
    std::shared_ptr<spdlog::details::thread_pool> pool;
    std::shared_ptr<spdlog::logger> logger;
    std::unique_ptr<spdlog::details::periodic_worker> periodic;
    std::size_t finalOverruns{0};
    void stop() noexcept {
        periodic.reset();
        // A flush in an overflowing queue could evict the last real record.
        // Instead, terminate and drain the pool, then flush each sink directly.
        if (pool) finalOverruns = pool->overrun_counter();
        pool.reset();
        if (logger) for (auto& sink : logger->sinks()) {
            try { sink->flush(); }
            catch (...) { detail::report_error("log sink flush failed"); }
        }
        logger.reset();
    }
    ~Session() { stop(); }
};
struct State {
    std::mutex errorMutex;
    std::string lastError;
    std::atomic<std::size_t> errors{0};
    std::mutex lifecycleMutex;
    std::mutex readersMutex;
    std::vector<std::unique_ptr<Reader>> readers;
    alignas(64) std::atomic<spdlog::logger*> published{nullptr};
    LogConfig config{};
    std::filesystem::path filePath;
    std::size_t finalOverruns{0};
    std::unique_ptr<Session> session;
    void close_gate() noexcept {
        detail::activeLevel.store(spdlog::level::off, std::memory_order_relaxed);
        auto* retired = published.exchange(nullptr, std::memory_order_seq_cst);
        std::size_t count;
        {
            std::lock_guard<std::mutex> lock(readersMutex);
            count = readers.size();
        }
        // Reader objects never move or disappear during a session. Do not
        // hold registration's mutex while waiting for an active formatter.
        for (std::size_t index = 0; retired && index < count; ++index) {
            Reader* reader;
            {
                std::lock_guard<std::mutex> lock(readersMutex);
                reader = readers[index].get();
            }
            while (reader->protectedLogger.load(std::memory_order_seq_cst) == retired)
                std::this_thread::yield();
        }
        // A set_level lease acquired before unpublishing may update the cache.
        detail::activeLevel.store(spdlog::level::off, std::memory_order_relaxed);
    }
    ~State() { close_gate(); session.reset(); }
};
State& state() { static State value; return value; }
struct ThreadLease {
    State* owner{nullptr};
    Reader* reader{nullptr};
    spdlog::logger* active{nullptr};
    std::size_t depth{0};
    ~ThreadLease() {
        if (!reader) return;
        reader->protectedLogger.store(nullptr, std::memory_order_release);
        std::lock_guard<std::mutex> lock(owner->readersMutex);
        reader->available = true;
    }
    void register_reader(State& current) {
        std::lock_guard<std::mutex> lock(current.readersMutex);
        owner = &current;
        for (auto& entry : current.readers) if (entry->available) {
            entry->available = false;
            reader = entry.get();
            return;
        }
        auto entry = std::make_unique<Reader>();
        auto* pointer = entry.get();
        current.readers.push_back(std::move(entry));
        reader = pointer;
    }
};
ThreadLease& thread_lease() { thread_local ThreadLease lease; return lease; }
spdlog::level::level_enum convert(Level value) noexcept {
    return static_cast<spdlog::level::level_enum>(value);
}
}
namespace detail {
LoggerLease acquire_logger() noexcept {
    auto& current = state();
    auto& local = thread_lease();
    if (local.depth) {
        ++local.depth;
        return LoggerLease{local.active, &local};
    }
    auto* logger = current.published.load(std::memory_order_seq_cst);
    if (!logger) return LoggerLease{};
    if (!local.reader) {
        try { local.register_reader(current); }
        catch (...) { report_error("cannot allocate a log reader record"); return LoggerLease{}; }
    }
    // Publish then validate. The SC order guarantees that a retiring writer
    // either sees this protection or this reader sees the changed publication.
    for (;;) {
        local.reader->protectedLogger.store(logger, std::memory_order_seq_cst);
        auto* latest = current.published.load(std::memory_order_seq_cst);
        const bool stable = latest == logger;
        logger = latest;
        if (stable) break;
    }
    if (logger) { local.active = logger; local.depth = 1; }
    return LoggerLease{logger, &local};
}
LoggerLease::~LoggerLease() {
    if (!logger) return;
    auto& local = *static_cast<ThreadLease*>(context);
    if (--local.depth == 0) {
        local.reader->protectedLogger.store(nullptr, std::memory_order_release);
        local.active = nullptr;
    }
}
void report_error(const char* message) noexcept {
    auto& current = state();
    current.errors.fetch_add(1, std::memory_order_relaxed);
    try {
        std::lock_guard<std::mutex> lock(current.errorMutex);
        current.lastError = message;
    } catch (...) {}
}
}
bool initialize(const LogConfig& requested) {
    auto& current = state();
    std::lock_guard<std::mutex> lock(current.lifecycleMutex);
    if (current.session) return true;
    {
        std::lock_guard<std::mutex> errorLock(current.errorMutex);
        current.lastError.clear();
    }
    current.errors.store(0, std::memory_order_relaxed);
    current.filePath.clear();
    current.finalOverruns = 0;
    try {
        if (requested.level < Level::Trace || requested.level > Level::Off)
            throw std::invalid_argument("invalid logging level");
        if (requested.flushInterval.count() < 0)
            throw std::invalid_argument("negative log flush interval");
        if (requested.workerThreads > 64)
            throw std::invalid_argument("log worker count exceeds 64");
        auto config = requested;
        config.queueSize = (std::max)(config.queueSize, std::size_t{256});
        config.workerThreads = (std::max)(config.workerThreads, std::size_t{1});
        config.maxFileSize = (std::max)(config.maxFileSize, std::size_t{1024});
        config.maxFiles = (std::max)(config.maxFiles, std::size_t{1});
        auto session = std::make_unique<Session>();
        std::vector<spdlog::sink_ptr> sinks;
        sinks.reserve(2);
        if (config.console) sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        if (config.file) {
            if (config.fileName.empty()) throw std::invalid_argument("log file name is empty");
            std::error_code error;
            if (!config.directory.empty()) std::filesystem::create_directories(config.directory, error);
            if (error) throw std::runtime_error("cannot create log directory: " + error.message());
            current.filePath = config.directory / config.fileName;
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                current.filePath.string(), config.maxFileSize, config.maxFiles, config.rotateOnOpen));
        }
        if (sinks.empty()) sinks.push_back(std::make_shared<spdlog::sinks::null_sink_mt>());
        if (config.asynchronous) {
            session->pool = std::make_shared<spdlog::details::thread_pool>(config.queueSize, config.workerThreads);
            const auto overflow = config.overflowPolicy == OverflowPolicy::Block
                ? spdlog::async_overflow_policy::block : spdlog::async_overflow_policy::overrun_oldest;
            session->logger = std::make_shared<spdlog::async_logger>(
                config.loggerName, sinks.begin(), sinks.end(), session->pool, overflow);
        } else session->logger = std::make_shared<spdlog::logger>(config.loggerName, sinks.begin(), sinks.end());
        auto logger = session->logger;
        logger->set_level(convert(config.level));
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] [thread %t] %v");
        logger->set_error_handler([](const std::string& message) { detail::report_error(message.c_str()); });
        if (config.flushOnError) logger->flush_on(spdlog::level::err);
        if (config.flushInterval.count() > 0) {
            session->periodic = std::make_unique<spdlog::details::periodic_worker>(
                [logger] { logger->flush(); }, config.flushInterval);
        }
        logger->info("logging initialized; async={}, queue={}, file={}", config.asynchronous,
            config.queueSize, config.file ? current.filePath.string() : "disabled");
        current.config = std::move(config);
        current.session = std::move(session);
        detail::activeLevel.store(convert(current.config.level), std::memory_order_relaxed);
        current.published.store(logger.get(), std::memory_order_seq_cst);
        return true;
    } catch (const std::exception& error) { detail::report_error(error.what()); }
    catch (...) { detail::report_error("unknown logging initialization failure"); }
    return false;
}
void shutdown() noexcept {
    auto& current = state();
    std::lock_guard<std::mutex> lock(current.lifecycleMutex);
    if (!current.session) return;
    current.close_gate();
    current.session->stop();
    current.finalOverruns = current.session->finalOverruns;
    current.session.reset();
}
void flush() noexcept {
    auto lease = detail::acquire_logger();
    if (lease.logger) try { lease.logger->flush(); }
    catch (...) { detail::report_error("log flush submission failed"); }
}
bool initialized() noexcept { return state().published.load(std::memory_order_acquire) != nullptr; }
void set_level(Level value) noexcept {
    if (value < Level::Trace || value > Level::Off) return;
    auto lease = detail::acquire_logger();
    if (!lease.logger) return;
    lease.logger->set_level(convert(value));
    detail::activeLevel.store(convert(value), std::memory_order_relaxed);
}
Level level() noexcept {
    auto lease = detail::acquire_logger();
    return lease.logger ? static_cast<Level>(lease.logger->level()) : Level::Off;
}
LogStats stats() {
    auto& current = state();
    std::lock_guard<std::mutex> lock(current.lifecycleMutex);
    LogStats result;
    result.initialized = static_cast<bool>(current.session);
    result.queueOverruns = current.finalOverruns;
    result.filePath = current.filePath;
    result.errors = current.errors.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> readerLock(current.readersMutex);
        result.readerSlots = current.readers.size();
    }
    if (current.session && current.session->pool) {
        result.queueCapacity = current.config.queueSize;
        result.workerThreads = current.config.workerThreads;
        result.queueDepth = current.session->pool->queue_size();
        result.queueOverruns = current.session->pool->overrun_counter();
    }
    {
        std::lock_guard<std::mutex> errorLock(current.errorMutex);
        result.lastError = current.lastError;
    }
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
