#pragma once

// Windows native backend. The ISystem subclasses and the per-feature object
// factories are internal: only make_windows_system() is reachable from the
// engine. Method definitions live in the Windows*.cpp files next to this
// header, grouped by area.

#include "shinkou/platform/System.h"

#if !defined(SHINKOU_PLATFORM_WINDOWS)
#error "WindowsBackend.h is only available on Windows builds"
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace shinkou::platform::detail {

class WindowsSystem final : public ISystem {
  public:
    WindowsSystem();

    WindowsSystem(const WindowsSystem &) = delete;
    WindowsSystem &operator=(const WindowsSystem &) = delete;

    // Identity and capabilities.
    BackendKind backend() const noexcept override;
    const char *backend_name() const noexcept override;
    const SystemInfo &info() const noexcept override;
    std::uint32_t capabilities() const noexcept override;

    // Clock and scheduling.
    Nanoseconds monotonic_time() const noexcept override;
    Nanoseconds wall_time() const noexcept override;
    CalendarTime calendar_time(bool local) const override;
    Nanoseconds timer_resolution() const noexcept override;
    void sleep_for(Nanoseconds duration) override;
    void yield_thread() override;
    Nanoseconds uptime() const override;

    // Topology and memory pressure.
    std::uint64_t current_thread_id() const noexcept override;
    std::uint64_t current_process_id() const noexcept override;
    std::size_t hardware_thread_count() const noexcept override;
    std::size_t page_size() const noexcept override;
    std::size_t allocation_granularity() const noexcept override;
    MemoryStatus memory_status() const override;

    // Threads.
    Result<std::unique_ptr<IThread>> create_thread(const ThreadDesc &desc,
                                                   ThreadFunction function) override;
    std::uint64_t current_thread_affinity() const override;
    bool set_current_thread_affinity(std::uint64_t processor_mask) override;
    bool set_current_thread_name(std::string_view name) override;
    bool set_current_thread_priority(ThreadPriority priority) override;
    ThreadPriority current_thread_priority() const override;
    Result<std::unique_ptr<IThreadLocal>> create_thread_local() override;

    // Kernel synchronization objects.
    Result<std::unique_ptr<IMutex>> create_mutex(const MutexOptions &options) override;
    Result<std::unique_ptr<IConditionVariable>> create_condition_variable() override;
    Result<std::unique_ptr<ISemaphore>> create_semaphore(const SemaphoreOptions &options) override;
    Result<std::unique_ptr<IEvent>> create_event(const EventOptions &options) override;

    // Fibers.
    bool convert_current_thread_to_fiber() override;
    bool revert_current_thread_from_fiber() override;
    bool current_thread_is_fiber() const noexcept override;
    Result<std::unique_ptr<IFiber>> create_fiber(FiberFunction function) override;
    void yield_fiber() override;

    // Virtual memory.
    Result<std::unique_ptr<IReservedRegion>> reserve_memory(std::size_t bytes,
                                                            MemoryFlags flags) override;
    Result<std::unique_ptr<IMappedRegion>> map_file(const std::string &path, std::size_t offset,
                                                    std::size_t bytes,
                                                    MemoryProtection protection) override;
    Result<std::unique_ptr<IMappedRegion>> create_shared_memory(const std::string &name,
                                                                std::size_t bytes,
                                                                MemoryProtection protection) override;
    Result<std::unique_ptr<IMappedRegion>> open_shared_memory(const std::string &name,
                                                              std::size_t bytes,
                                                              MemoryProtection protection) override;

    // Files and directories.
    Result<std::unique_ptr<IFileStream>> open_file(const std::string &path,
                                                   const OpenOptions &options) override;
    Result<std::unique_ptr<IDirectoryIterator>>
    enumerate_directory(const std::string &path) override;
    Result<FileInfo> stat(const std::string &path) override;
    bool exists(const std::string &path) override;
    Status create_directory(const std::string &path, bool recursive) override;
    Status remove_file(const std::string &path) override;
    Status remove_directory(const std::string &path, bool recursive) override;
    Status rename(const std::string &from, const std::string &to) override;
    Status copy_file(const std::string &from, const std::string &to, bool overwrite) override;
    std::string current_directory() const override;
    Status set_current_directory(const std::string &path) override;
    std::string temp_directory() const override;
    Result<std::string> create_temp_file(const std::string &prefix) override;
    Result<std::unique_ptr<IFileWatcher>> watch_directory(const std::string &path,
                                                          bool recursive) override;

    // Processes and environment.
    Result<std::unique_ptr<IProcess>> spawn_process(const ProcessOptions &options) override;
    std::string environment_variable(const std::string &name) const override;
    Status set_environment_variable(const std::string &name, const std::string &value) override;
    Status unset_environment_variable(const std::string &name) override;
    std::vector<EnvEntry> environment() const override;

    // Dynamic modules.
    Result<std::unique_ptr<ILibrary>> load_library(const std::string &path) override;
    std::vector<ModuleInfo> modules() const override;
    std::string module_for_address(const void *address) const override;

    // Diagnostics.
    void debug_output(std::string_view text) override;
    bool is_debugger_present() const override;
    void debug_break() override;
    std::size_t capture_stack_trace(StackFrame *frames, std::size_t capacity,
                                    std::size_t skip) override;
    Status install_crash_handler(CrashHandler handler, void *user_data) override;
    void uninstall_crash_handler() override;

    // Console.
    Status write_console(std::string_view text, StandardStream stream) override;
    bool is_console_terminal(StandardStream stream) const override;
    bool set_console_colors_enabled(bool enabled) override;

  private:
    SystemInfo info_;
    std::uint32_t capabilities_{0};
    std::uint64_t qpc_frequency_{0};
    Nanoseconds timer_resolution_{kNanosecondsPerMillisecond};
    std::size_t page_size_{4096};
    std::size_t allocation_granularity_{65536};
};

std::unique_ptr<ISystem> make_windows_system();

// Per-feature factories used by WindowsSystem; each is implemented next to the
// native object it builds.
Result<std::unique_ptr<IMutex>> make_windows_mutex(const MutexOptions &options);
Result<std::unique_ptr<IConditionVariable>> make_windows_condition_variable();
Result<std::unique_ptr<ISemaphore>> make_windows_semaphore(const SemaphoreOptions &options);
Result<std::unique_ptr<IEvent>> make_windows_event(const EventOptions &options);
Result<std::unique_ptr<IThreadLocal>> make_windows_thread_local();
Result<std::unique_ptr<IThread>> make_windows_thread(const ThreadDesc &desc,
                                                     ThreadFunction function);
Result<std::unique_ptr<IFiber>> make_windows_fiber(FiberFunction function, void *return_fiber);

Result<std::unique_ptr<IReservedRegion>> make_windows_reserved_region(std::size_t bytes,
                                                                      MemoryFlags flags);
Result<std::unique_ptr<IMappedRegion>> make_windows_file_mapping(const std::string &path,
                                                                 std::size_t offset,
                                                                 std::size_t bytes,
                                                                 MemoryProtection protection);
Result<std::unique_ptr<IMappedRegion>> make_windows_shared_memory(const std::string &name,
                                                                  std::size_t bytes,
                                                                  MemoryProtection protection,
                                                                  bool create);

Result<std::unique_ptr<IFileStream>> make_windows_file_stream(const std::string &path,
                                                              const OpenOptions &options);
Result<std::unique_ptr<IDirectoryIterator>> make_windows_directory_iterator(const std::string &path);
Result<std::unique_ptr<IFileWatcher>> make_windows_file_watcher(const std::string &path,
                                                                bool recursive);

Result<std::unique_ptr<IProcess>> make_windows_process(const ProcessOptions &options);
Result<std::unique_ptr<ILibrary>> make_windows_library(const std::string &path);

} // namespace shinkou::platform::detail
