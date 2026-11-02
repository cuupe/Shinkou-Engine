#pragma once

// Shinkou Engine - the unified operating-system interface.
//
// One ISystem instance exposes the complete native surface used by the engine:
// time and scheduling, kernel virtual memory, threads and synchronization, TLS
// and fibers, files and directory watching, child processes and the
// environment, dynamic modules and kernel-level diagnostics/console access.
//
// Two backends ship with the engine:
//   * Native   - Windows kernel objects (VirtualAlloc, SRWLOCK, fibers, named
//                mutexes/events/semaphores, ReadDirectoryChangesW, ...).
//   * Portable - C++17 standard facilities (std::thread, std::mutex,
//                std::filesystem, ...). It builds and runs on every platform,
//                including Windows, and honestly reports the features it
//                cannot provide through supports().
//
// The interface is additive: existing subsystems keep their current code paths
// until they are migrated deliberately. See engine/docs/Platform.md.

#include "shinkou/platform/Diagnostics.h"
#include "shinkou/platform/Error.h"
#include "shinkou/platform/File.h"
#include "shinkou/platform/Library.h"
#include "shinkou/platform/Memory.h"
#include "shinkou/platform/Platform.h"
#include "shinkou/platform/Process.h"
#include "shinkou/platform/Sync.h"
#include "shinkou/platform/SystemInfo.h"
#include "shinkou/platform/Thread.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::platform {

class ISystem {
  public:
    virtual ~ISystem() = default;

    // -------------------------------------------------------------------
    // Identity and capabilities
    // -------------------------------------------------------------------
    virtual BackendKind backend() const noexcept = 0;
    virtual const char *backend_name() const noexcept = 0;
    virtual const SystemInfo &info() const noexcept = 0;
    virtual std::uint32_t capabilities() const noexcept = 0;

    bool supports(Capability capability) const noexcept {
        return (capabilities() & capability_mask(capability)) != 0;
    }

    // -------------------------------------------------------------------
    // Clock and scheduling (kernel timer + scheduler yields)
    // -------------------------------------------------------------------
    // Monotonic nanoseconds since an unspecified epoch; use for intervals.
    virtual Nanoseconds monotonic_time() const noexcept = 0;
    // Nanoseconds since the Unix epoch, UTC.
    virtual Nanoseconds wall_time() const noexcept = 0;
    virtual CalendarTime calendar_time(bool local) const = 0;
    // Smallest sleep granularity the timer can honour reliably.
    virtual Nanoseconds timer_resolution() const noexcept = 0;
    virtual void sleep_for(Nanoseconds duration) = 0;
    virtual void yield_thread() = 0;
    virtual Nanoseconds uptime() const = 0;

    // -------------------------------------------------------------------
    // Process/thread identity, topology and memory pressure
    // -------------------------------------------------------------------
    virtual std::uint64_t current_thread_id() const noexcept = 0;
    virtual std::uint64_t current_process_id() const noexcept = 0;
    virtual std::size_t hardware_thread_count() const noexcept = 0;
    virtual std::size_t page_size() const noexcept = 0;
    virtual std::size_t allocation_granularity() const noexcept = 0;
    virtual MemoryStatus memory_status() const = 0;

    // -------------------------------------------------------------------
    // Threads
    // -------------------------------------------------------------------
    virtual Result<std::unique_ptr<IThread>> create_thread(const ThreadDesc &desc,
                                                           ThreadFunction function) = 0;

    virtual std::uint64_t current_thread_affinity() const = 0;
    virtual bool set_current_thread_affinity(std::uint64_t processor_mask) = 0;
    virtual bool set_current_thread_name(std::string_view name) = 0;
    virtual bool set_current_thread_priority(ThreadPriority priority) = 0;
    virtual ThreadPriority current_thread_priority() const = 0;

    // Thread-local storage slot owned by this instance.
    virtual Result<std::unique_ptr<IThreadLocal>> create_thread_local() = 0;

    // -------------------------------------------------------------------
    // Kernel synchronization objects
    // -------------------------------------------------------------------
    virtual Result<std::unique_ptr<IMutex>> create_mutex(const MutexOptions &options = {}) = 0;
    virtual Result<std::unique_ptr<IConditionVariable>> create_condition_variable() = 0;
    virtual Result<std::unique_ptr<ISemaphore>>
    create_semaphore(const SemaphoreOptions &options = {}) = 0;
    virtual Result<std::unique_ptr<IEvent>> create_event(const EventOptions &options = {}) = 0;

    // -------------------------------------------------------------------
    // Fibers (user-scheduled contexts on a converted thread)
    // -------------------------------------------------------------------
    virtual bool convert_current_thread_to_fiber() = 0;
    virtual bool revert_current_thread_from_fiber() = 0;
    virtual bool current_thread_is_fiber() const noexcept = 0;
    virtual Result<std::unique_ptr<IFiber>> create_fiber(FiberFunction function) = 0;
    virtual void yield_fiber() = 0;

    // -------------------------------------------------------------------
    // Virtual memory
    // -------------------------------------------------------------------
    virtual Result<std::unique_ptr<IReservedRegion>>
    reserve_memory(std::size_t bytes, MemoryFlags flags = MemoryFlags::None) = 0;

