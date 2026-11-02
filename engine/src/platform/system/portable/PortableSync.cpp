// Portable synchronization objects, threads and thread-local storage built on
// the C++17 standard library.

#include "PortableBackend.h"

#include "PortableCommon.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace shinkou::platform::detail {
namespace {

struct PortableSyncToken {
    int value{0};
};
PortableSyncToken g_portable_sync_token;

std::atomic<std::uint32_t> g_thread_local_next{1};
thread_local std::unordered_map<std::uint32_t, void *> t_thread_local_values;

constexpr std::chrono::nanoseconds to_chrono(Nanoseconds value) {
    return std::chrono::nanoseconds{value};
}

} // namespace

// ---------------------------------------------------------------------------
// Mutex
// ---------------------------------------------------------------------------
class PortableMutex final : public IMutex {
  public:
    explicit PortableMutex(bool recursive) : recursive_(recursive) {
    }

    void lock() override {
        if (recursive_)
            recursive_mutex_.lock();
        else
            mutex_.lock();
    }

    bool try_lock() override {
        return recursive_ ? recursive_mutex_.try_lock() : mutex_.try_lock();
    }

    void unlock() override {
        if (recursive_)
            recursive_mutex_.unlock();
        else
            mutex_.unlock();
    }

    void *native_handle() noexcept override {
        return recursive_ ? static_cast<void *>(&recursive_mutex_)
                          : static_cast<void *>(&mutex_);
    }

    const void *backend_token() const noexcept override {
        return &g_portable_sync_token;
    }

  private:
    bool recursive_{false};
    std::mutex mutex_;
    std::recursive_mutex recursive_mutex_;
};

// ---------------------------------------------------------------------------
// Condition variable
// ---------------------------------------------------------------------------
class PortableConditionVariable final : public IConditionVariable {
  public:
    void wait(IMutex &mutex) override {
        condition_.wait(mutex);
    }

    bool wait_for(IMutex &mutex, Nanoseconds timeout) override {
        if (timeout < 0) {
            condition_.wait(mutex);
            return true;
        }
        return condition_.wait_for(mutex, to_chrono(timeout)) == std::cv_status::no_timeout;
    }

    void notify_one() override {
        condition_.notify_one();
    }

    void notify_all() override {
        condition_.notify_all();
    }

    void *native_handle() noexcept override {
        return &condition_;
    }

  private:
    std::condition_variable_any condition_;
};

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------
class PortableSemaphore final : public ISemaphore {
  public:
    explicit PortableSemaphore(const SemaphoreOptions &options) {
        if (options.initial_count < 0 || options.maximum_count <= 0 ||
            options.initial_count > options.maximum_count) {
            error_ = make_error(ErrorCode::InvalidArgument, 0,
                                "semaphore count must satisfy 0 <= initial <= maximum");
            return;
        }
        count_ = options.initial_count;
        maximum_ = options.maximum_count;
        valid_ = true;
    }

    bool valid() const noexcept {
        return valid_;
    }

    const SystemError &error() const noexcept {
        return error_;
    }

    void wait() override {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return count_ > 0; });
        --count_;
    }

    bool wait_for(Nanoseconds timeout) override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (timeout < 0) {
            condition_.wait(lock, [this] { return count_ > 0; });
            --count_;
            return true;
        }
        if (!condition_.wait_for(lock, to_chrono(timeout), [this] { return count_ > 0; }))
            return false;
        --count_;
        return true;
    }

    bool try_wait() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ <= 0)
            return false;
        --count_;
        return true;
    }

    void post(std::int32_t count) override {
        if (count <= 0)
            return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            count_ = std::min(maximum_, count_ + count);
        }
        condition_.notify_all();
    }

    void *native_handle() noexcept override {
        return &mutex_;
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::int32_t count_{0};
    std::int32_t maximum_{1};
    bool valid_{false};
    SystemError error_;
};

// ---------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------
class PortableEvent final : public IEvent {
  public:
    explicit PortableEvent(const EventOptions &options)
        : manual_reset_(options.manual_reset), signalled_(options.initial_state) {
    }

    void set() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            signalled_ = true;
        }
        if (manual_reset_)
            condition_.notify_all();
        else
            condition_.notify_one();
    }

    void reset() override {
        std::lock_guard<std::mutex> lock(mutex_);
        signalled_ = false;
    }

    void wait() override {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return signalled_; });
        if (!manual_reset_)
            signalled_ = false;
    }

    bool wait_for(Nanoseconds timeout) override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (timeout < 0) {
            condition_.wait(lock, [this] { return signalled_; });
            if (!manual_reset_)
                signalled_ = false;
            return true;
        }
        if (!condition_.wait_for(lock, to_chrono(timeout), [this] { return signalled_; }))
            return false;
        if (!manual_reset_)
            signalled_ = false;
        return true;
    }

    bool is_signalled() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return signalled_;
    }

    void *native_handle() noexcept override {
        return &mutex_;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool manual_reset_{false};
    bool signalled_{false};
};

