// POSIX threads, thread scheduling control and ucontext-based fibers.

#include "PosixConfig.h"

#include "PosixBackend.h"
#include "PosixCommon.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <ucontext.h>
#include <unistd.h>

#include <limits.h>

namespace shinkou::platform::detail {
namespace {

std::string truncated_thread_name(std::string_view name) {
    constexpr std::size_t kMaxThreadName = 15;
    const std::size_t length = std::min(name.size(), kMaxThreadName);
    return std::string{name.substr(0, length)};
}

bool set_thread_name(pthread_t thread, std::string_view name) {
    if (name.empty())
        return false;
#if defined(__linux__)
    const std::string value = truncated_thread_name(name);
    return pthread_setname_np(thread, value.c_str()) == 0;
#elif defined(__APPLE__)
    if (!pthread_equal(thread, pthread_self()))
        return false;
    const std::string value = truncated_thread_name(name);
    return pthread_setname_np(value.c_str()) == 0;
#else
    (void)thread;
    return false;
#endif
}

bool set_thread_affinity(pthread_t thread, std::uint64_t processor_mask) {
#if defined(__linux__)
    if (processor_mask == 0)
        return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int index = 0; index < 64 && index < CPU_SETSIZE; ++index) {
        if ((processor_mask & (1ULL << static_cast<unsigned int>(index))) != 0)
            CPU_SET(index, &set);
    }
    return pthread_setaffinity_np(thread, sizeof(set), &set) == 0;
#else
    (void)thread;
    (void)processor_mask;
    return false;
#endif
}

std::uint64_t thread_affinity(pthread_t thread) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (pthread_getaffinity_np(thread, sizeof(set), &set) != 0)
        return 0;
    std::uint64_t mask = 0;
    for (int index = 0; index < 64 && index < CPU_SETSIZE; ++index) {
        if (CPU_ISSET(index, &set))
            mask |= 1ULL << static_cast<unsigned int>(index);
    }
    return mask;
#else
    (void)thread;
    return 0;
#endif
}

bool set_thread_priority(ThreadPriority priority) {
    // SCHED_OTHER only has priority 0; real-time classes need elevated
    // privileges, so the backend advertises only the normal priority.
    if (priority != ThreadPriority::Normal)
        return false;
    sched_param parameters{};
    parameters.sched_priority = 0;
    return pthread_setschedparam(pthread_self(), SCHED_OTHER, &parameters) == 0;
}

std::uint64_t thread_id_of(pthread_t thread) noexcept {
    std::uint64_t value = 0;
    static_assert(sizeof(pthread_t) <= sizeof(value), "pthread_t does not fit in uint64");
    std::memcpy(&value, &thread, sizeof(thread));
    return value;
}

struct ThreadCompletion {
    std::mutex mutex;
    std::condition_variable condition;
    bool finished{false};
};

struct ThreadStartContext {
    ThreadFunction function;
    ThreadDesc description;
    std::shared_ptr<ThreadCompletion> completion;
};

void *thread_entry(void *parameter) {
    std::unique_ptr<ThreadStartContext> context(
        static_cast<ThreadStartContext *>(parameter));
    if (!context)
        return nullptr;
    if (!context->description.name.empty())
        (void)set_thread_name(pthread_self(), context->description.name);
    if (context->description.affinity_mask != 0)
        (void)set_thread_affinity(pthread_self(), context->description.affinity_mask);
    (void)set_thread_priority(context->description.priority);
    if (context->function)
        context->function();
    {
        std::lock_guard<std::mutex> lock(context->completion->mutex);
        context->completion->finished = true;
    }
    context->completion->condition.notify_all();
    return nullptr;
}

class PosixThread final : public IThread {
  public:
    PosixThread(pthread_t thread, std::shared_ptr<ThreadCompletion> completion)
        : thread_(thread), completion_(std::move(completion)), id_(thread_id_of(thread)) {
    }

