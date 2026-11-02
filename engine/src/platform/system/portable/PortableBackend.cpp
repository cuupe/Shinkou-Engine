// Portable ISystem backend: host information, clocks, topology and the
// factory dispatch for the feature-specific portable objects.

#include "PortableBackend.h"

#include "PortableCommon.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#if !defined(SHINKOU_PLATFORM_WINDOWS)
#include <unistd.h>
#endif

namespace shinkou::platform::detail {
namespace {

std::atomic<std::uint64_t> g_next_thread_id{1};
thread_local const std::uint64_t t_portable_thread_id = g_next_thread_id.fetch_add(
    1, std::memory_order_relaxed);

Nanoseconds steady_now() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

OperatingSystem portable_operating_system() noexcept {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    return OperatingSystem::Windows;
#elif defined(SHINKOU_PLATFORM_APPLE)
    return OperatingSystem::MacOS;
#elif defined(SHINKOU_PLATFORM_ANDROID)
    return OperatingSystem::Android;
#elif defined(SHINKOU_PLATFORM_LINUX)
    return OperatingSystem::Linux;
#elif defined(__FreeBSD__)
    return OperatingSystem::FreeBSD;
#elif defined(__OpenBSD__)
    return OperatingSystem::OpenBSD;
#elif defined(__NetBSD__)
    return OperatingSystem::NetBSD;
#elif defined(SHINKOU_PLATFORM_EMSCRIPTEN)
    return OperatingSystem::Emscripten;
#else
    return OperatingSystem::Unknown;
#endif
}

CpuArchitecture portable_architecture() noexcept {
#if defined(SHINKOU_ARCH_X64)
    return CpuArchitecture::X64;
#elif defined(SHINKOU_ARCH_X86)
    return CpuArchitecture::X86;
#elif defined(SHINKOU_ARCH_ARM64)
    return CpuArchitecture::Arm64;
#elif defined(SHINKOU_ARCH_ARM)
    return CpuArchitecture::Arm32;
#elif defined(SHINKOU_ARCH_RISCV64)
    return CpuArchitecture::RiscV64;
#elif defined(SHINKOU_ARCH_WASM64)
    return CpuArchitecture::Wasm64;
#elif defined(SHINKOU_ARCH_WASM32)
    return CpuArchitecture::Wasm32;
#else
    return CpuArchitecture::Unknown;
#endif
}

MemoryStatus query_memory_status() {
    MemoryStatus status;
#if defined(SHINKOU_PLATFORM_WINDOWS)
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        status.total_physical = memory.ullTotalPhys;
        status.available_physical = memory.ullAvailPhys;
        status.total_virtual = memory.ullTotalPageFile;
        status.available_virtual = memory.ullAvailPageFile;
    }
#else
    const long total_pages = sysconf(_SC_PHYS_PAGES);
    const long page = sysconf(_SC_PAGESIZE);
    if (total_pages > 0 && page > 0)
        status.total_physical =
            static_cast<std::uint64_t>(total_pages) * static_cast<std::uint64_t>(page);
    const long available_pages = sysconf(_SC_AVPHYS_PAGES);
    if (available_pages > 0 && page > 0)
        status.available_physical =
            static_cast<std::uint64_t>(available_pages) * static_cast<std::uint64_t>(page);
#endif
    return status;
}

} // namespace

PortableSystem::PortableSystem() {
    page_size_ = portable_page_size();
    allocation_granularity_ = portable_allocation_granularity();
    boot_reference_ = steady_now();

    info_.os = portable_operating_system();
    info_.architecture = portable_architecture();
    info_.backend = BackendKind::Portable;
    info_.backend_name = "Portable (std)";
    info_.little_endian = kLittleEndian;

    info_.os_name = to_string(info_.os);
    info_.os_version = "unknown";

    info_.logical_cores = std::max<std::uint32_t>(1, std::thread::hardware_concurrency());
    info_.physical_cores = info_.logical_cores;
    info_.cache_line_size = kCacheLineSize;

    info_.page_size = page_size_;
    info_.allocation_granularity = allocation_granularity_;
    info_.total_physical_memory = query_memory_status().total_physical;

    info_.computer_name = portable_computer_name();
    info_.user_name = portable_user_name();
    info_.executable_path = portable_executable_path();
    info_.command_line = portable_command_line();
    info_.process_id = portable_process_id();

    std::uint32_t capabilities = capability_mask(Capability::VirtualMemory) |
                                 capability_mask(Capability::PageLock) |
                                 capability_mask(Capability::ThreadLocalStorage) |
                                 capability_mask(Capability::ProcessSpawn) |
                                 capability_mask(Capability::ProcessEnvironment) |
                                 capability_mask(Capability::DynamicModules) |
                                 capability_mask(Capability::FileWatch) |
                                 capability_mask(Capability::WallClockLocalTime);
#if defined(SHINKOU_PLATFORM_WINDOWS)
    capabilities |= capability_mask(Capability::StackTrace);
    capabilities |= capability_mask(Capability::ConsoleColors);
#endif
    capabilities_ = capabilities;
}

