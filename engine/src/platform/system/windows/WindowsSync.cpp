// Windows native synchronization objects, threads, TLS and fibers.

#include "WindowsBackend.h"

#include "WindowsCommon.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace shinkou::platform::detail {
namespace {

// One token address per backend; combined-object waits compare it.
struct SyncBackendToken {
    int value{0};
};
SyncBackendToken g_sync_backend_token;

constexpr DWORD kThreadResumeFailed = 0xFFFFFFFFu;

} // namespace

// ---------------------------------------------------------------------------
// Mutex
// ---------------------------------------------------------------------------
class WindowsMutex final : public IMutex {
  public:
    enum class Kind { Invalid, CriticalSection, SrwLock, KernelMutex };

    explicit WindowsMutex(const MutexOptions &options) {
        if (!options.name.empty()) {
            // Named objects must be kernel mutexes so they can be shared.
            const std::wstring wide_name = wide_from_utf8(options.name);
            handle_ = CreateMutexW(nullptr, FALSE, wide_name.c_str());
            if (handle_ == nullptr) {
                error_ = win32_error(ErrorCode::SystemFailure, "CreateMutexW");
                return;
            }
            kind_ = Kind::KernelMutex;
            return;
        }
        if (options.recursive) {
            InitializeCriticalSection(&critical_section_);
            kind_ = Kind::CriticalSection;
        } else {
            InitializeSRWLock(&srw_lock_);
            kind_ = Kind::SrwLock;
        }
    }

    ~WindowsMutex() override {
        switch (kind_) {
        case Kind::CriticalSection:
            DeleteCriticalSection(&critical_section_);
            break;
        case Kind::KernelMutex:
            if (handle_ != nullptr)
                CloseHandle(handle_);
            break;
        case Kind::SrwLock:
        case Kind::Invalid:
            break;
        }
    }

    bool valid() const noexcept {
        return kind_ != Kind::Invalid;
    }

    const SystemError &error() const noexcept {
        return error_;
    }

    Kind kind() const noexcept {
        return kind_;
    }

    CRITICAL_SECTION *critical_section() noexcept {
        return &critical_section_;
    }

    SRWLOCK *srw_lock() noexcept {
        return &srw_lock_;
    }

    void lock() override {
        switch (kind_) {
        case Kind::CriticalSection:
            EnterCriticalSection(&critical_section_);
            break;
        case Kind::SrwLock:
            AcquireSRWLockExclusive(&srw_lock_);
            break;
        case Kind::KernelMutex:
            (void)WaitForSingleObject(handle_, INFINITE);
            break;
        case Kind::Invalid:
            break;
        }
    }

    bool try_lock() override {
        switch (kind_) {
        case Kind::CriticalSection:
            return TryEnterCriticalSection(&critical_section_) != FALSE;
        case Kind::SrwLock:
            return TryAcquireSRWLockExclusive(&srw_lock_) != FALSE;
        case Kind::KernelMutex:
            return WaitForSingleObject(handle_, 0) == WAIT_OBJECT_0;
        case Kind::Invalid:
            return false;
        }
        return false;
    }

    void unlock() override {
        switch (kind_) {
        case Kind::CriticalSection:
            LeaveCriticalSection(&critical_section_);
            break;
        case Kind::SrwLock:
            ReleaseSRWLockExclusive(&srw_lock_);
            break;
        case Kind::KernelMutex:
            (void)ReleaseMutex(handle_);
            break;
        case Kind::Invalid:
            break;
        }
    }

    void *native_handle() noexcept override {
        switch (kind_) {
        case Kind::CriticalSection:
            return &critical_section_;
        case Kind::SrwLock:
            return &srw_lock_;
        case Kind::KernelMutex:
            return handle_;
        case Kind::Invalid:
            break;
        }
        return nullptr;
    }

    const void *backend_token() const noexcept override {
        return &g_sync_backend_token;
    }

  private:
    Kind kind_{Kind::Invalid};
    CRITICAL_SECTION critical_section_{};
    SRWLOCK srw_lock_{};
    HANDLE handle_{nullptr};
    SystemError error_;
};

