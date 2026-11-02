// Portable dynamic module loading: LoadLibrary on Windows, dlopen elsewhere.

#include "PortableBackend.h"

#include "PortableCommon.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>

#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace shinkou::platform::detail {

class PortableLibrary final : public ILibrary {
  public:
#if defined(SHINKOU_PLATFORM_WINDOWS)
    using NativeHandle = HMODULE;
#else
    using NativeHandle = void *;
#endif

    PortableLibrary(NativeHandle handle, std::string path)
        : handle_(handle), path_(std::move(path)) {
    }

    ~PortableLibrary() override {
#if defined(SHINKOU_PLATFORM_WINDOWS)
        if (handle_ != nullptr)
            FreeLibrary(handle_);
#else
        if (handle_ != nullptr)
            dlclose(handle_);
#endif
    }

    Result<void *> symbol(std::string_view name) override {
        if (handle_ == nullptr)
            return make_error(ErrorCode::LibraryError, 0, "library is not loaded");
        if (name.empty())
            return make_error(ErrorCode::InvalidArgument, 0, "symbol name is empty");
        const std::string narrow{name};
#if defined(SHINKOU_PLATFORM_WINDOWS)
        FARPROC address = GetProcAddress(handle_, narrow.c_str());
        if (address == nullptr)
            return win32_error(ErrorCode::LibraryError, "GetProcAddress");
        void *symbol_address = nullptr;
        static_assert(sizeof(symbol_address) == sizeof(address));
        std::memcpy(&symbol_address, &address, sizeof(symbol_address));
        return symbol_address;
#else
        dlerror();
        void *address = dlsym(handle_, narrow.c_str());
        if (address == nullptr) {
            const char *message = dlerror();
            return make_error(ErrorCode::LibraryError, 0,
                              message != nullptr ? message : "dlsym returned no symbol");
        }
        return address;
#endif
    }

    std::string path() const override {
        return path_;
    }

    void *native_handle() noexcept override {
        return reinterpret_cast<void *>(handle_);
    }

  private:
    NativeHandle handle_{};
    std::string path_;
};

Result<std::unique_ptr<ILibrary>> make_portable_library(const std::string &path) {
    if (path.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "library path is empty");
#if defined(SHINKOU_PLATFORM_WINDOWS)
    const std::wstring wide = wide_from_utf8(path);
    if (wide.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "library path is not valid UTF-8");
    HMODULE module = LoadLibraryW(wide.c_str());
    if (module == nullptr)
        return win32_error(ErrorCode::LibraryError, "LoadLibraryW");
    return std::unique_ptr<ILibrary>(new PortableLibrary(module, path));
#else
    void *handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (handle == nullptr) {
        const char *message = dlerror();
        return make_error(ErrorCode::LibraryError, 0,
                          message != nullptr ? message : "dlopen failed");
    }
    return std::unique_ptr<ILibrary>(new PortableLibrary(handle, path));
#endif
}

} // namespace shinkou::platform::detail
