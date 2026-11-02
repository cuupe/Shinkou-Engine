// Portable diagnostics and console access.

#include "PortableBackend.h"

#include "PortableCommon.h"

#include <cstdio>
#include <cstring>
#include <string_view>

#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace shinkou::platform::detail {
namespace {

FILE *portable_stream(StandardStream stream) {
    return stream == StandardStream::Error ? stderr : stdout;
}

#if defined(SHINKOU_PLATFORM_WINDOWS)
HANDLE win32_standard_handle(StandardStream stream) {
    return GetStdHandle(stream == StandardStream::Error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
}
#else
int posix_file_descriptor(StandardStream stream) {
    return stream == StandardStream::Error ? STDERR_FILENO : STDOUT_FILENO;
}
#endif

} // namespace

void PortableSystem::debug_output(std::string_view text) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    const std::wstring wide = wide_from_utf8(text);
    if (!wide.empty())
        OutputDebugStringW(wide.c_str());
#endif
    if (!text.empty())
        (void)std::fwrite(text.data(), 1, text.size(), stderr);
}

bool PortableSystem::is_debugger_present() const {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    return IsDebuggerPresent() != FALSE;
#else
    return false;
#endif
}

void PortableSystem::debug_break() {
    SHINKOU_BREAK_INTO_DEBUGGER();
}

std::size_t PortableSystem::capture_stack_trace(StackFrame *frames, std::size_t capacity,
                                                std::size_t skip) {
    if (frames == nullptr || capacity == 0)
        return 0;
#if defined(SHINKOU_PLATFORM_WINDOWS)
    static_assert(sizeof(StackFrame) == sizeof(void *), "StackFrame must wrap one address");
    return CaptureStackBackTrace(static_cast<DWORD>(skip + 1), static_cast<DWORD>(capacity),
                                 reinterpret_cast<PVOID *>(frames), nullptr);
#else
    (void)skip;
    return 0;
#endif
}

Status PortableSystem::install_crash_handler(CrashHandler, void *) {
    return make_error(ErrorCode::NotSupported, 0,
                      "crash handlers are not available in the portable backend");
}

void PortableSystem::uninstall_crash_handler() {
}

Status PortableSystem::write_console(std::string_view text, StandardStream stream) {
    FILE *file = portable_stream(stream);
    if (text.empty())
        return Status{};
    const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
    (void)std::fflush(file);
    if (written != text.size())
        return portable_last_error(ErrorCode::IoError, "fwrite(console)");
    return Status{};
}

bool PortableSystem::is_console_terminal(StandardStream stream) const {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    DWORD mode = 0;
    return GetConsoleMode(win32_standard_handle(stream), &mode) != 0;
#else
    return isatty(posix_file_descriptor(stream)) != 0;
#endif
}

bool PortableSystem::set_console_colors_enabled(bool enabled) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    bool any_changed = false;
    for (const StandardStream stream : {StandardStream::Output, StandardStream::Error}) {
        const HANDLE handle = win32_standard_handle(stream);
        DWORD mode = 0;
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode))
            continue;
        const DWORD updated = enabled ? (mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)
                                      : (mode & ~ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        if (updated != mode && SetConsoleMode(handle, updated))
            any_changed = true;
    }
    return any_changed;
#else
    (void)enabled;
    return false;
#endif
}

} // namespace shinkou::platform::detail