std::unique_ptr<ISystem> make_portable_system() {
    return std::make_unique<PortableSystem>();
}

BackendKind PortableSystem::backend() const noexcept {
    return BackendKind::Portable;
}

const char *PortableSystem::backend_name() const noexcept {
    return info_.backend_name.c_str();
}

const SystemInfo &PortableSystem::info() const noexcept {
    return info_;
}

std::uint32_t PortableSystem::capabilities() const noexcept {
    return capabilities_;
}

// ---------------------------------------------------------------------------
// Clock and scheduling
// ---------------------------------------------------------------------------
Nanoseconds PortableSystem::monotonic_time() const noexcept {
    return steady_now();
}

Nanoseconds PortableSystem::wall_time() const noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

CalendarTime PortableSystem::calendar_time(bool local) const {
    const Nanoseconds now = wall_time();
    const auto seconds = static_cast<std::time_t>(now / kNanosecondsPerSecond);
    std::tm time{};
    {
        // std::localtime/std::gmtime share a static buffer; copy under a lock.
        static std::mutex localtime_mutex;
        std::lock_guard<std::mutex> lock(localtime_mutex);
        const std::tm *source = local ? std::localtime(&seconds) : std::gmtime(&seconds);
        if (source == nullptr)
            return {};
        time = *source;
    }
    CalendarTime result;
    result.year = time.tm_year + 1900;
    result.month = time.tm_mon + 1;
    result.day = time.tm_mday;
    result.hour = time.tm_hour;
    result.minute = time.tm_min;
    result.second = time.tm_sec;
    result.millisecond = static_cast<std::int32_t>((now / 1000000) % 1000);
    result.day_of_week = time.tm_wday;
    result.day_of_year = time.tm_yday + 1;
    result.daylight_saving = time.tm_isdst > 0;
#if defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__)
    result.utc_offset_seconds = static_cast<std::int32_t>(time.tm_gmtoff);
#endif
    return result;
}

Nanoseconds PortableSystem::timer_resolution() const noexcept {
    return kNanosecondsPerMillisecond;
}

void PortableSystem::sleep_for(Nanoseconds duration) {
    if (duration <= 0) {
        if (duration == 0)
            yield_thread();
        return;
    }
    std::this_thread::sleep_for(std::chrono::nanoseconds{duration});
}

void PortableSystem::yield_thread() {
    std::this_thread::yield();
}

Nanoseconds PortableSystem::uptime() const {
    const Nanoseconds system_uptime = portable_uptime();
    if (system_uptime != 0)
        return system_uptime;
    return monotonic_time() - boot_reference_;
}

// ---------------------------------------------------------------------------
// Topology and memory pressure
// ---------------------------------------------------------------------------
std::uint64_t PortableSystem::current_thread_id() const noexcept {
    return t_portable_thread_id;
}

std::uint64_t PortableSystem::current_process_id() const noexcept {
    return portable_process_id();
}

std::size_t PortableSystem::hardware_thread_count() const noexcept {
    return info_.logical_cores;
}

std::size_t PortableSystem::page_size() const noexcept {
    return page_size_;
}

std::size_t PortableSystem::allocation_granularity() const noexcept {
    return allocation_granularity_;
}

MemoryStatus PortableSystem::memory_status() const {
    return query_memory_status();
}

// ---------------------------------------------------------------------------
// Thread control
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IThread>> PortableSystem::create_thread(const ThreadDesc &desc,
                                                              ThreadFunction function) {
    return make_portable_thread(desc, std::move(function));
}

std::uint64_t PortableSystem::current_thread_affinity() const {
    return 0;
}

