#include "WindowsCommon.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace shinkou::platform::detail {

std::string utf8_from_wide(const wchar_t *text, std::size_t length) {
    if (text == nullptr || length == 0)
        return {};
    const auto safe_length = static_cast<int>(length > 0x7FFFFFFFu ? 0x7FFFFFFFu : length);
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, safe_length, nullptr,
                                         0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, safe_length, result.data(), size,
                            nullptr, nullptr) != size)
        return {};
    return result;
}

std::string utf8_from_wide(const std::wstring &text) {
    return utf8_from_wide(text.data(), text.size());
}

std::wstring wide_from_utf8(std::string_view text) {
    if (text.empty())
        return {};
    const auto safe_length =
        static_cast<int>(text.size() > 0x7FFFFFFFu ? 0x7FFFFFFFu : text.size());
    const int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), safe_length, nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), safe_length, result.data(),
                            size) != size)
        return {};
    return result;
}

ErrorCode error_code_from_win32(DWORD code) noexcept {
    switch (code) {
    case ERROR_SUCCESS:
        return ErrorCode::None;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
        return ErrorCode::NotFound;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_WRITE_PROTECT:
        return ErrorCode::AccessDenied;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return ErrorCode::AlreadyExists;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_BUSY:
    case ERROR_PIPE_BUSY:
    case ERROR_RETRY:
        return ErrorCode::Busy;
    case ERROR_TIMEOUT:
    case WAIT_TIMEOUT:
        return ErrorCode::Timeout;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_COMMITMENT_LIMIT:
        return ErrorCode::OutOfMemory;
    case ERROR_INVALID_HANDLE:
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_DIRECTORY:
    case ERROR_INVALID_FLAGS:
        return ErrorCode::InvalidArgument;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
    case ERROR_INVALID_FUNCTION:
        return ErrorCode::NotSupported;
    case ERROR_OPERATION_ABORTED:
    case ERROR_CANCELLED:
        return ErrorCode::Interrupted;
    case ERROR_NOT_READY:
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return ErrorCode::IoError;
    default:
        return ErrorCode::SystemFailure;
    }
}

std::string win32_message(DWORD code) {
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        if (buffer != nullptr)
            LocalFree(buffer);
        return "Win32 error " + std::to_string(code);
    }
    std::string text = utf8_from_wide(buffer, length);
    LocalFree(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
                             text.back() == '.'))
        text.pop_back();
    return text;
}

SystemError win32_error(DWORD code, ErrorCode fallback, const char *context) {
    ErrorCode mapped = error_code_from_win32(code);
    if (mapped == ErrorCode::SystemFailure && fallback != ErrorCode::SystemFailure)
        mapped = fallback;
    std::string message = context != nullptr ? context : "Win32";
    message += ": ";
    message += win32_message(code);
    return SystemError{mapped, static_cast<std::int32_t>(code), std::move(message)};
}

SystemError win32_error(ErrorCode fallback, const char *context) {
    return win32_error(GetLastError(), fallback, context);
}

DWORD win32_timeout(Nanoseconds timeout) noexcept {
    if (timeout < 0)
        return INFINITE;
    if (timeout == 0)
        return 0;
    constexpr DWORD kMaxFiniteTimeout = 0xFFFFFFFEu;
    constexpr Nanoseconds kMaxFiniteNanoseconds =
        static_cast<Nanoseconds>(kMaxFiniteTimeout) * kNanosecondsPerMillisecond;
    if (timeout >= kMaxFiniteNanoseconds)
        return kMaxFiniteTimeout;
    const auto milliseconds = (timeout + kNanosecondsPerMillisecond - 1) / kNanosecondsPerMillisecond;
    return static_cast<DWORD>(milliseconds == 0 ? 1 : milliseconds);
}

int win32_thread_priority(ThreadPriority priority) noexcept {
    switch (priority) {
    case ThreadPriority::Idle:
        return THREAD_PRIORITY_IDLE;
    case ThreadPriority::Lowest:
        return THREAD_PRIORITY_LOWEST;
    case ThreadPriority::BelowNormal:
        return THREAD_PRIORITY_BELOW_NORMAL;
    case ThreadPriority::Normal:
        return THREAD_PRIORITY_NORMAL;
    case ThreadPriority::AboveNormal:
        return THREAD_PRIORITY_ABOVE_NORMAL;
    case ThreadPriority::Highest:
        return THREAD_PRIORITY_HIGHEST;
    case ThreadPriority::TimeCritical:
        return THREAD_PRIORITY_TIME_CRITICAL;
    }
    return THREAD_PRIORITY_NORMAL;
}

ThreadPriority thread_priority_from_win32(int priority) noexcept {
    switch (priority) {
    case THREAD_PRIORITY_IDLE:
        return ThreadPriority::Idle;
    case THREAD_PRIORITY_LOWEST:
        return ThreadPriority::Lowest;
    case THREAD_PRIORITY_BELOW_NORMAL:
        return ThreadPriority::BelowNormal;
    case THREAD_PRIORITY_ABOVE_NORMAL:
        return ThreadPriority::AboveNormal;
    case THREAD_PRIORITY_HIGHEST:
        return ThreadPriority::Highest;
    case THREAD_PRIORITY_TIME_CRITICAL:
        return ThreadPriority::TimeCritical;
    default:
        return ThreadPriority::Normal;
    }
}

bool set_win32_thread_name(HANDLE thread, std::string_view name) {
    if (thread == nullptr || name.empty())
        return false;
    using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    static const SetThreadDescriptionFn set_description = []() -> SetThreadDescriptionFn {
        const FARPROC address =
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
        SetThreadDescriptionFn function = nullptr;
        static_assert(sizeof(function) == sizeof(address));
        std::memcpy(&function, &address, sizeof(function));
        return function;
    }();
    if (set_description == nullptr)
        return false;
    const std::wstring wide_name = wide_from_utf8(name);
    if (wide_name.empty())
        return false;
    return SUCCEEDED(set_description(thread, wide_name.c_str()));
}

bool win32_thread_naming_available() noexcept {
    return GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription") != nullptr;
}

std::int64_t unix_nanoseconds_from_filetime(const FILETIME &time) {
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    if (ticks == 0)
        return 0;
    constexpr std::int64_t kWindowsEpochToUnixEpoch100ns = 116444736000000000LL;
    return (static_cast<std::int64_t>(ticks) - kWindowsEpochToUnixEpoch100ns) * 100;
}

std::string win32_module_path(HMODULE module) {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            return utf8_from_wide(buffer);
        }
        if (buffer.size() >= 0x10000)
            return {};
        buffer.resize(buffer.size() * 2);
    }
}

} // namespace shinkou::platform::detail
