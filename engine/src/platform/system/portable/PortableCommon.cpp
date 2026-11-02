#include "PortableCommon.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <cerrno>
#if defined(SHINKOU_PLATFORM_LINUX)
#include <climits>
#endif
#endif

namespace shinkou::platform::detail {

std::size_t portable_page_size() {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwPageSize == 0 ? 4096 : info.dwPageSize;
#else
    const long value = sysconf(_SC_PAGESIZE);
    return value > 0 ? static_cast<std::size_t>(value) : 4096;
#endif
}

std::size_t portable_allocation_granularity() {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwAllocationGranularity == 0 ? 65536 : info.dwAllocationGranularity;
#else
    return portable_page_size();
#endif
}

std::string portable_executable_path() {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            return utf8_from_wide(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(SHINKOU_PLATFORM_LINUX)
    std::string buffer(PATH_MAX, '\0');
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length <= 0)
        return {};
    buffer.resize(static_cast<std::size_t>(length));
    return buffer;
#else
    return {};
#endif
}

std::string portable_computer_name() {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!GetComputerNameW(buffer, &size))
        return {};
    return utf8_from_wide(buffer, size);
#else
    char buffer[256]{};
    if (gethostname(buffer, sizeof(buffer)) != 0)
        return {};
    buffer[sizeof(buffer) - 1] = '\0';
    return std::string{buffer};
#endif
}

std::string portable_user_name() {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    const char *value = std::getenv("USERNAME");
#else
    const char *value = std::getenv("USER");
#endif
    return value != nullptr ? std::string{value} : std::string{};
}

std::uint64_t portable_process_id() {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

std::string portable_command_line() {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    return utf8_from_wide(GetCommandLineW());
#else
    return {};
#endif
}

Nanoseconds portable_uptime() {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    return static_cast<Nanoseconds>(GetTickCount64()) * kNanosecondsPerMillisecond;
#elif defined(CLOCK_BOOTTIME)
    timespec value{};
    if (clock_gettime(CLOCK_BOOTTIME, &value) == 0)
        return static_cast<Nanoseconds>(value.tv_sec) * kNanosecondsPerSecond + value.tv_nsec;
    return 0;
#else
    return 0;
#endif
}

ErrorCode map_std_errc(const std::error_code &code) noexcept {
    if (!code)
        return ErrorCode::None;
    if (code == std::errc::no_such_file_or_directory || code == std::errc::not_a_directory)
        return ErrorCode::NotFound;
    if (code == std::errc::file_exists)
        return ErrorCode::AlreadyExists;
    if (code == std::errc::permission_denied || code == std::errc::read_only_file_system)
        return ErrorCode::AccessDenied;
    if (code == std::errc::not_enough_memory || code == std::errc::no_space_on_device)
        return ErrorCode::OutOfMemory;
    if (code == std::errc::device_or_resource_busy || code == std::errc::text_file_busy)
        return ErrorCode::Busy;
    if (code == std::errc::operation_not_supported || code == std::errc::function_not_supported)
        return ErrorCode::NotSupported;
    if (code == std::errc::invalid_argument || code == std::errc::filename_too_long)
        return ErrorCode::InvalidArgument;
    if (code == std::errc::timed_out)
        return ErrorCode::Timeout;
    if (code == std::errc::io_error)
        return ErrorCode::IoError;
    return ErrorCode::SystemFailure;
}

SystemError portable_error(std::error_code code, ErrorCode fallback, const char *context) {
    const std::int32_t native = code.value();
    ErrorCode mapped = map_std_errc(code);
    if (mapped == ErrorCode::SystemFailure && fallback != ErrorCode::SystemFailure)
        mapped = fallback;
    std::string message = context != nullptr ? context : "portable";
    message += ": ";
    message += code.message();
    return SystemError{mapped, native, std::move(message)};
}

SystemError portable_last_error(ErrorCode fallback, const char *context) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    return win32_error(fallback, context);
#else
    const int code = errno;
    std::string message = context != nullptr ? context : "portable";
    message += ": ";
    message += std::strerror(code);
    return SystemError{fallback, code, std::move(message)};
#endif
}

std::FILE *portable_fopen(std::string_view path, std::string_view mode) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    const std::wstring wide_path = wide_from_utf8(path);
    const std::wstring wide_mode = wide_from_utf8(mode);
    if (wide_path.empty() || wide_mode.empty())
        return nullptr;
    return _wfopen(wide_path.c_str(), wide_mode.c_str());
#else
    const std::string native_path{path};
    const std::string native_mode{mode};
    return std::fopen(native_path.c_str(), native_mode.c_str());
#endif
}

std::int64_t portable_ftell(std::FILE *file) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    return static_cast<std::int64_t>(_ftelli64(file));
#else
    return static_cast<std::int64_t>(ftello(file));
#endif
}

bool portable_fseek(std::FILE *file, std::int64_t offset, int origin) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    return _fseeki64(file, offset, origin) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), origin) == 0;
#endif
}

bool portable_truncate(std::FILE *file, std::int64_t size) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    return _chsize_s(_fileno(file), size) == 0;
#else
    return ftruncate(fileno(file), static_cast<off_t>(size)) == 0;
#endif
}

} // namespace shinkou::platform::detail