// ---------------------------------------------------------------------------
// Condition variable
// ---------------------------------------------------------------------------
class WindowsConditionVariable final : public IConditionVariable {
  public:
    WindowsConditionVariable() {
        InitializeConditionVariable(&condition_variable_);
    }

    ~WindowsConditionVariable() override = default;

    void wait(IMutex &mutex) override {
        (void)wait_for(mutex, kInfiniteTimeout);
    }

    bool wait_for(IMutex &mutex, Nanoseconds timeout) override {
        auto *native_mutex = dynamic_cast<WindowsMutex *>(&mutex);
        if (native_mutex == nullptr || mutex.backend_token() != &g_sync_backend_token)
            return false;
        // A named (kernel) mutex cannot back a CONDITION_VARIABLE wait; the
        // documented contract requires an anonymous mutex.
        const DWORD milliseconds = win32_timeout(timeout);
        switch (native_mutex->kind()) {
        case WindowsMutex::Kind::CriticalSection:
            return SleepConditionVariableCS(&condition_variable_,
                                            native_mutex->critical_section(), milliseconds) != FALSE;
        case WindowsMutex::Kind::SrwLock:
            return SleepConditionVariableSRW(&condition_variable_, native_mutex->srw_lock(),
                                             milliseconds, 0) != FALSE;
        case WindowsMutex::Kind::KernelMutex:
        case WindowsMutex::Kind::Invalid:
            return false;
        }
        return false;
    }

    void notify_one() override {
        WakeConditionVariable(&condition_variable_);
    }

    void notify_all() override {
        WakeAllConditionVariable(&condition_variable_);
    }

    void *native_handle() noexcept override {
        return &condition_variable_;
    }

  private:
    CONDITION_VARIABLE condition_variable_{};
};

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------
class WindowsSemaphore final : public ISemaphore {
  public:
    explicit WindowsSemaphore(const SemaphoreOptions &options) {
        if (options.initial_count < 0 || options.maximum_count <= 0 ||
            options.initial_count > options.maximum_count) {
            error_ = make_error(ErrorCode::InvalidArgument, 0,
                                "semaphore count must satisfy 0 <= initial <= maximum");
            return;
        }
        const std::wstring wide_name = wide_from_utf8(options.name);
        handle_ = CreateSemaphoreW(nullptr, options.initial_count, options.maximum_count,
                                   wide_name.empty() ? nullptr : wide_name.c_str());
        if (handle_ == nullptr)
            error_ = win32_error(ErrorCode::SystemFailure, "CreateSemaphoreW");
    }

    ~WindowsSemaphore() override {
        if (handle_ != nullptr)
            CloseHandle(handle_);
    }

    bool valid() const noexcept {
        return handle_ != nullptr;
    }

    const SystemError &error() const noexcept {
        return error_;
    }

    void wait() override {
        if (handle_ != nullptr)
            (void)WaitForSingleObject(handle_, INFINITE);
    }

    bool wait_for(Nanoseconds timeout) override {
        return handle_ != nullptr && WaitForSingleObject(handle_, win32_timeout(timeout)) ==
                                        WAIT_OBJECT_0;
    }

    bool try_wait() override {
        return handle_ != nullptr && WaitForSingleObject(handle_, 0) == WAIT_OBJECT_0;
    }

    void post(std::int32_t count) override {
        if (handle_ != nullptr && count > 0)
            (void)ReleaseSemaphore(handle_, count, nullptr);
    }

    void *native_handle() noexcept override {
        return handle_;
    }

  private:
    HANDLE handle_{nullptr};
    SystemError error_;
};

// ---------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------
class WindowsEvent final : public IEvent {
  public:
    explicit WindowsEvent(const EventOptions &options)
        : manual_reset_(options.manual_reset),
          auto_signalled_(!options.manual_reset && options.initial_state) {
        const std::wstring wide_name = wide_from_utf8(options.name);
        handle_ = CreateEventW(nullptr, options.manual_reset ? TRUE : FALSE,
                               options.initial_state ? TRUE : FALSE,
                               wide_name.empty() ? nullptr : wide_name.c_str());
        if (handle_ == nullptr)
            error_ = win32_error(ErrorCode::SystemFailure, "CreateEventW");
    }

