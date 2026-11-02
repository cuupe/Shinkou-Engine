#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace shinkou::core {
// Existing platform values retain their numeric representation. Domains extend
// the context/message, rather than inventing another incompatible error channel.
enum class ErrorCode : std::int32_t {
    None = 0, Unknown, NotSupported, InvalidArgument, OutOfRange, OutOfMemory,
    AccessDenied, NotFound, AlreadyExists, Busy, Timeout, Interrupted, IoError,
    ProcessError, LibraryError, SystemFailure,
    ParseError, InvalidData, TypeMismatch, LimitExceeded, Conflict, Cancelled,
    Overflow, InvalidState,
};
const char* to_string(ErrorCode code) noexcept;
struct Error {
    ErrorCode code{ErrorCode::None};
    std::int32_t native_code{0};
    std::string message;
    std::size_t offset{0};
    std::string path;
    Error() = default;
    explicit Error(ErrorCode value) : code(value) {}
    Error(ErrorCode value, std::int32_t native, std::string text = {})
        : code(value), native_code(native), message(std::move(text)) {}
    bool ok() const noexcept { return code == ErrorCode::None; }
    explicit operator bool() const noexcept { return !ok(); }
    std::string describe() const;
};
inline Error make_error(ErrorCode code, std::int32_t native = 0, std::string message = {}) {
    return Error{code, native, std::move(message)};
}
inline void set_error_if_empty(Error& first, ErrorCode code, std::int32_t native = 0, std::string message = {}) {
    if (first.ok()) first = make_error(code, native, std::move(message));
}
}
