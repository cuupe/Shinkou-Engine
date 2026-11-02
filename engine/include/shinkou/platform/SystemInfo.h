#pragma once

// Shinkou Engine - static and runtime facts about the host operating system.
//
// SystemInfo is a snapshot filled once by a backend. It is deliberately plain
// data so it can be copied, logged and stored in tests without touching the
// kernel again. Query the live values (free memory, time) through ISystem.

#include "shinkou/platform/Error.h"
#include "shinkou/platform/Platform.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace shinkou::platform {

enum class OperatingSystem : std::uint8_t {
    Unknown = 0,
    Windows,
    Linux,
    MacOS,
    Android,
    FreeBSD,
    OpenBSD,
    NetBSD,
    Emscripten,
};

enum class CpuArchitecture : std::uint8_t {
    Unknown = 0,
    X86,
    X64,
    Arm32,
    Arm64,
    RiscV64,
    Wasm32,
    Wasm64,
};

// Selects which implementation make_system() builds. Auto picks the native
// backend when the platform supports it and transparently falls back to the
// portable backend otherwise.
enum class BackendKind : std::uint8_t { Auto = 0, Native, Portable };

SHINKOU_API const char *to_string(OperatingSystem value) noexcept;
SHINKOU_API const char *to_string(CpuArchitecture value) noexcept;
SHINKOU_API const char *to_string(BackendKind value) noexcept;

// Optional kernel or OS features that are not available everywhere. Callers
// must check supports() before relying on them; the portable backend reports a
// narrower set than the Windows backend by design.
enum class Capability : std::uint32_t {
    None = 0,
    VirtualMemory = 1u << 0,   // reserve/commit/protect/release kernel pages
    LargePages = 1u << 1,      // allocate pages backed by a large-page pool
    PageLock = 1u << 2,        // pin committed pages in physical memory
    FileMapping = 1u << 3,     // map a regular file into the address space
    SharedMemory = 1u << 4,    // named, cross-process memory mapping
    FileWatch = 1u << 5,       // asynchronous directory change notifications
    Fibers = 1u << 6,          // user-scheduled execution contexts
    ThreadAffinity = 1u << 7,  // per-thread processor affinity masks
    ThreadPriority = 1u << 8,  // per-thread scheduling priority
    ThreadNaming = 1u << 9,    // per-thread debugger display name
    ThreadLocalStorage = 1u << 10,
    NamedKernelObjects = 1u << 11, // mutex/event/semaphore visible by name
    ProcessSpawn = 1u << 12,
    ProcessCapture = 1u << 13,     // capture stdout/stderr through pipes
    ProcessEnvironment = 1u << 14,
    DynamicModules = 1u << 15,
    ModuleEnumeration = 1u << 16,
    StackTrace = 1u << 17,
    CrashHandler = 1u << 18,
    ConsoleColors = 1u << 19,
    PhysicalCoreCount = 1u << 20,
    CpuBrand = 1u << 21,
    WallClockLocalTime = 1u << 22,
};

constexpr std::uint32_t capability_mask(Capability value) noexcept {
    return static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t operator|(Capability left, Capability right) noexcept {
    return capability_mask(left) | capability_mask(right);
}

constexpr std::uint32_t operator|(std::uint32_t left, Capability right) noexcept {
    return left | capability_mask(right);
}

// Broken-down calendar time in the local or UTC time zone.
struct CalendarTime {
    std::int32_t year{1970};
    std::int32_t month{1};
    std::int32_t day{1};
    std::int32_t hour{0};
    std::int32_t minute{0};
    std::int32_t second{0};
    std::int32_t millisecond{0};
    // 0 = Sunday .. 6 = Saturday, matching struct tm / SYSTEMTIME.
    std::int32_t day_of_week{4};
    // 1-based day of the year.
    std::int32_t day_of_year{1};
    bool daylight_saving{false};
    // Offset applied to UTC to obtain this calendar time, in seconds.
    std::int32_t utc_offset_seconds{0};
};

SHINKOU_API std::string format_iso8601(const CalendarTime &value);

struct MemoryStatus {
    std::uint64_t total_physical{0};
    std::uint64_t available_physical{0};
    std::uint64_t total_virtual{0};
    std::uint64_t available_virtual{0};
};

// A stable snapshot of the host. Fields that a backend cannot observe stay at
// their documented default rather than being guessed.
struct SystemInfo {
    OperatingSystem os{OperatingSystem::Unknown};
    CpuArchitecture architecture{CpuArchitecture::Unknown};
    BackendKind backend{BackendKind::Portable};
    bool little_endian{kLittleEndian};

    // Example: "Windows 10.0.19045" / "Linux 6.8.0".
    std::string os_name;
    std::string os_version;
    std::uint32_t os_build{0};

    std::string cpu_vendor; // e.g. "GenuineIntel"
    std::string cpu_brand;  // e.g. "12th Gen Intel(R) Core(TM) i7"
    std::uint32_t logical_cores{0};
    std::uint32_t physical_cores{0};
    std::size_t cache_line_size{kCacheLineSize};

    std::size_t page_size{0};
    std::size_t allocation_granularity{0};
    std::uint64_t total_physical_memory{0};

    std::string computer_name;
    std::string user_name;

    std::string executable_path;
    std::string command_line;
    std::uint64_t process_id{0};

    std::string backend_name;
};

} // namespace shinkou::platform
