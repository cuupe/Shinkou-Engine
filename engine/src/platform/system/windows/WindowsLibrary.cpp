// Windows native dynamic modules and loaded-module introspection.

#include "WindowsBackend.h"

#include "WindowsCommon.h"

#include <tlhelp32.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace shinkou::platform::detail {

class WindowsLibrary final : public ILibrary {
  public:
    WindowsLibrary(HMODULE module, std::string path)
        : module_(module), path_(std::move(path)) {
    }

    ~WindowsLibrary() override {
        if (module_ != nullptr)
            FreeLibrary(module_);
    }

    Result<void *> symbol(std::string_view name) override {
        if (module_ == nullptr)
            return make_error(ErrorCode::LibraryError, 0, "library is not loaded");
        if (name.empty())
            return make_error(ErrorCode::InvalidArgument, 0, "symbol name is empty");
        const std::string narrow{name};
        SetLastError(0);
        FARPROC address = GetProcAddress(module_, narrow.c_str());
        if (address == nullptr) {
            const DWORD error = GetLastError();
            return win32_error(error, ErrorCode::LibraryError,
                               ("GetProcAddress(" + narrow + ")").c_str());
        }
        void *symbol = nullptr;
        static_assert(sizeof(symbol) == sizeof(address));
        std::memcpy(&symbol, &address, sizeof(symbol));
        return symbol;
    }

    std::string path() const override {
        return path_;
    }

    void *native_handle() noexcept override {
        return module_;
    }

  private:
    HMODULE module_{nullptr};
    std::string path_;
};

Result<std::unique_ptr<ILibrary>> make_windows_library(const std::string &path) {
    if (path.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "library path is empty");
    const std::wstring wide = wide_from_utf8(path);
    if (wide.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "library path is not valid UTF-8");
    HMODULE module = LoadLibraryW(wide.c_str());
    if (module == nullptr)
        return win32_error(ErrorCode::LibraryError, "LoadLibraryW");
    return std::unique_ptr<ILibrary>(new WindowsLibrary(module, path));
}

// ---------------------------------------------------------------------------
// WindowsSystem entry points
// ---------------------------------------------------------------------------
Result<std::unique_ptr<ILibrary>> WindowsSystem::load_library(const std::string &path) {
    return make_windows_library(path);
}

std::vector<ModuleInfo> WindowsSystem::modules() const {
    std::vector<ModuleInfo> modules;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return modules;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            ModuleInfo module;
            module.name = utf8_from_wide(entry.szModule);
            module.path = utf8_from_wide(entry.szExePath);
            module.base_address = entry.modBaseAddr;
            module.size = entry.modBaseSize;
            modules.push_back(std::move(module));
            entry.dwSize = sizeof(entry);
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return modules;
}

std::string WindowsSystem::module_for_address(const void *address) const {
    if (address == nullptr)
        return {};
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    for (const ModuleInfo &module : modules()) {
        const auto base = reinterpret_cast<std::uintptr_t>(module.base_address);
        if (target >= base && target < base + module.size)
            return module.name;
    }
    return {};
}

} // namespace shinkou::platform::detail
