#pragma once

// Shinkou Engine - kernel synchronization primitives.
//
// These interfaces wrap the native blocking primitives (critical sections /
// SRW locks / condition variables / semaphores / events on Windows and their
// pthread equivalents elsewhere). They are created by ISystem because the
// native object must be owned by the backend that understands it.
//
// Locking a mutex is a cold path compared to the engine's spin locks and
// atomics; the virtual dispatch buys named cross-process kernel objects,
// timeouts and debugger ownership information. Hot paths should keep using
// std::atomic and SpinLock below.
//
// All waiting functions take a Nanoseconds timeout. A negative timeout means
// "wait until signalled"; zero performs a poll. Wait functions on condition
// variables and events return true when signalled and false on timeout.

#include "shinkou/platform/Error.h"
#include "shinkou/platform/Platform.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace shinkou::platform {

struct MutexOptions {
    // A recursive mutex may be locked again by the thread that owns it.
    bool recursive{false};
    // Non-empty creates a named, system-wide object shared by processes on
    // backends that support Capability::NamedKernelObjects.
    std::string name;
};

struct SemaphoreOptions {
    std::int32_t initial_count{0};
    std::int32_t maximum_count{0x7fffffff};
    std::string name;
};

struct EventOptions {
    // A manual-reset event stays signalled until reset(); an auto-reset event
    // releases exactly one waiter per signal.
    bool manual_reset{false};
    bool initial_state{false};
    std::string name;
};

// Backend identity token. Wait operations that combine two objects (condition
// variable + mutex) verify that both came from the same ISystem instance. Each
// backend returns the address of its own static token.

class IMutex {
  public:
    virtual ~IMutex() = default;

    virtual void lock() = 0;
    virtual bool try_lock() = 0;
    virtual void unlock() = 0;

    // Address of the native object: CRITICAL_SECTION / SRWLOCK / pthread
    // mutex. Exposed for interop; owners must not destroy it.
    virtual void *native_handle() noexcept = 0;
    virtual const void *backend_token() const noexcept = 0;
};

class IConditionVariable {
  public:
    virtual ~IConditionVariable() = default;

    // Atomically releases `mutex`, waits, and reacquires it before returning.
    // `mutex` must have been created by the same backend instance.
    virtual void wait(IMutex &mutex) = 0;
    virtual bool wait_for(IMutex &mutex, Nanoseconds timeout) = 0;

    virtual void notify_one() = 0;
    virtual void notify_all() = 0;

    virtual void *native_handle() noexcept = 0;
};

class ISemaphore {
  public:
    virtual ~ISemaphore() = default;

    virtual void wait() = 0;
    virtual bool wait_for(Nanoseconds timeout) = 0;
    virtual bool try_wait() = 0;
    // Raises the count by `count` (clamped to the configured maximum).
    virtual void post(std::int32_t count = 1) = 0;

    virtual void *native_handle() noexcept = 0;
};

class IEvent {
  public:
    virtual ~IEvent() = default;

    virtual void set() = 0;
    virtual void reset() = 0;
    virtual void wait() = 0;
    virtual bool wait_for(Nanoseconds timeout) = 0;
    // For a manual-reset event this observes the kernel object. A zero-timeout
    // wait consumes an auto-reset event, so backends track that state locally;
    // a named auto-reset event signalled by another process becomes visible
    // here only after this process waits on it.
    virtual bool is_signalled() const = 0;

    virtual void *native_handle() noexcept = 0;
};

// Thread-local storage slot. Each instance owns one OS slot; the value is a
// plain void* and is never destructed by the platform layer. The destructor
// frees the slot for every thread at once, so instances must outlive their
// users. The portable backend erases recorded values for the calling thread;
// pointers set by other threads are released when those threads exit.
class IThreadLocal {
  public:
    virtual ~IThreadLocal() = default;

    virtual void *get() const = 0;
    virtual void set(void *value) = 0;

    // Backend-specific slot/keys handle: TlsAlloc index or pthread key.
    virtual std::uintptr_t native_key() const noexcept = 0;
};

// A user-scheduled execution context. Windows implements this with fibers;
// backends without fiber support report Capability::Fibers and return
// NotSupported from ISystem::create_fiber.
class IFiber {
  public:
    virtual ~IFiber() = default;

    // Switches the calling (converted) thread into this fiber. Control returns
    // when the fiber finishes or calls ISystem::yield_fiber().
    virtual void resume() = 0;
    virtual bool finished() const noexcept = 0;
    virtual void *native_handle() noexcept = 0;
};

using FiberFunction = std::function<void()>;

// ---------------------------------------------------------------------------
// Header-only portable primitives for hot paths.
// ---------------------------------------------------------------------------

// A minimal test-and-set spin lock. Extends the lock as an RAII guard through
// std::lock_guard<SpinLock>.
class SpinLock {
  public:
    SpinLock() = default;
    SpinLock(const SpinLock &) = delete;
    SpinLock &operator=(const SpinLock &) = delete;

    void lock() noexcept {
        while (locked_.exchange(true, std::memory_order_acquire)) {
            while (locked_.load(std::memory_order_relaxed))
                cpu_relax();
        }
    }

    bool try_lock() noexcept {
        return !locked_.exchange(true, std::memory_order_acquire);
    }

    void unlock() noexcept {
        locked_.store(false, std::memory_order_release);
    }

  private:
    std::atomic<bool> locked_{false};
};

// Portable one-time initialisation. std::call_once is already implemented on
// every supported standard library; wrapping it keeps the platform API
// symmetrical with the kernel-backed primitives above.
class OnceFlag {
  public:
    OnceFlag() = default;
    OnceFlag(const OnceFlag &) = delete;
    OnceFlag &operator=(const OnceFlag &) = delete;

    template <class Function> void call_once(Function &&function) {
        std::call_once(flag_, std::forward<Function>(function));
    }

  private:
    std::once_flag flag_;
};

} // namespace shinkou::platform
