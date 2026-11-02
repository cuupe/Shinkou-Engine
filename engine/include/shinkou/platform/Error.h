#pragma once

// Source-compatible platform names for the engine-wide error/result contract.
// Native backends retain their native_code and portable code without depending
// on editor, renderer, or the JSON implementation.
#include "shinkou/platform/Platform.h"
#include "shinkou/core/Result.h"

namespace shinkou::platform {
using ErrorCode = core::ErrorCode;
using SystemError = core::Error;
template<class T> using Result = core::Result<T>;
using Status = core::Status;
using core::to_string;
using core::make_error;
using core::set_error_if_empty;
}
