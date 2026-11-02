#include "shinkou/core/Error.h"
namespace shinkou::core {
const char* to_string(ErrorCode code) noexcept {
    switch (code) {
#define SHINKOU_ERROR_NAME(value) case ErrorCode::value: return #value;
    SHINKOU_ERROR_NAME(None) SHINKOU_ERROR_NAME(Unknown) SHINKOU_ERROR_NAME(NotSupported)
    SHINKOU_ERROR_NAME(InvalidArgument) SHINKOU_ERROR_NAME(OutOfRange) SHINKOU_ERROR_NAME(OutOfMemory)
    SHINKOU_ERROR_NAME(AccessDenied) SHINKOU_ERROR_NAME(NotFound) SHINKOU_ERROR_NAME(AlreadyExists)
    SHINKOU_ERROR_NAME(Busy) SHINKOU_ERROR_NAME(Timeout) SHINKOU_ERROR_NAME(Interrupted)
    SHINKOU_ERROR_NAME(IoError) SHINKOU_ERROR_NAME(ProcessError) SHINKOU_ERROR_NAME(LibraryError)
    SHINKOU_ERROR_NAME(SystemFailure) SHINKOU_ERROR_NAME(ParseError) SHINKOU_ERROR_NAME(InvalidData)
    SHINKOU_ERROR_NAME(TypeMismatch) SHINKOU_ERROR_NAME(LimitExceeded) SHINKOU_ERROR_NAME(Conflict)
    SHINKOU_ERROR_NAME(Cancelled) SHINKOU_ERROR_NAME(Overflow) SHINKOU_ERROR_NAME(InvalidState)
#undef SHINKOU_ERROR_NAME
    }
    return "Unknown";
}
std::string Error::describe() const {
    if (ok()) return "ok";
    std::string text = to_string(code);
    if (native_code) text += " (" + std::to_string(native_code) + ")";
    if (!message.empty()) text += ": " + message;
    if (!path.empty()) text += " [" + path + "]";
    if (offset) text += " at byte " + std::to_string(offset);
    return text;
}
}
