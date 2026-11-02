// POSIX loaded-module introspection. Linux uses dl_iterate_phdr; the other
// POSIX targets keep the portable behaviour (no enumeration, no capability).

#include "PosixConfig.h"

#include "PosixBackend.h"
#include "PosixCommon.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <link.h>
#include <unistd.h>
#endif

namespace shinkou::platform::detail {

#if defined(__linux__)
namespace {

std::string main_executable_path() {
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
}

std::string base_name(const std::string &path) {
    const std::size_t separator = path.find_last_of('/');
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

int collect_module(dl_phdr_info *info, std::size_t, void *data) {
    auto *modules = static_cast<std::vector<ModuleInfo> *>(data);
    ModuleInfo module;
    if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0')
        module.path = info->dlpi_name;
    else
        module.path = main_executable_path();
    module.name = base_name(module.path);
    module.base_address = reinterpret_cast<void *>(info->dlpi_addr);
    std::size_t size = 0;
    for (int index = 0; index < info->dlpi_phnum; ++index) {
        const auto &header = info->dlpi_phdr[index];
        if (header.p_type == PT_LOAD)
            size = std::max<std::size_t>(
                size, static_cast<std::size_t>(header.p_vaddr + header.p_memsz));
    }
    module.size = size;
    modules->push_back(std::move(module));
    return 0;
}

} // namespace

std::vector<ModuleInfo> PosixSystem::modules() const {
    std::vector<ModuleInfo> modules;
    dl_iterate_phdr(&collect_module, &modules);
    return modules;
}
#else
std::vector<ModuleInfo> PosixSystem::modules() const {
    return {};
}
#endif

std::string PosixSystem::module_for_address(const void *address) const {
    if (address == nullptr)
        return {};
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    for (const ModuleInfo &module : modules()) {
        if (module.size == 0)
            continue;
        const auto base = reinterpret_cast<std::uintptr_t>(module.base_address);
        if (target >= base && target < base + module.size)
            return module.name;
    }
    return {};
}

} // namespace shinkou::platform::detail