// ---------------------------------------------------------------------------
// Thread-local storage
// ---------------------------------------------------------------------------
class PortableThreadLocal final : public IThreadLocal {
  public:
    PortableThreadLocal() : key_(g_thread_local_next.fetch_add(1, std::memory_order_relaxed)) {
    }

    ~PortableThreadLocal() override {
        t_thread_local_values.erase(key_);
    }

    void *get() const override {
        const auto found = t_thread_local_values.find(key_);
        return found == t_thread_local_values.end() ? nullptr : found->second;
    }

    void set(void *value) override {
        if (value == nullptr)
            t_thread_local_values.erase(key_);
        else
            t_thread_local_values[key_] = value;
    }

    std::uintptr_t native_key() const noexcept override {
        return static_cast<std::uintptr_t>(key_);
    }

  private:
    std::uint32_t key_{0};
};

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------
namespace {

struct PortableThreadState {
    ThreadFunction function;
    std::atomic<bool> finished{false};
};

} // namespace

class PortableThread final : public IThread {
  public:
    explicit PortableThread(std::shared_ptr<PortableThreadState> state) : state_(std::move(state)) {
        thread_ = std::thread([state = state_] {
            if (state->function)
                state->function();
            state->finished.store(true, std::memory_order_release);
        });
        id_ = static_cast<std::uint64_t>(
            std::hash<std::thread::id>{}(thread_.get_id()));
    }

    ~PortableThread() override {
        if (thread_.joinable())
            thread_.detach();
    }

    std::uint64_t id() const noexcept override {
        return id_;
    }

    bool joinable() const noexcept override {
        return thread_.joinable();
    }

    void join() override {
        if (thread_.joinable())
            thread_.join();
        state_->finished.store(true, std::memory_order_release);
    }

    bool join_for(Nanoseconds timeout) override {
        if (!thread_.joinable())
            return true;
        if (timeout < 0) {
            join();
            return true;
        }
        const auto deadline = std::chrono::steady_clock::now() + to_chrono(timeout);
        while (!state_->finished.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        join();
        return true;
    }

    void detach() override {
        if (thread_.joinable())
            thread_.detach();
    }

    bool set_priority(ThreadPriority) override {
        return false;
    }

    ThreadPriority priority() const override {
        return ThreadPriority::Normal;
    }

    bool set_affinity(std::uint64_t) override {
        return false;
    }

    std::uint64_t affinity() const override {
        return 0;
    }

    bool set_name(std::string_view) override {
        return false;
    }

    void *native_handle() noexcept override {
        return nullptr;
    }

  private:
    std::shared_ptr<PortableThreadState> state_;
    std::thread thread_;
    std::uint64_t id_{0};
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IMutex>> make_portable_mutex(const MutexOptions &options) {
    if (!options.name.empty())
        return make_error(ErrorCode::NotSupported, 0,
                          "named kernel mutexes are not available in the portable backend");
    return std::unique_ptr<IMutex>(std::make_unique<PortableMutex>(options.recursive));
}

Result<std::unique_ptr<IConditionVariable>> make_portable_condition_variable() {
    return std::unique_ptr<IConditionVariable>(std::make_unique<PortableConditionVariable>());
}

Result<std::unique_ptr<ISemaphore>> make_portable_semaphore(const SemaphoreOptions &options) {
    if (!options.name.empty())
        return make_error(ErrorCode::NotSupported, 0,
                          "named semaphores are not available in the portable backend");
    auto semaphore = std::make_unique<PortableSemaphore>(options);
    if (!semaphore->valid())
        return semaphore->error();
    return std::unique_ptr<ISemaphore>(std::move(semaphore));
}

Result<std::unique_ptr<IEvent>> make_portable_event(const EventOptions &options) {
    if (!options.name.empty())
        return make_error(ErrorCode::NotSupported, 0,
                          "named events are not available in the portable backend");
    return std::unique_ptr<IEvent>(std::make_unique<PortableEvent>(options));
}

Result<std::unique_ptr<IThreadLocal>> make_portable_thread_local() {
    return std::unique_ptr<IThreadLocal>(std::make_unique<PortableThreadLocal>());
}

Result<std::unique_ptr<IThread>> make_portable_thread(const ThreadDesc &,
                                                      ThreadFunction function) {
    if (!function)
        return make_error(ErrorCode::InvalidArgument, 0, "thread function is empty");
    auto state = std::make_shared<PortableThreadState>();
    state->function = std::move(function);
    return std::unique_ptr<IThread>(std::make_unique<PortableThread>(std::move(state)));
}

} // namespace shinkou::platform::detail
