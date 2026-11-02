// POSIX backend construction: host information and capability advertisement.
// Native services live in PosixMemory.cpp, PosixThread.cpp, PosixProcess.cpp,
// PosixModules.cpp and PosixDiagnostics.cpp.

#include "PosixBackend.h"

#include "PosixCommon.h"

#include <sys/utsname.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <stdlib.h>
#include <string>
#include <utility>

#if !defined(PATH_MAX)
#define PATH_MAX 4096
#endif

namespace shinkou::platform::detail {
namespace {

#if defined(__linux__)
std::string trim(std::string value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    value.erase(0, first);
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    value.erase(last + 1);
    return value;
}

std::string processor_info_value(const char *key) {
    std::ifstream file{"/proc/cpuinfo"};
    if (!file)
        return {};
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind(key, 0) != 0)
            continue;
        const std::size_t separator = line.find(':');
        if (separator == std::string::npos)
            continue;
        return trim(line.substr(separator + 1));
    }
    return {};
}

std::uint32_t linux_physical_core_count() {
    std::ifstream file{"/proc/cpuinfo"};
    if (!file)
        return 0;
    std::set<std::pair<std::string, std::string>> cores;
    std::string physical_id;
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("physical id", 0) == 0) {
            const std::size_t separator = line.find(':');
            if (separator != std::string::npos)
                physical_id = trim(line.substr(separator + 1));
        } else if (line.rfind("core id", 0) == 0) {
            const std::size_t separator = line.find(':');
            if (separator != std::string::npos) {
                const std::string core_id = trim(line.substr(separator + 1));
                if (!physical_id.empty() && !core_id.empty())
                    cores.emplace(physical_id, core_id);
            }
        }
    }
    return static_cast<std::uint32_t>(cores.size());
}
#endif

#if defined(__APPLE__)
std::string sysctl_string(const char *name) {
    std::size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0)
        return {};
    std::string value(size, '\0');
    if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0)
        return {};
    if (!value.empty() && value.back() == '\0')
        value.pop_back();
    return value;
}

std::uint64_t sysctl_unsigned(const char *name) {
    std::uint64_t value = 0;
    std::size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0)
        return 0;
    return value;
}
#endif

std::string host_name() {
    utsname info{};
    if (uname(&info) != 0)
        return {};
    return info.sysname;
}

std::string host_release() {
    utsname info{};
    if (uname(&info) != 0)
        return {};
    return info.release;
}

std::string executable_path() {
#if defined(__linux__)
    std::string buffer(4096, '\0');
    for (;;) {
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0)
            return {};
        if (static_cast<std::size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<std::size_t>(length));
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    char buffer[PATH_MAX]{};
    std::uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0)
        return {};
    char resolved[PATH_MAX]{};
    if (realpath(buffer, resolved) != nullptr)
        return resolved;
    return buffer;
#else
    return {};
#endif
}

std::string command_line() {
#if defined(__linux__)
    std::ifstream file{"/proc/self/cmdline", std::ios::binary};
    if (!file)
        return {};
    std::string line{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    for (char &character : line) {
        if (character == '\0')
            character = ' ';
    }
    while (!line.empty() && line.back() == ' ')
        line.pop_back();
    return line;
#elif defined(__APPLE__)
    const int argument_count = *_NSGetArgc();
    char **arguments = *_NSGetArgv();
    if (argument_count <= 0 || arguments == nullptr)
        return {};
    std::string line;
    for (int index = 0; index < argument_count; ++index) {
        if (index != 0)
            line.push_back(' ');
        line += arguments[index] != nullptr ? arguments[index] : "";
    }
    return line;
#else
    return {};
#endif
}

} // namespace

PosixSystem::PosixSystem() : PortableSystem() {
    info_.backend = BackendKind::Native;
    info_.backend_name = "POSIX";

    const std::string os_name = host_name();
    if (!os_name.empty())
        info_.os_name = os_name;
    const std::string release = host_release();
    if (!release.empty())
        info_.os_version = release;

    std::string cpu_vendor;
    std::string cpu_brand;
    std::uint32_t physical_cores = 0;
    std::size_t cache_line = 0;

#if defined(__linux__)
    cpu_vendor = processor_info_value("vendor_id");
    cpu_brand = processor_info_value("model name");
    physical_cores = linux_physical_core_count();
#elif defined(__APPLE__)
    cpu_vendor = sysctl_string("machdep.cpu.vendor");
    cpu_brand = sysctl_string("machdep.cpu.brand_string");
    if (cpu_brand.empty()) {
        cpu_brand = sysctl_string("hw.model");
        if (cpu_vendor.empty())
            cpu_vendor = "Apple";
    }
    physical_cores = static_cast<std::uint32_t>(sysctl_unsigned("hw.physicalcpu"));
    cache_line = static_cast<std::size_t>(sysctl_unsigned("hw.cachelinesize"));
#endif

    if (!cpu_vendor.empty())
        info_.cpu_vendor = cpu_vendor;
    if (!cpu_brand.empty())
        info_.cpu_brand = cpu_brand;
    if (physical_cores > 0)
        info_.physical_cores = physical_cores;
    if (info_.physical_cores == 0)
        info_.physical_cores = info_.logical_cores;
    if (cache_line > 0)
        info_.cache_line_size = cache_line;

    const std::string executable = executable_path();
    if (!executable.empty())
        info_.executable_path = executable;
    info_.command_line = command_line();

    std::uint32_t capabilities = capabilities_;
    capabilities |= capability_mask(Capability::FileMapping);
    capabilities |= capability_mask(Capability::SharedMemory);
    capabilities |= capability_mask(Capability::Fibers);
    capabilities |= capability_mask(Capability::ProcessCapture);
    capabilities |= capability_mask(Capability::CrashHandler);
#if defined(SHINKOU_POSIX_HAS_EXECINFO)
    capabilities |= capability_mask(Capability::StackTrace);
#endif
#if defined(__linux__)
    capabilities |= capability_mask(Capability::ModuleEnumeration);
    capabilities |= capability_mask(Capability::ThreadAffinity);
    capabilities |= capability_mask(Capability::ThreadNaming);
#endif
    if (info_.physical_cores > 0)
        capabilities |= capability_mask(Capability::PhysicalCoreCount);
    if (!info_.cpu_brand.empty())
        capabilities |= capability_mask(Capability::CpuBrand);
    capabilities_ = capabilities;
}

BackendKind PosixSystem::backend() const noexcept {
    return BackendKind::Native;
}

const char *PosixSystem::backend_name() const noexcept {
    return "POSIX";
}

std::uint64_t PosixSystem::current_thread_id() const noexcept {
    return posix_thread_id_value();
}

std::unique_ptr<ISystem> make_posix_system() {
    return std::make_unique<PosixSystem>();
}

} // namespace shinkou::platform::detail