    // Maps `bytes` (0 = to the end of the file) starting at `offset`.
    virtual Result<std::unique_ptr<IMappedRegion>>
    map_file(const std::string &path, std::size_t offset, std::size_t bytes,
             MemoryProtection protection) = 0;

    virtual Result<std::unique_ptr<IMappedRegion>>
    create_shared_memory(const std::string &name, std::size_t bytes,
                         MemoryProtection protection) = 0;
    virtual Result<std::unique_ptr<IMappedRegion>>
    open_shared_memory(const std::string &name, std::size_t bytes,
                       MemoryProtection protection) = 0;

    // -------------------------------------------------------------------
    // Files and directories
    // -------------------------------------------------------------------
    virtual Result<std::unique_ptr<IFileStream>>
    open_file(const std::string &path, const OpenOptions &options = {}) = 0;
    virtual Result<std::unique_ptr<IDirectoryIterator>>
    enumerate_directory(const std::string &path) = 0;
    virtual Result<FileInfo> stat(const std::string &path) = 0;
    virtual bool exists(const std::string &path) = 0;

    virtual Status create_directory(const std::string &path, bool recursive = true) = 0;
    virtual Status remove_file(const std::string &path) = 0;
    virtual Status remove_directory(const std::string &path, bool recursive = false) = 0;
    // Replaces an existing destination when the platform offers atomic replace;
    // both backends implement replace-on-rename semantics.
    virtual Status rename(const std::string &from, const std::string &to) = 0;
    virtual Status copy_file(const std::string &from, const std::string &to,
                             bool overwrite = false) = 0;

    virtual std::string current_directory() const = 0;
    virtual Status set_current_directory(const std::string &path) = 0;
    virtual std::string temp_directory() const = 0;
    // Creates a uniquely named file and returns its path. The caller owns it.
    virtual Result<std::string> create_temp_file(const std::string &prefix = "shinkou") = 0;

    virtual Result<std::unique_ptr<IFileWatcher>>
    watch_directory(const std::string &path, bool recursive = false) = 0;

    // -------------------------------------------------------------------
    // Child processes and the process environment
    // -------------------------------------------------------------------
    virtual Result<std::unique_ptr<IProcess>> spawn_process(const ProcessOptions &options) = 0;

    virtual std::string environment_variable(const std::string &name) const = 0;
    virtual Status set_environment_variable(const std::string &name,
                                            const std::string &value) = 0;
    virtual Status unset_environment_variable(const std::string &name) = 0;
    virtual std::vector<EnvEntry> environment() const = 0;

    // -------------------------------------------------------------------
    // Dynamic modules
    // -------------------------------------------------------------------
    virtual Result<std::unique_ptr<ILibrary>> load_library(const std::string &path) = 0;
    virtual std::vector<ModuleInfo> modules() const = 0;
    // Returns the module name containing `address`, or an empty string.
    virtual std::string module_for_address(const void *address) const = 0;

    // -------------------------------------------------------------------
    // Debugger and kernel diagnostics
    // -------------------------------------------------------------------
    virtual void debug_output(std::string_view text) = 0;
    virtual bool is_debugger_present() const = 0;
    virtual void debug_break() = 0;
    // Fills up to `capacity` frames, skipping `skip` innermost frames.
    virtual std::size_t capture_stack_trace(StackFrame *frames, std::size_t capacity,
                                            std::size_t skip = 0) = 0;
    // Installs a process-wide unhandled-exception handler. Backends without
    // Capability::CrashHandler return NotSupported.
    virtual Status install_crash_handler(CrashHandler handler, void *user_data = nullptr) = 0;
    virtual void uninstall_crash_handler() = 0;

    // -------------------------------------------------------------------
    // Console / standard streams
    // -------------------------------------------------------------------
    virtual Status write_console(std::string_view text,
                                 StandardStream stream = StandardStream::Output) = 0;
    virtual bool is_console_terminal(StandardStream stream = StandardStream::Output) const = 0;
    virtual bool set_console_colors_enabled(bool enabled) = 0;
};

// Backend discovery. native is available when the platform has a native
// implementation compiled in; portable is always available.
SHINKOU_API BackendKind default_backend() noexcept;
SHINKOU_API bool backend_available(BackendKind kind) noexcept;

// Builds a backend. On failure returns nullptr and fills `error` when supplied.
// Auto selects the native backend when available and the portable one
// otherwise.
SHINKOU_API std::unique_ptr<ISystem>
make_system(BackendKind kind = BackendKind::Auto, SystemError *error = nullptr);

// Process-wide instance for code that cannot receive a pointer. The first call
// lazily builds the default backend. set_instance() is intended for startup and
// tests: callers must not replace the instance while other threads are using
// the reference returned by instance().
class System final {
  public:
    static ISystem &instance();
    static ISystem *try_instance() noexcept;
    static bool has_instance() noexcept;
    // Passing nullptr resets the instance; the next instance() rebuilds the
    // default backend.
    static void set_instance(std::unique_ptr<ISystem> system);
};

} // namespace shinkou::platform
