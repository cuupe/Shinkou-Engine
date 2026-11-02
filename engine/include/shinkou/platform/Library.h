#pragma once

// Shinkou Engine - dynamic modules and loaded-module introspection.
//
// The engine links its systems statically today; this interface covers plugins,
// optional backends (renderer/shader tooling) and diagnostics that must resolve
// a symbol or list the modules mapped into the process.

#include "shinkou/platform/Error.h"
#include "shinkou/platform/Platform.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace shinkou::platform {

struct ModuleInfo {
    std::string name;
    std::string path;
    void *base_address{nullptr};
    std::size_t size{0};
};

class ILibrary {
  public:
    virtual ~ILibrary() = default;

    // Returns the symbol address or a LibraryError when it is not exported.
    virtual Result<void *> symbol(std::string_view name) = 0;

    virtual std::string path() const = 0;
    virtual void *native_handle() noexcept = 0;
};

} // namespace shinkou::platform
