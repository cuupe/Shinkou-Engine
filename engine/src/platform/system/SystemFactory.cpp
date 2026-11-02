// Backend discovery, construction and the process-wide instance.

#include "shinkou/platform/System.h"

#include "portable/PortableBackend.h"

#if defined(SHINKOU_PLATFORM_WINDOWS)
#include "windows/WindowsBackend.h"
#endif
#if defined(SHINKOU_PLATFORM_HAS_POSIX_BACKEND)
#include "posix/PosixBackend.h"
#endif

#include <memory>
#include <mutex>
#include <utility>

namespace shinkou::platform {
namespace {
std::mutex g_instance_mutex;
std::unique_ptr<ISystem> g_instance;
} // namespace

BackendKind default_backend() noexcept {
#if defined(SHINKOU_PLATFORM_WINDOWS) || defined(SHINKOU_PLATFORM_HAS_POSIX_BACKEND)
    return BackendKind::Native;
#else
    return BackendKind::Portable;
#endif
}

bool backend_available(BackendKind kind) noexcept {
    switch (kind) {
    case BackendKind::Auto:
        return true;
    case BackendKind::Native:
#if defined(SHINKOU_PLATFORM_WINDOWS) || defined(SHINKOU_PLATFORM_HAS_POSIX_BACKEND)
        return true;
#else
        return false;
#endif
    case BackendKind::Portable:
        return true;
    }
    return false;
}

std::unique_ptr<ISystem> make_system(BackendKind kind, SystemError *error) {
    if (error != nullptr)
        *error = SystemError{};
    if (kind == BackendKind::Auto)
        kind = default_backend();

    switch (kind) {
    case BackendKind::Native:
#if defined(SHINKOU_PLATFORM_WINDOWS)
        return detail::make_windows_system();
#elif defined(SHINKOU_PLATFORM_HAS_POSIX_BACKEND)
        return detail::make_posix_system();
#else
        if (error != nullptr)
            *error = make_error(ErrorCode::NotSupported, 0,
                                "no native backend is compiled for this platform");
        return nullptr;
#endif
    case BackendKind::Portable:
        return detail::make_portable_system();
    case BackendKind::Auto:
        break;
    }
    if (error != nullptr)
        *error = make_error(ErrorCode::NotSupported, 0, "unknown backend");
    return nullptr;
}

ISystem &System::instance() {
    std::lock_guard<std::mutex> lock(g_instance_mutex);
    if (!g_instance)
        g_instance = make_system(BackendKind::Auto);
    return *g_instance;
}

ISystem *System::try_instance() noexcept {
    std::lock_guard<std::mutex> lock(g_instance_mutex);
    return g_instance.get();
}

bool System::has_instance() noexcept {
    std::lock_guard<std::mutex> lock(g_instance_mutex);
    return g_instance != nullptr;
}

void System::set_instance(std::unique_ptr<ISystem> system) {
    std::lock_guard<std::mutex> lock(g_instance_mutex);
    g_instance = std::move(system);
}

} // namespace shinkou::platform