    ~PosixThread() override {
        if (joinable_)
            (void)pthread_detach(thread_);
    }

    std::uint64_t id() const noexcept override {
        return id_;
    }

    bool joinable() const noexcept override {
        return joinable_;
    }

    void join() override {
        if (!joinable_)
            return;
        {
            std::unique_lock<std::mutex> lock(completion_->mutex);
            completion_->condition.wait(lock, [this] { return completion_->finished; });
        }
        (void)pthread_join(thread_, nullptr);
        joinable_ = false;
    }

    bool join_for(Nanoseconds timeout) override {
        if (!joinable_)
            return true;
        if (timeout < 0) {
            join();
            return true;
        }
        {
            std::unique_lock<std::mutex> lock(completion_->mutex);
            if (!completion_->condition.wait_for(lock, std::chrono::nanoseconds{timeout},
                                                 [this] { return completion_->finished; }))
                return false;
        }
        (void)pthread_join(thread_, nullptr);
        joinable_ = false;
        return true;
    }

    void detach() override {
        if (!joinable_)
            return;
        (void)pthread_detach(thread_);
        joinable_ = false;
    }

    bool set_priority(ThreadPriority priority) override {
        if (!joinable_)
            return false;
        if (priority != ThreadPriority::Normal)
            return false;
        sched_param parameters{};
        parameters.sched_priority = 0;
        return pthread_setschedparam(thread_, SCHED_OTHER, &parameters) == 0;
    }

    ThreadPriority priority() const override {
        return ThreadPriority::Normal;
    }

    bool set_affinity(std::uint64_t processor_mask) override {
        return joinable_ && set_thread_affinity(thread_, processor_mask);
    }

    std::uint64_t affinity() const override {
        return joinable_ ? thread_affinity(thread_) : 0;
    }

    bool set_name(std::string_view name) override {
        return joinable_ && set_thread_name(thread_, name);
    }

    void *native_handle() noexcept override {
        std::uintptr_t value = 0;
        static_assert(sizeof(pthread_t) <= sizeof(value), "pthread_t does not fit in uintptr");
        std::memcpy(&value, &thread_, sizeof(thread_));
        return reinterpret_cast<void *>(value);
    }

  private:
    pthread_t thread_{};
    std::shared_ptr<ThreadCompletion> completion_;
    std::uint64_t id_{0};
    bool joinable_{true};
};

// ---------------------------------------------------------------------------
// Fibers (ucontext)
// ---------------------------------------------------------------------------
class PosixFiber;

thread_local bool t_fiber_converted = false;
thread_local PosixFiber *t_active_fiber = nullptr;

class PosixFiber final : public IFiber {
  public:
    explicit PosixFiber(FiberFunction function) : function_(std::move(function)) {
        stack_.resize(1024 * 1024);
        if (getcontext(&context_) != 0) {
            error_ = posix_last_error(ErrorCode::SystemFailure, "getcontext");
            return;
        }
        context_.uc_stack.ss_sp = stack_.data();
        context_.uc_stack.ss_size = stack_.size();
        context_.uc_link = nullptr;
        makecontext(&context_, &PosixFiber::entry, 0);
        valid_ = true;
    }

    void resume() override {
        if (!valid_ || finished_ || !t_fiber_converted)
            return;
        PosixFiber *previous = t_active_fiber;
        t_active_fiber = this;
        (void)swapcontext(&caller_, &context_);
        t_active_fiber = previous;
    }

    bool finished() const noexcept override {
        return finished_;
    }

    void *native_handle() noexcept override {
        return &context_;
    }

    bool valid() const noexcept {
        return valid_;
    }

    const SystemError &error() const noexcept {
        return error_;
    }

    void yield_to_caller() {
        if (!valid_ || finished_)
            return;
        (void)swapcontext(&context_, &caller_);
    }

