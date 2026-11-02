#pragma once

// POSIX backend: the portable standard-library baseline plus the native kernel
// services the standard library cannot express (mmap reservations, file/shared
// mappings, pthread scheduling control, fibers, posix_spawn, dl_iterate_phdr,
// signal-based crash reporting).
//
// Built on Linux, macOS and the BSDs. The Windows build excludes this
// directory; the portable backend remains the baseline on every platform.

#include "shinkou/platform/System.h"

#include "../portable/PortableBackend.h"

#if !defined(SHINKOU_PLATFORM_HAS_POSIX_BACKEND)
#error "PosixBackend.h is only available on POSIX builds with the native backend"
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::platform::detail {

class PosixSystem final : public PortableSystem {
  public:
    PosixSystem();

    BackendKind backend() const noexcept override;
    const char *backend_name() const noexcept override;
    std::uint64_t current_thread_id() const noexcept override;

    // Threads.
    Result<std::unique_ptr<IThread>> create_thread(const ThreadDesc &desc,
                                                   ThreadFunction function) override;
    std::uint64_t current_thread_affinity() const override;
    bool set_current_thread_affinity(std::uint64_t processor_mask) override;
    bool set_current_thread_name(std::string_view name) override;
    bool set_current_thread_priority(ThreadPriority priority) override;
    ThreadPriority current_thread_priority() const override;

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

    // Processes.
    Result<std::unique_ptr<IProcess>> spawn_process(const ProcessOptions &options) override;

    // Modules.
    std::vector<ModuleInfo> modules() const override;
    std::string module_for_address(const void *address) const override;

    // Diagnostics.
    std::size_t capture_stack_trace(StackFrame *frames, std::size_t capacity,
                                    std::size_t skip) override;
    Status install_crash_handler(CrashHandler handler, void *user_data) override;
    void uninstall_crash_handler() override;
};

std::unique_ptr<ISystem> make_posix_system();

} // namespace shinkou::platform::detail