    ~WindowsEvent() override {
        if (handle_ != nullptr)
            CloseHandle(handle_);
    }

    bool valid() const noexcept {
        return handle_ != nullptr;
    }

    const SystemError &error() const noexcept {
        return error_;
    }

    void set() override {
        if (handle_ == nullptr)
            return;
        (void)SetEvent(handle_);
        std::lock_guard<std::mutex> lock(mutex_);
        auto_signalled_ = true;
    }

    void reset() override {
        if (handle_ == nullptr)
            return;
        (void)ResetEvent(handle_);
        std::lock_guard<std::mutex> lock(mutex_);
        auto_signalled_ = false;
    }

    void wait() override {
        if (handle_ == nullptr)
            return;
        if (WaitForSingleObject(handle_, INFINITE) == WAIT_OBJECT_0)
            mark_consumed();
    }

    bool wait_for(Nanoseconds timeout) override {
        if (handle_ == nullptr)
            return false;
        if (WaitForSingleObject(handle_, win32_timeout(timeout)) != WAIT_OBJECT_0)
            return false;
        mark_consumed();
        return true;
    }

    bool is_signalled() const override {
        if (handle_ == nullptr)
            return false;
        // A zero-timeout wait would consume an auto-reset event, so track the
        // logical state for that case. Named auto-reset events signalled by
        // another process are not reflected until this process waits on them.
        if (manual_reset_)
            return WaitForSingleObject(handle_, 0) == WAIT_OBJECT_0;
        std::lock_guard<std::mutex> lock(mutex_);
        return auto_signalled_;
    }

    void *native_handle() noexcept override {
        return handle_;
    }

  private:
    void mark_consumed() {
        if (manual_reset_)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto_signalled_ = false;
    }

    HANDLE handle_{nullptr};
    bool manual_reset_{false};
    mutable std::mutex mutex_;
    bool auto_signalled_{false};
    SystemError error_;
};

// ---------------------------------------------------------------------------
// Thread-local storage
// ---------------------------------------------------------------------------
class WindowsThreadLocal final : public IThreadLocal {
  public:
    WindowsThreadLocal() {
        key_ = TlsAlloc();
        if (key_ == TLS_OUT_OF_INDEXES)
            error_ = win32_error(ErrorCode::SystemFailure, "TlsAlloc");
    }

    ~WindowsThreadLocal() override {
        if (key_ != TLS_OUT_OF_INDEXES)
            (void)TlsFree(key_);
    }

    bool valid() const noexcept {
        return key_ != TLS_OUT_OF_INDEXES;
    }

    const SystemError &error() const noexcept {
        return error_;
    }

    void *get() const override {
        return key_ == TLS_OUT_OF_INDEXES ? nullptr : TlsGetValue(key_);
    }

    void set(void *value) override {
        if (key_ != TLS_OUT_OF_INDEXES)
            (void)TlsSetValue(key_, value);
    }

    std::uintptr_t native_key() const noexcept override {
        return static_cast<std::uintptr_t>(key_);
    }

  private:
    DWORD key_{TLS_OUT_OF_INDEXES};
    SystemError error_;
};

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------
namespace {

struct ThreadStartContext {
    ThreadFunction function;
    ThreadDesc description;
};

DWORD WINAPI windows_thread_entry(LPVOID parameter) {
    std::unique_ptr<ThreadStartContext> context(static_cast<ThreadStartContext *>(parameter));
    if (!context || !context->function)
        return 0;
    if (!context->description.name.empty())
        (void)set_win32_thread_name(GetCurrentThread(), context->description.name);
    if (context->description.affinity_mask != 0)
        (void)SetThreadAffinityMask(
            GetCurrentThread(), static_cast<DWORD_PTR>(context->description.affinity_mask));
    (void)SetThreadPriority(GetCurrentThread(),
                            win32_thread_priority(context->description.priority));
    context->function();
    return 0;
}

} // namespace

class WindowsThread final : public IThread {
  public:
    WindowsThread(HANDLE handle, DWORD id) : handle_(handle), id_(id) {
    }

    ~WindowsThread() override {
        detach();
    }

    std::uint64_t id() const noexcept override {
        return id_;
    }