  private:
    static void entry() {
        PosixFiber *self = t_active_fiber;
        if (self != nullptr && self->function_)
            self->function_();
        if (self != nullptr) {
            self->finished_ = true;
            (void)swapcontext(&self->context_, &self->caller_);
        }
        // A finished fiber is never resumed again (resume() checks the flag),
        // but it must never fall off the ucontext stack either.
        for (;;) {
        }
    }

    FiberFunction function_;
    ucontext_t context_{};
    ucontext_t caller_{};
    std::vector<char> stack_;
    bool valid_{false};
    bool finished_{false};
    SystemError error_;
};

} // namespace

Result<std::unique_ptr<IThread>> make_posix_thread(const ThreadDesc &desc,
                                                   ThreadFunction function) {
    if (!function)
        return make_error(ErrorCode::InvalidArgument, 0, "thread function is empty");

    auto completion = std::make_shared<ThreadCompletion>();
    auto *context = new ThreadStartContext{std::move(function), desc, completion};

    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) {
        delete context;
        return make_error(ErrorCode::SystemFailure, 0, "pthread_attr_init failed");
    }
    if (desc.stack_size > 0) {
        const std::size_t stack_size =
            std::max(desc.stack_size, static_cast<std::size_t>(PTHREAD_STACK_MIN));
        (void)pthread_attr_setstacksize(&attributes, stack_size);
    }

    pthread_t thread{};
    const int result = pthread_create(&thread, &attributes, &thread_entry, context);
    (void)pthread_attr_destroy(&attributes);
    if (result != 0) {
        delete context;
        return posix_error(result, ErrorCode::SystemFailure, "pthread_create");
    }
    return std::unique_ptr<IThread>(new PosixThread(thread, std::move(completion)));
}

// ---------------------------------------------------------------------------
// PosixSystem thread entry points
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IThread>> PosixSystem::create_thread(const ThreadDesc &desc,
                                                            ThreadFunction function) {
    return make_posix_thread(desc, std::move(function));
}

std::uint64_t PosixSystem::current_thread_affinity() const {
    return thread_affinity(pthread_self());
}

bool PosixSystem::set_current_thread_affinity(std::uint64_t processor_mask) {
    return set_thread_affinity(pthread_self(), processor_mask);
}

bool PosixSystem::set_current_thread_name(std::string_view name) {
    return set_thread_name(pthread_self(), name);
}

bool PosixSystem::set_current_thread_priority(ThreadPriority priority) {
    return set_thread_priority(priority);
}

ThreadPriority PosixSystem::current_thread_priority() const {
    return ThreadPriority::Normal;
}

// ---------------------------------------------------------------------------
// PosixSystem fiber entry points
// ---------------------------------------------------------------------------
bool PosixSystem::convert_current_thread_to_fiber() {
    if (t_fiber_converted)
        return true;
    t_fiber_converted = true;
    t_active_fiber = nullptr;
    return true;
}

bool PosixSystem::revert_current_thread_from_fiber() {
    if (!t_fiber_converted)
        return true;
    if (t_active_fiber != nullptr)
        return false;
    t_fiber_converted = false;
    return true;
}

bool PosixSystem::current_thread_is_fiber() const noexcept {
    return t_fiber_converted;
}

Result<std::unique_ptr<IFiber>> PosixSystem::create_fiber(FiberFunction function) {
    if (!t_fiber_converted)
        return make_error(ErrorCode::InvalidArgument, 0,
                          "convert the current thread to a fiber first");
    if (!function)
        return make_error(ErrorCode::InvalidArgument, 0, "fiber function is empty");
    auto fiber = std::make_unique<PosixFiber>(std::move(function));
    if (!fiber->valid())
        return fiber->error();
    return std::unique_ptr<IFiber>(std::move(fiber));
}

void PosixSystem::yield_fiber() {
    if (t_active_fiber != nullptr)
        t_active_fiber->yield_to_caller();
}

} // namespace shinkou::platform::detail
