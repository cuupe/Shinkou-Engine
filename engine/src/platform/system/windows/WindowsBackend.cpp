// Windows native backend: host information, time, topology and thread control.
// Feature-specific methods live in WindowsSync.cpp, WindowsMemory.cpp,
// WindowsFile.cpp, WindowsProcess.cpp, WindowsLibrary.cpp and
// WindowsDiagnostics.cpp.

#include "WindowsBackend.h"

#include "WindowsCommon.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#if (defined(SHINKOU_ARCH_X64) || defined(SHINKOU_ARCH_X86)) && !defined(SHINKOU_COMPILER_MSVC)
#include <cpuid.h>
#endif

namespace shinkou::platform::detail {
namespace {

// ---------------------------------------------------------------------------
// CPU identification
// ---------------------------------------------------------------------------
#if defined(SHINKOU_ARCH_X64) || defined(SHINKOU_ARCH_X86)
void cpuid_call(int leaf, int subleaf, int registers[4]) noexcept {
#if defined(SHINKOU_COMPILER_MSVC)
    __cpuidex(registers, leaf, subleaf);
#else
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    __cpuid_count(static_cast<unsigned int>(leaf), static_cast<unsigned int>(subleaf), eax, ebx, ecx,
                  edx);
    registers[0] = static_cast<int>(eax);
    registers[1] = static_cast<int>(ebx);
    registers[2] = static_cast<int>(ecx);
    registers[3] = static_cast<int>(edx);
#endif
}

int max_extended_leaf() noexcept {
    int registers[4]{};
    cpuid_call(0x80000000, 0, registers);
    return registers[0];
}
#endif

void query_cpu_identity(std::string &vendor, std::string &brand) {
#if defined(SHINKOU_ARCH_X64) || defined(SHINKOU_ARCH_X86)
    int registers[4]{};
    cpuid_call(0, 0, registers);
    char vendor_text[13]{};
    std::memcpy(vendor_text, &registers[1], 4);
    std::memcpy(vendor_text + 4, &registers[3], 4);
    std::memcpy(vendor_text + 8, &registers[2], 4);
    vendor = vendor_text;

    if (max_extended_leaf() >= static_cast<int>(0x80000004u)) {
        char brand_text[49]{};
        for (int leaf = 0; leaf < 3; ++leaf) {
            cpuid_call(0x80000002 + leaf, 0, registers);
            std::memcpy(brand_text + leaf * 16, registers, 16);
        }
        std::string text{brand_text};
        const std::size_t first = text.find_first_not_of(" \t");
        if (first != std::string::npos) {
            text.erase(0, first);
            const std::size_t last = text.find_last_not_of(" \t");
            text.erase(last + 1);
            brand = std::move(text);
        }
    }
#else
    (void)vendor;
    (void)brand;
#endif
}

// ---------------------------------------------------------------------------
// OS version
// ---------------------------------------------------------------------------
struct OsVersionInfo {
    std::uint32_t size;
    std::uint32_t major;
    std::uint32_t minor;
    std::uint32_t build;
    std::uint32_t platform;
    wchar_t service_pack[128];
};

bool query_os_version(OsVersionInfo &version) noexcept {
    version = {};
    version.size = sizeof(OsVersionInfo);
    using RtlGetVersionFn = LONG(WINAPI *)(OsVersionInfo *);
    const FARPROC address = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (address == nullptr)
        return false;
    RtlGetVersionFn get_version = nullptr;
    static_assert(sizeof(get_version) == sizeof(address));
    std::memcpy(&get_version, &address, sizeof(get_version));
    if (get_version == nullptr)
        return false;
    return get_version(&version) >= 0;
}

// ---------------------------------------------------------------------------
// Processor topology
// ---------------------------------------------------------------------------
struct ProcessorTopology {
    std::uint32_t physical_cores{0};
    std::size_t cache_line{0};
};

ProcessorTopology query_processor_topology() {
    ProcessorTopology topology;
    DWORD byte_length = 0;
    (void)GetLogicalProcessorInformation(nullptr, &byte_length);
    if (byte_length == 0)
        return topology;

    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> entries(
        byte_length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
    DWORD buffer_length = static_cast<DWORD>(entries.size() * sizeof(entries[0]));
    if (!GetLogicalProcessorInformation(entries.data(), &buffer_length))
        return topology;

    const std::size_t count = buffer_length / sizeof(entries[0]);
    for (std::size_t index = 0; index < count; ++index) {
        if (entries[index].Relationship == RelationProcessorCore)
            ++topology.physical_cores;
        else if (entries[index].Relationship == RelationCache) {
            const CACHE_DESCRIPTOR &cache = entries[index].Cache;
            if (cache.Level == 1 && topology.cache_line == 0 &&
                (cache.Type == CacheData || cache.Type == CacheUnified))
                topology.cache_line = cache.LineSize;
        }
    }
    return topology;
}

CpuArchitecture current_cpu_architecture() noexcept {
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
#else
    return CpuArchitecture::Unknown;
#endif
}

std::string computer_name() {
    wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!GetComputerNameW(buffer, &size))
        return {};
    return utf8_from_wide(buffer, size);
}

std::string environment_string(const wchar_t *name) {
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0)
        return {};
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    if (written == 0 || written >= size)
        return {};
    value.resize(written);
    return utf8_from_wide(value);
}

Nanoseconds qpc_to_nanoseconds(std::uint64_t ticks, std::uint64_t frequency) noexcept {
    if (frequency == 0)
        return 0;
    const std::uint64_t seconds = ticks / frequency;
    const std::uint64_t remainder = ticks % frequency;
    return static_cast<Nanoseconds>(seconds) * kNanosecondsPerSecond +
           static_cast<Nanoseconds>((remainder * 1000000000ULL) / frequency);
}

constexpr int days_from_civil(int year, int month, int day) noexcept {
    year -= month <= 2 ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned int year_of_era = static_cast<unsigned int>(year - era * 400);
    const unsigned int day_of_year =
        (153u * static_cast<unsigned int>(month > 2 ? month - 3 : month + 9) + 2u) / 5u +
        static_cast<unsigned int>(day) - 1u;
    const unsigned int day_of_era =
        year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;
    return era * 146097 + static_cast<int>(day_of_era) - 719468;
}

CalendarTime calendar_from_systemtime(const SYSTEMTIME &time) {
    CalendarTime result;
    result.year = time.wYear;
    result.month = time.wMonth;
    result.day = time.wDay;
    result.hour = time.wHour;
    result.minute = time.wMinute;
    result.second = time.wSecond;
    result.millisecond = time.wMilliseconds;
    result.day_of_week = time.wDayOfWeek;
    result.day_of_year =
        days_from_civil(time.wYear, time.wMonth, time.wDay) -
        days_from_civil(time.wYear, 1, 1) + 1;
    return result;
}

Nanoseconds query_timer_resolution() noexcept {
    using NtQueryTimerResolutionFn = LONG(WINAPI *)(ULONG *, ULONG *, ULONG *);
    const FARPROC address =
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryTimerResolution");
    if (address == nullptr)
        return kNanosecondsPerMillisecond;
    NtQueryTimerResolutionFn query = nullptr;
    static_assert(sizeof(query) == sizeof(address));
    std::memcpy(&query, &address, sizeof(query));
    if (query == nullptr)
        return kNanosecondsPerMillisecond;
    ULONG minimum = 0;
    ULONG maximum = 0;
    ULONG current = 0;
    if (query(&minimum, &maximum, &current) < 0 || current == 0)
        return kNanosecondsPerMillisecond;
    // NtQueryTimerResolution reports 100 ns units.
    return static_cast<Nanoseconds>(current) * 100;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
WindowsSystem::WindowsSystem() {
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    page_size_ = system_info.dwPageSize;
    allocation_granularity_ = system_info.dwAllocationGranularity;

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
    timer_resolution_ = query_timer_resolution();

    info_.os = OperatingSystem::Windows;
    info_.architecture = current_cpu_architecture();
    info_.backend = BackendKind::Native;
    info_.backend_name = "Windows Native";
    info_.little_endian = true;

    OsVersionInfo version{};
    info_.os_name = "Windows";
    if (query_os_version(version)) {
        info_.os_version = std::to_string(version.major) + "." + std::to_string(version.minor) +
                           "." + std::to_string(version.build);
        info_.os_build = version.build;
    } else {
        info_.os_version = "unknown";
    }

    query_cpu_identity(info_.cpu_vendor, info_.cpu_brand);

    const ProcessorTopology topology = query_processor_topology();
    info_.logical_cores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (info_.logical_cores == 0)
        info_.logical_cores = system_info.dwNumberOfProcessors;
    info_.physical_cores = topology.physical_cores;
    if (info_.physical_cores == 0)
        info_.physical_cores = info_.logical_cores;
    info_.cache_line_size = topology.cache_line != 0 ? topology.cache_line : kCacheLineSize;

    info_.page_size = page_size_;
    info_.allocation_granularity = allocation_granularity_;

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
        info_.total_physical_memory = memory.ullTotalPhys;

    info_.computer_name = computer_name();
    info_.user_name = environment_string(L"USERNAME");
    info_.executable_path = win32_module_path(nullptr);
    info_.command_line = utf8_from_wide(GetCommandLineW());
    info_.process_id = GetCurrentProcessId();

    std::uint32_t capabilities = capability_mask(Capability::VirtualMemory) |
                                 capability_mask(Capability::LargePages) |
                                 capability_mask(Capability::PageLock) |
                                 capability_mask(Capability::FileMapping) |
                                 capability_mask(Capability::SharedMemory) |
                                 capability_mask(Capability::FileWatch) |
                                 capability_mask(Capability::Fibers) |
                                 capability_mask(Capability::ThreadAffinity) |
                                 capability_mask(Capability::ThreadPriority) |
                                 capability_mask(Capability::ThreadLocalStorage) |
                                 capability_mask(Capability::NamedKernelObjects) |
                                 capability_mask(Capability::ProcessSpawn) |
                                 capability_mask(Capability::ProcessCapture) |
                                 capability_mask(Capability::ProcessEnvironment) |
                                 capability_mask(Capability::DynamicModules) |
                                 capability_mask(Capability::ModuleEnumeration) |
                                 capability_mask(Capability::StackTrace) |
                                 capability_mask(Capability::CrashHandler) |
                                 capability_mask(Capability::ConsoleColors) |
                                 capability_mask(Capability::WallClockLocalTime);
    if (info_.physical_cores > 0)
        capabilities |= capability_mask(Capability::PhysicalCoreCount);
    if (!info_.cpu_brand.empty())
        capabilities |= capability_mask(Capability::CpuBrand);
    if (win32_thread_naming_available())
        capabilities |= capability_mask(Capability::ThreadNaming);
    capabilities_ = capabilities;
}

std::unique_ptr<ISystem> make_windows_system() {
    return std::make_unique<WindowsSystem>();
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
BackendKind WindowsSystem::backend() const noexcept {
    return BackendKind::Native;
}

const char *WindowsSystem::backend_name() const noexcept {
    return info_.backend_name.c_str();
}

const SystemInfo &WindowsSystem::info() const noexcept {
    return info_;
}

std::uint32_t WindowsSystem::capabilities() const noexcept {
    return capabilities_;
}

// ---------------------------------------------------------------------------
// Clock and scheduling
// ---------------------------------------------------------------------------
Nanoseconds WindowsSystem::monotonic_time() const noexcept {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return qpc_to_nanoseconds(static_cast<std::uint64_t>(counter.QuadPart), qpc_frequency_);
}

Nanoseconds WindowsSystem::wall_time() const noexcept {
    FILETIME file_time{};
    GetSystemTimeAsFileTime(&file_time);
    return unix_nanoseconds_from_filetime(file_time);
}

CalendarTime WindowsSystem::calendar_time(bool local) const {
    SYSTEMTIME time{};
    if (local)
        GetLocalTime(&time);
    else
        GetSystemTime(&time);
    CalendarTime result = calendar_from_systemtime(time);

    TIME_ZONE_INFORMATION time_zone{};
    const DWORD state = GetTimeZoneInformation(&time_zone);
    if (state == TIME_ZONE_ID_INVALID)
        return result;

    result.daylight_saving = state == TIME_ZONE_ID_DAYLIGHT;
    LONG bias_minutes = time_zone.Bias;
    if (result.daylight_saving)
        bias_minutes += time_zone.DaylightBias;
    result.utc_offset_seconds = -static_cast<std::int32_t>(bias_minutes) * 60;
    return result;
}

Nanoseconds WindowsSystem::timer_resolution() const noexcept {
    return timer_resolution_;
}

void WindowsSystem::sleep_for(Nanoseconds duration) {
    if (duration <= 0) {
        if (duration == 0)
            yield_thread();
        return;
    }
    const Nanoseconds deadline = monotonic_time() + duration;
    for (;;) {
        const Nanoseconds remaining = deadline - monotonic_time();
        if (remaining <= 0)
            return;
        if (remaining > kNanosecondsPerMillisecond) {
            const auto milliseconds = remaining / kNanosecondsPerMillisecond;
            const DWORD capped = milliseconds > 0xFFFFFFFELL ? 0xFFFFFFFEu
                                                             : static_cast<DWORD>(milliseconds);
            Sleep(capped == 0 ? 1 : capped);
        } else {
            Sleep(0);
        }
    }
}

void WindowsSystem::yield_thread() {
    if (!SwitchToThread())
        Sleep(0);
}

Nanoseconds WindowsSystem::uptime() const {
    return static_cast<Nanoseconds>(GetTickCount64()) * kNanosecondsPerMillisecond;
}

// ---------------------------------------------------------------------------
// Topology and memory pressure
// ---------------------------------------------------------------------------
std::uint64_t WindowsSystem::current_thread_id() const noexcept {
    return GetCurrentThreadId();
}

std::uint64_t WindowsSystem::current_process_id() const noexcept {
    return GetCurrentProcessId();
}

std::size_t WindowsSystem::hardware_thread_count() const noexcept {
    return info_.logical_cores;
}

std::size_t WindowsSystem::page_size() const noexcept {
    return page_size_;
}

std::size_t WindowsSystem::allocation_granularity() const noexcept {
    return allocation_granularity_;
}

MemoryStatus WindowsSystem::memory_status() const {
    MemoryStatus status;
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory))
        return status;
    status.total_physical = memory.ullTotalPhys;
    status.available_physical = memory.ullAvailPhys;
    status.total_virtual = memory.ullTotalPageFile;
    status.available_virtual = memory.ullAvailPageFile;
    return status;
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IThread>> WindowsSystem::create_thread(const ThreadDesc &desc,
                                                             ThreadFunction function) {
    return make_windows_thread(desc, std::move(function));
}

std::uint64_t WindowsSystem::current_thread_affinity() const {
    GROUP_AFFINITY group{};
    if (GetThreadGroupAffinity(GetCurrentThread(), &group))
        return static_cast<std::uint64_t>(group.Mask);
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask))
        return static_cast<std::uint64_t>(process_mask);
    return 0;
}

