#pragma once

// Shinkou Engine - OS thread handle.
//
// The engine's job system owns its own worker pool; this interface exists for
// long-lived platform threads that need kernel-level control (stack size,
// affinity, priority, debugger names) and for tools that must run a background
// thread outside the job system.

#include "shinkou/platform/Error.h"
#include "shinkou/platform/Platform.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace shinkou::platform {

enum class ThreadPriority : std::uint8_t {
    Idle = 0,
    Lowest,
    BelowNormal,
    Normal,
    AboveNormal,
    Highest,
    TimeCritical,
};

SHINKOU_API const char *to_string(ThreadPriority value) noexcept;

struct ThreadDesc {
    std::string name;
    // 0 selects the platform default stack size.
    std::size_t stack_size{0};
    ThreadPriority priority{ThreadPriority::Normal};
    // 0 inherits the creating thread's affinity; otherwise a processor mask.
    // Only consulted when Capability::ThreadAffinity is present.
    std::uint64_t affinity_mask{0};
};

using ThreadFunction = std::function<void()>;

// Move-only handle to a running native thread. The destructor detaches a
// still-joinable thread instead of blocking, so a thread object can safely
// outlive the code that started it; call join()/join_for() explicitly when the
// result must be observed.
class IThread {
  public:
    virtual ~IThread() = default;

    virtual std::uint64_t id() const noexcept = 0;
    virtual bool joinable() const noexcept = 0;
    virtual void join() = 0;
    // Waits up to `timeout` for the thread to finish. Returns true when the
    // thread ended (and the handle was joined), false on timeout.
    virtual bool join_for(Nanoseconds timeout) = 0;
    virtual void detach() = 0;

    virtual bool set_priority(ThreadPriority priority) = 0;
    virtual ThreadPriority priority() const = 0;

    virtual bool set_affinity(std::uint64_t processor_mask) = 0;
    virtual std::uint64_t affinity() const = 0;

    virtual bool set_name(std::string_view name) = 0;

    virtual void *native_handle() noexcept = 0;
};

} // namespace shinkou::platform
