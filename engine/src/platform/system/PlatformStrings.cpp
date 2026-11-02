// Single translation unit for the platform enums' diagnostic names. Keeping
// them together avoids one tiny .cpp per header while still keeping the
// strings out of the headers.

#include "shinkou/platform/Error.h"
#include "shinkou/platform/SystemInfo.h"
#include "shinkou/platform/Thread.h"

#include <cstdio>
#include <string>

namespace shinkou::platform {



const char *to_string(OperatingSystem value) noexcept {
    switch (value) {
    case OperatingSystem::Unknown:
        return "Unknown";
    case OperatingSystem::Windows:
        return "Windows";
    case OperatingSystem::Linux:
        return "Linux";
    case OperatingSystem::MacOS:
        return "macOS";
    case OperatingSystem::Android:
        return "Android";
    case OperatingSystem::FreeBSD:
        return "FreeBSD";
    case OperatingSystem::OpenBSD:
        return "OpenBSD";
    case OperatingSystem::NetBSD:
        return "NetBSD";
    case OperatingSystem::Emscripten:
        return "Emscripten";
    }
    return "Unknown";
}

const char *to_string(CpuArchitecture value) noexcept {
    switch (value) {
    case CpuArchitecture::Unknown:
        return "Unknown";
    case CpuArchitecture::X86:
        return "x86";
    case CpuArchitecture::X64:
        return "x64";
    case CpuArchitecture::Arm32:
        return "arm32";
    case CpuArchitecture::Arm64:
        return "arm64";
    case CpuArchitecture::RiscV64:
        return "riscv64";
    case CpuArchitecture::Wasm32:
        return "wasm32";
    case CpuArchitecture::Wasm64:
        return "wasm64";
    }
    return "Unknown";
}

const char *to_string(BackendKind value) noexcept {
    switch (value) {
    case BackendKind::Auto:
        return "Auto";
    case BackendKind::Native:
        return "Native";
    case BackendKind::Portable:
        return "Portable";
    }
    return "Auto";
}

const char *to_string(ThreadPriority value) noexcept {
    switch (value) {
    case ThreadPriority::Idle:
        return "Idle";
    case ThreadPriority::Lowest:
        return "Lowest";
    case ThreadPriority::BelowNormal:
        return "BelowNormal";
    case ThreadPriority::Normal:
        return "Normal";
    case ThreadPriority::AboveNormal:
        return "AboveNormal";
    case ThreadPriority::Highest:
        return "Highest";
    case ThreadPriority::TimeCritical:
        return "TimeCritical";
    }
    return "Normal";
}

std::string format_iso8601(const CalendarTime &value) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03d", value.year,
                  value.month, value.day, value.hour, value.minute, value.second,
                  value.millisecond);
    return std::string{buffer};
}

} // namespace shinkou::platform