bool WindowsSystem::set_current_thread_affinity(std::uint64_t processor_mask) {
    if (processor_mask == 0)
        return false;
    return SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(processor_mask)) != 0;
}

bool WindowsSystem::set_current_thread_name(std::string_view name) {
    return set_win32_thread_name(GetCurrentThread(), name);
}

bool WindowsSystem::set_current_thread_priority(ThreadPriority priority) {
    return SetThreadPriority(GetCurrentThread(), win32_thread_priority(priority)) != FALSE;
}

ThreadPriority WindowsSystem::current_thread_priority() const {
    return thread_priority_from_win32(GetThreadPriority(GetCurrentThread()));
}

Result<std::unique_ptr<IThreadLocal>> WindowsSystem::create_thread_local() {
    return make_windows_thread_local();
}

// ---------------------------------------------------------------------------
// Kernel synchronization objects (implemented in WindowsSync.cpp)
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IMutex>> WindowsSystem::create_mutex(const MutexOptions &options) {
    return make_windows_mutex(options);
}

Result<std::unique_ptr<IConditionVariable>> WindowsSystem::create_condition_variable() {
    return make_windows_condition_variable();
}

Result<std::unique_ptr<ISemaphore>> WindowsSystem::create_semaphore(const SemaphoreOptions &options) {
    return make_windows_semaphore(options);
}

Result<std::unique_ptr<IEvent>> WindowsSystem::create_event(const EventOptions &options) {
    return make_windows_event(options);
}

} // namespace shinkou::platform::detail