bool PortableSystem::set_current_thread_affinity(std::uint64_t) {
    return false;
}

bool PortableSystem::set_current_thread_name(std::string_view) {
    return false;
}

bool PortableSystem::set_current_thread_priority(ThreadPriority) {
    return false;
}

ThreadPriority PortableSystem::current_thread_priority() const {
    return ThreadPriority::Normal;
}

Result<std::unique_ptr<IThreadLocal>> PortableSystem::create_thread_local() {
    return make_portable_thread_local();
}

// ---------------------------------------------------------------------------
// Synchronization objects
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IMutex>> PortableSystem::create_mutex(const MutexOptions &options) {
    return make_portable_mutex(options);
}

Result<std::unique_ptr<IConditionVariable>> PortableSystem::create_condition_variable() {
    return make_portable_condition_variable();
}

Result<std::unique_ptr<ISemaphore>> PortableSystem::create_semaphore(const SemaphoreOptions &options) {
    return make_portable_semaphore(options);
}

Result<std::unique_ptr<IEvent>> PortableSystem::create_event(const EventOptions &options) {
    return make_portable_event(options);
}

// ---------------------------------------------------------------------------
// Fibers (not available without an OS stack-switching primitive)
// ---------------------------------------------------------------------------
bool PortableSystem::convert_current_thread_to_fiber() {
    return false;
}

bool PortableSystem::revert_current_thread_from_fiber() {
    return false;
}

bool PortableSystem::current_thread_is_fiber() const noexcept {
    return false;
}

Result<std::unique_ptr<IFiber>> PortableSystem::create_fiber(FiberFunction) {
    return make_error(ErrorCode::NotSupported, 0, "fibers are not available in the portable backend");
}

void PortableSystem::yield_fiber() {
}

// ---------------------------------------------------------------------------
// Virtual memory
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IReservedRegion>> PortableSystem::reserve_memory(std::size_t bytes,
                                                                        MemoryFlags flags) {
    return make_portable_reserved_region(bytes, flags);
}

Result<std::unique_ptr<IMappedRegion>> PortableSystem::map_file(const std::string &,
                                                                std::size_t, std::size_t,
                                                                MemoryProtection) {
    return make_error(ErrorCode::NotSupported, 0,
                      "file mapping is not available in the portable backend");
}

Result<std::unique_ptr<IMappedRegion>> PortableSystem::create_shared_memory(const std::string &,
                                                                            std::size_t,
                                                                            MemoryProtection) {
    return make_error(ErrorCode::NotSupported, 0,
                      "shared memory is not available in the portable backend");
}

Result<std::unique_ptr<IMappedRegion>> PortableSystem::open_shared_memory(const std::string &,
                                                                          std::size_t,
                                                                          MemoryProtection) {
    return make_error(ErrorCode::NotSupported, 0,
                      "shared memory is not available in the portable backend");
}

// ---------------------------------------------------------------------------
// Files (implemented in PortableFile.cpp)
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IFileStream>> PortableSystem::open_file(const std::string &path,
                                                               const OpenOptions &options) {
    return make_portable_file_stream(path, options);
}

Result<std::unique_ptr<IDirectoryIterator>>
PortableSystem::enumerate_directory(const std::string &path) {
    return make_portable_directory_iterator(path);
}

std::string PortableSystem::current_directory() const {
    std::error_code error;
    const auto path = std::filesystem::current_path(error);
    if (error)
        return {};
    return portable_path_to_utf8(path);
}

std::string PortableSystem::temp_directory() const {
    std::error_code error;
    const auto path = std::filesystem::temp_directory_path(error);
    if (error)
        return {};
    return portable_path_to_utf8(path);
}

Result<std::unique_ptr<IFileWatcher>> PortableSystem::watch_directory(const std::string &path,
                                                                      bool recursive) {
    return make_portable_file_watcher(path, recursive);
}

// ---------------------------------------------------------------------------
// Processes (implemented in PortableProcess.cpp)
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IProcess>> PortableSystem::spawn_process(const ProcessOptions &options) {
    return make_portable_process(options);
}

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------
Result<std::unique_ptr<ILibrary>> PortableSystem::load_library(const std::string &path) {
    return make_portable_library(path);
}

std::vector<ModuleInfo> PortableSystem::modules() const {
    return {};
}

std::string PortableSystem::module_for_address(const void *) const {
    return {};
}

} // namespace shinkou::platform::detail
