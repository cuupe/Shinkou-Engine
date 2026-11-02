#pragma once

// Shared helpers for the portable (standard-library) backend.

#include "shinkou/platform/Error.h"
#include "shinkou/platform/Platform.h"

#if defined(SHINKOU_PLATFORM_WINDOWS)
#include "../windows/WindowsCommon.h"
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace shinkou::platform::detail {

inline std::filesystem::path portable_path_from_utf8(std::string_view text) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    return std::filesystem::path(wide_from_utf8(text));
#else
    return std::filesystem::path(std::string{text});
#endif
}

inline std::string portable_path_to_utf8(const std::filesystem::path &path) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    return utf8_from_wide(path.native());
#else
    return path.native();
#endif
}

std::size_t portable_page_size();
std::size_t portable_allocation_granularity();

std::string portable_executable_path();
std::string portable_computer_name();
std::string portable_user_name();
std::string portable_command_line();
std::uint64_t portable_process_id();
Nanoseconds portable_uptime();

SystemError portable_error(std::error_code code, ErrorCode fallback, const char *context);
SystemError portable_last_error(ErrorCode fallback, const char *context);

// UTF-8-aware stdio helpers (the C runtime expects UTF-16 paths on Windows).
std::FILE *portable_fopen(std::string_view path, std::string_view mode);
std::int64_t portable_ftell(std::FILE *file);
bool portable_fseek(std::FILE *file, std::int64_t offset, int origin);
bool portable_truncate(std::FILE *file, std::int64_t size);

} // namespace shinkou::platform::detail