    bool joinable() const noexcept override {
        return handle_ != nullptr;
    }

    void join() override {
        (void)join_for(kInfiniteTimeout);
    }

    bool join_for(Nanoseconds timeout) override {
        if (handle_ == nullptr)
            return true;
        const DWORD result = WaitForSingleObject(handle_, win32_timeout(timeout));
        if (result != WAIT_OBJECT_0)
            return false;
        CloseHandle(handle_);
        handle_ = nullptr;
        return true;
    }

    void detach() override {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    bool set_priority(ThreadPriority priority) override {
        return handle_ != nullptr &&
               SetThreadPriority(handle_, win32_thread_priority(priority)) != FALSE;
    }

    ThreadPriority priority() const override {
        if (handle_ == nullptr)
            return ThreadPriority::Normal;
        return thread_priority_from_win32(GetThreadPriority(handle_));
    }

    bool set_affinity(std::uint64_t processor_mask) override {
        if (handle_ == nullptr || processor_mask == 0)
            return false;
        return SetThreadAffinityMask(handle_, static_cast<DWORD_PTR>(processor_mask)) != 0;
    }

    std::uint64_t affinity() const override {
        if (handle_ == nullptr)
            return 0;
        GROUP_AFFINITY group{};
        if (!GetThreadGroupAffinity(handle_, &group))
            return 0;
        return static_cast<std::uint64_t>(group.Mask);
    }

    bool set_name(std::string_view name) override {
        return handle_ != nullptr && set_win32_thread_name(handle_, name);
    }

    void *native_handle() noexcept override {
        return handle_;
    }

  private:
    HANDLE handle_{nullptr};
    DWORD id_{0};
};

// ---------------------------------------------------------------------------
// Fibers
// ---------------------------------------------------------------------------
namespace {

struct FiberThreadState {
    bool converted{false};
    void *owner{nullptr};
    void *current{nullptr};
    std::vector<void *> parents;
};

thread_local FiberThreadState t_fiber_state;

class WindowsFiber final : public IFiber {
  public:
    WindowsFiber(FiberFunction function, void *return_fiber)
        : function_(std::move(function)), return_fiber_(return_fiber) {
        handle_ = CreateFiber(0, &WindowsFiber::trampoline, this);
    }

    ~WindowsFiber() override {
        if (handle_ != nullptr)
            DeleteFiber(handle_);
    }

    bool valid() const noexcept {
        return handle_ != nullptr;
    }

    void resume() override {
        if (handle_ == nullptr || finished_)
            return;
        if (!t_fiber_state.converted)
            return;
        return_fiber_ = t_fiber_state.current;
        t_fiber_state.parents.push_back(t_fiber_state.current);
        t_fiber_state.current = handle_;
        SwitchToFiber(handle_);
        if (!t_fiber_state.parents.empty())
            t_fiber_state.parents.pop_back();
        t_fiber_state.current = return_fiber_;
    }

    bool finished() const noexcept override {
        return finished_;
    }

    void *native_handle() noexcept override {
        return handle_;
    }

  private:
    static VOID WINAPI trampoline(PVOID parameter) {
        auto *fiber = static_cast<WindowsFiber *>(parameter);
        if (fiber->function_)
            fiber->function_();
        fiber->finished_ = true;
        // A fiber's start routine must never return: switch to the fiber that
        // resumed us and keep this fiber parked until DeleteFiber runs.
        for (;;)
            SwitchToFiber(fiber->return_fiber_);
    }

