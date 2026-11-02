#include "PosixCommon.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <utility>

#include <pthread.h>
#include <unistd.h>

namespace shinkou::platform::detail {

ErrorCode error_code_from_errno(int code) noexcept {
    switch (code) {
    case 0:
        return ErrorCode::None;
    case ENOENT:
    case ENOTDIR:
    case ENXIO:
    case ENODEV:
    case ESRCH:
        return ErrorCode::NotFound;
    case EEXIST:
        return ErrorCode::AlreadyExists;
    case EACCES:
    case EPERM:
    case EROFS:
        return ErrorCode::AccessDenied;
    case ENOMEM:
    case ENOBUFS:
        return ErrorCode::OutOfMemory;
    case EAGAIN:
    case EBUSY:
    case EDEADLK:
        return ErrorCode::Busy;
    case ETIMEDOUT:
        return ErrorCode::Timeout;
    case EINTR:
    case ECANCELED:
        return ErrorCode::Interrupted;
    case EINVAL:
    case ENAMETOOLONG:
    case ELOOP:
        return ErrorCode::InvalidArgument;
    case EOPNOTSUPP:
    case ENOSYS:
        return ErrorCode::NotSupported;
    case EFBIG:
    case ERANGE:
        return ErrorCode::OutOfRange;
    case EIO:
    case EPIPE:
    case ENOSPC:
    case ENOTEMPTY:
        return ErrorCode::IoError;
    default:
        return ErrorCode::SystemFailure;
    }
}

SystemError posix_error(int code, ErrorCode fallback, const char *context) {
    ErrorCode mapped = error_code_from_errno(code);
    if (mapped == ErrorCode::SystemFailure && fallback != ErrorCode::SystemFailure)
        mapped = fallback;
    std::string message = context != nullptr ? context : "POSIX";
    message += ": ";
    // strerror is not thread-safe on every libc, but these paths are cold and
    // the message is copied immediately.
    message += std::strerror(code);
    return SystemError{mapped, code, std::move(message)};
}

SystemError posix_last_error(ErrorCode fallback, const char *context) {
    return posix_error(errno, fallback, context);
}

std::uint64_t posix_thread_id_value() noexcept {
    const pthread_t self = pthread_self();
    std::uint64_t value = 0;
    static_assert(sizeof(pthread_t) <= sizeof(value), "pthread_t does not fit in uint64");
    std::memcpy(&value, &self, sizeof(self));
    return value;
}

std::size_t posix_page_size() {
    const long value = sysconf(_SC_PAGESIZE);
    return value > 0 ? static_cast<std::size_t>(value) : 4096;
}

Nanoseconds posix_clock_nanoseconds(int clock_id) {
    timespec value{};
    if (clock_gettime(static_cast<clockid_t>(clock_id), &value) != 0)
        return 0;
    return static_cast<Nanoseconds>(value.tv_sec) * kNanosecondsPerSecond + value.tv_nsec;
}

} // namespace shinkou::platform::detail
