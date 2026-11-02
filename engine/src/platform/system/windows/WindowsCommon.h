#pragma once

// Shared Windows backend helpers: UTF-8 <-> UTF-16 conversion, Win32 error
// translation, timeouts, priorities and thread naming. Internal to the
// platform implementation; never included by public headers.

#include "shinkou/platform/Error.h"
#include "shinkou/platform/Platform.h"
#include "shinkou/platform/Thread.h"

#if !defined(SHINKOU_PLATFORM_WINDOWS)
#error "WindowsCommon.h is only available on Windows builds"
#endif

#if !defined(NOMINMAX)
#define NOMINMAX 1
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN 1
#endif

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::platform::detail {

std::string utf8_from_wide(const wchar_t *text, std::size_t length);
std::string utf8_from_wide(const std::wstring &text);
std::wstring wide_from_utf8(std::string_view text);

SHINKOU_API ErrorCode error_code_from_win32(DWORD code) noexcept;
SystemError win32_error(DWORD code, ErrorCode fallback, const char *context);
// Captures GetLastError() immediately and converts it.
SystemError win32_error(ErrorCode fallback, const char *context);
std::string win32_message(DWORD code);

// Converts a nanosecond timeout to a Win32 wait/millisecond timeout. Negative
// means infinite; sub-millisecond nonzero timeouts round up so a polling wait
// cannot spin.
DWORD win32_timeout(Nanoseconds timeout) noexcept;

int win32_thread_priority(ThreadPriority priority) noexcept;
ThreadPriority thread_priority_from_win32(int priority) noexcept;

// SetThreadDescription is resolved dynamically because the import does not
// exist on Windows 7/8. Returns false when the OS cannot store the name.
bool set_win32_thread_name(HANDLE thread, std::string_view name);
bool win32_thread_naming_available() noexcept;

std::int64_t unix_nanoseconds_from_filetime(const FILETIME &time);
std::string win32_module_path(HMODULE module);

// Monotonic nanoseconds for a FILETIME-style 100 ns counter.
inline Nanoseconds filetime_ticks_to_nanoseconds(std::int64_t ticks) noexcept {
    return ticks * 100;
}

} // namespace shinkou::platform::detail
