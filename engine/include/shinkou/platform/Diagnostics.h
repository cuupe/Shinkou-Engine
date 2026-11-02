#pragma once

// Shinkou Engine - kernel-facing diagnostics: debugger output, stack capture,
// unhandled-exception reporting and console access.

#include "shinkou/platform/Error.h"
#include "shinkou/platform/Platform.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace shinkou::platform {

struct StackFrame {
    void *address{nullptr};
};

// Snapshot handed to a crash handler. `code` follows the platform convention
// (Windows NTSTATUS / POSIX signal number). Only the calling thread is
// described; walking every thread from a crashed process is deliberately out of
// scope for the first version.
//
// Members avoid the `exception_code` / `exception_info` names because the
// Windows SDK defines them as macros in <excpt.h>.
struct CrashReport {
    std::uint32_t code{0};
    const void *address{nullptr};
    std::uint64_t thread_id{0};
    const StackFrame *frames{nullptr};
    std::size_t frame_count{0};
    void *user_data{nullptr};
};

// Runs on the faulting thread after the process has been declared unhandled.
// The handler must not throw, and should be short: write a dump, log the code
// and terminate. Returning lets the default platform behaviour (terminate)
// proceed.
using CrashHandler = void (*)(const CrashReport &report);

enum class StandardStream : std::uint8_t { Output = 0, Error };

} // namespace shinkou::platform