    FiberFunction function_;
    void *return_fiber_{nullptr};
    void *handle_{nullptr};
    bool finished_{false};
};

} // namespace

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IMutex>> make_windows_mutex(const MutexOptions &options) {
    auto mutex = std::make_unique<WindowsMutex>(options);
    if (!mutex->valid())
        return mutex->error();
    return std::unique_ptr<IMutex>(std::move(mutex));
}

Result<std::unique_ptr<IConditionVariable>> make_windows_condition_variable() {
    return std::unique_ptr<IConditionVariable>(std::make_unique<WindowsConditionVariable>());
}

Result<std::unique_ptr<ISemaphore>> make_windows_semaphore(const SemaphoreOptions &options) {
    auto semaphore = std::make_unique<WindowsSemaphore>(options);
    if (!semaphore->valid())
        return semaphore->error();
    return std::unique_ptr<ISemaphore>(std::move(semaphore));
}

Result<std::unique_ptr<IEvent>> make_windows_event(const EventOptions &options) {
    auto event = std::make_unique<WindowsEvent>(options);
    if (!event->valid())
        return event->error();
    return std::unique_ptr<IEvent>(std::move(event));
}

Result<std::unique_ptr<IThreadLocal>> make_windows_thread_local() {
    auto local = std::make_unique<WindowsThreadLocal>();
    if (!local->valid())
        return local->error();
    return std::unique_ptr<IThreadLocal>(std::move(local));
}

Result<std::unique_ptr<IThread>> make_windows_thread(const ThreadDesc &desc,
                                                     ThreadFunction function) {
    if (!function)
        return make_error(ErrorCode::InvalidArgument, 0, "thread function is empty");

    auto context = std::make_unique<ThreadStartContext>();
    context->function = std::move(function);
    context->description = desc;
    ThreadStartContext *raw_context = context.get();

    DWORD id = 0;
    HANDLE handle = CreateThread(nullptr, desc.stack_size, &windows_thread_entry, raw_context,
                                 CREATE_SUSPENDED, &id);
    if (handle == nullptr)
        return win32_error(ErrorCode::SystemFailure, "CreateThread");

    context.release();
    if (!desc.name.empty())
        (void)set_win32_thread_name(handle, desc.name);
    if (desc.affinity_mask != 0)
        (void)SetThreadAffinityMask(handle, static_cast<DWORD_PTR>(desc.affinity_mask));
    (void)SetThreadPriority(handle, win32_thread_priority(desc.priority));

    if (ResumeThread(handle) == kThreadResumeFailed) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        delete raw_context;
        return win32_error(error, ErrorCode::SystemFailure, "ResumeThread");
    }
    return std::unique_ptr<IThread>(new WindowsThread(handle, id));
}

Result<std::unique_ptr<IFiber>> make_windows_fiber(FiberFunction function, void *return_fiber) {
    if (!function)
        return make_error(ErrorCode::InvalidArgument, 0, "fiber function is empty");
    auto fiber = std::make_unique<WindowsFiber>(std::move(function), return_fiber);
    if (!fiber->valid())
        return win32_error(ErrorCode::SystemFailure, "CreateFiber");
    return std::unique_ptr<IFiber>(std::move(fiber));
}

// ---------------------------------------------------------------------------
// WindowsSystem fiber entry points (defined here so all fiber state stays in
// this translation unit).
// ---------------------------------------------------------------------------
bool WindowsSystem::convert_current_thread_to_fiber() {
    if (t_fiber_state.converted)
        return true;
    void *owner = ConvertThreadToFiber(nullptr);
    if (owner == nullptr) {
        if (GetLastError() != ERROR_ALREADY_FIBER)
            return false;
        owner = GetCurrentFiber();
        if (owner == nullptr)
            return false;
    }
    t_fiber_state.converted = true;
    t_fiber_state.owner = owner;
    t_fiber_state.current = owner;
    t_fiber_state.parents.clear();
    return true;
}

bool WindowsSystem::revert_current_thread_from_fiber() {
    if (!t_fiber_state.converted)
        return true;
    if (!t_fiber_state.parents.empty() || t_fiber_state.current != t_fiber_state.owner)
        return false;
    if (!ConvertFiberToThread())
        return false;
    t_fiber_state = FiberThreadState{};
    return true;
}

bool WindowsSystem::current_thread_is_fiber() const noexcept {
    return t_fiber_state.converted;
}

Result<std::unique_ptr<IFiber>> WindowsSystem::create_fiber(FiberFunction function) {
    if (!t_fiber_state.converted)
        return make_error(ErrorCode::InvalidArgument, 0,
                          "convert the current thread to a fiber first");
    return make_windows_fiber(std::move(function), t_fiber_state.current);
}

void WindowsSystem::yield_fiber() {
    if (t_fiber_state.parents.empty())
        return;
    SwitchToFiber(t_fiber_state.parents.back());
}

} // namespace shinkou::platform::detail
