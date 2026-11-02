// Windows kernel diagnostics: debugger output, stack capture, unhandled
// exception reporting and console access.

#include "WindowsBackend.h"

#include "WindowsCommon.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace shinkou::platform::detail {
namespace {

struct CrashHandlerState {
    CrashHandler handler{nullptr};
    void *user_data{nullptr};
    LPTOP_LEVEL_EXCEPTION_FILTER previous_filter{nullptr};
    bool installed{false};
};

CrashHandlerState g_crash_state;
std::mutex g_crash_mutex;

LONG WINAPI shinkou_unhandled_exception_filter(EXCEPTION_POINTERS *exception) {
    CrashHandler handler = nullptr;
    void *user_data = nullptr;
    LPTOP_LEVEL_EXCEPTION_FILTER previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_crash_mutex);
        handler = g_crash_state.handler;
        user_data = g_crash_state.user_data;
        previous = g_crash_state.previous_filter;
    }

    if (handler != nullptr) {
        StackFrame frames[64]{};
        const USHORT count = CaptureStackBackTrace(0, 64, reinterpret_cast<PVOID *>(frames), nullptr);
        CrashReport report;
        report.code = exception != nullptr && exception->ExceptionRecord != nullptr
                          ? static_cast<std::uint32_t>(exception->ExceptionRecord->ExceptionCode)
                          : 0u;
        report.address = exception != nullptr && exception->ExceptionRecord != nullptr
                             ? exception->ExceptionRecord->ExceptionAddress
                             : nullptr;
        report.thread_id = GetCurrentThreadId();
        report.frames = frames;
        report.frame_count = count;
        report.user_data = user_data;
        handler(report);
    }

    if (previous != nullptr)
        return previous(exception);
    return EXCEPTION_EXECUTE_HANDLER;
}

HANDLE standard_stream_handle(StandardStream stream) noexcept {
    return GetStdHandle(stream == StandardStream::Error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
}

} // namespace

// ---------------------------------------------------------------------------
// WindowsSystem diagnostics entry points
// ---------------------------------------------------------------------------
void WindowsSystem::debug_output(std::string_view text) {
    const std::wstring wide = wide_from_utf8(text);
    if (!wide.empty())
        OutputDebugStringW(wide.c_str());
}

bool WindowsSystem::is_debugger_present() const {
    return IsDebuggerPresent() != FALSE;
}

void WindowsSystem::debug_break() {
    DebugBreak();
}

std::size_t WindowsSystem::capture_stack_trace(StackFrame *frames, std::size_t capacity,
                                               std::size_t skip) {
    if (frames == nullptr || capacity == 0)
        return 0;
    static_assert(sizeof(StackFrame) == sizeof(void *), "StackFrame must wrap one address");
    const DWORD skip_count = static_cast<DWORD>(skip + 1); // hide this function
    const USHORT count = CaptureStackBackTrace(
        skip_count, static_cast<DWORD>(capacity), reinterpret_cast<PVOID *>(frames), nullptr);
    return count;
}

Status WindowsSystem::install_crash_handler(CrashHandler handler, void *user_data) {
    if (handler == nullptr)
        return make_error(ErrorCode::InvalidArgument, 0, "crash handler is null");
    std::lock_guard<std::mutex> lock(g_crash_mutex);
    if (g_crash_state.installed)
        return make_error(ErrorCode::AlreadyExists, 0, "a crash handler is already installed");
    g_crash_state.handler = handler;
    g_crash_state.user_data = user_data;
    g_crash_state.previous_filter = SetUnhandledExceptionFilter(&shinkou_unhandled_exception_filter);
    g_crash_state.installed = true;
    return Status{};
}

void WindowsSystem::uninstall_crash_handler() {
    std::lock_guard<std::mutex> lock(g_crash_mutex);
    if (!g_crash_state.installed)
        return;
    SetUnhandledExceptionFilter(g_crash_state.previous_filter);
    g_crash_state = CrashHandlerState{};
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------
Status WindowsSystem::write_console(std::string_view text, StandardStream stream) {
    const HANDLE handle = standard_stream_handle(stream);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        return make_error(ErrorCode::NotSupported, 0, "standard stream is not attached");
    if (text.empty())
        return Status{};
    if (text.size() > 0xFFFFFFFFu)
        return make_error(ErrorCode::InvalidArgument, 0, "console write exceeds 4 GiB");
    DWORD written = 0;
    if (!WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr))
        return win32_error(ErrorCode::IoError, "WriteFile(console)");
    return Status{};
}

bool WindowsSystem::is_console_terminal(StandardStream stream) const {
    DWORD mode = 0;
    return GetConsoleMode(standard_stream_handle(stream), &mode) != 0;
}

bool WindowsSystem::set_console_colors_enabled(bool enabled) {
    bool any_changed = false;
    for (const StandardStream stream : {StandardStream::Output, StandardStream::Error}) {
        const HANDLE handle = standard_stream_handle(stream);
        DWORD mode = 0;
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode))
            continue;
        const DWORD updated = enabled ? (mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)
                                      : (mode & ~ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        if (updated != mode && SetConsoleMode(handle, updated))
            any_changed = true;
    }
    return any_changed;
}

} // namespace shinkou::platform::detail
