#pragma once

// Shared helpers for the POSIX backend. Internal to the platform layer.

#include "shinkou/platform/Error.h"
#include "shinkou/platform/Platform.h"

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(__has_include)
#if __has_include(<execinfo.h>)
#define SHINKOU_POSIX_HAS_EXECINFO 1
#endif
#endif

namespace shinkou::platform::detail {

ErrorCode error_code_from_errno(int code) noexcept;
SystemError posix_error(int code, ErrorCode fallback, const char *context);
SystemError posix_last_error(ErrorCode fallback, const char *context);

// Numeric identity of a pthread_t, usable with the portable uint64 id surface.
std::uint64_t posix_thread_id_value() noexcept;

std::size_t posix_page_size();
Nanoseconds posix_clock_nanoseconds(int clock_id);

} // namespace shinkou::platform::detail
