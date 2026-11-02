#pragma once

// Shinkou Engine - child process control.
//
// Used by the editor build system, tool integration and the scripting
// compilers. Arguments are passed as separate UTF-8 strings, never through a
// shell, so paths and user data cannot be re-interpreted. Environment entries
// override the inherited block when inherit_environment is true and form the
// complete block when it is false.

#include "shinkou/platform/Error.h"
#include "shinkou/platform/Platform.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace shinkou::platform {

struct EnvEntry {
    std::string name;
    std::string value;
};

struct ProcessOptions {
    // Absolute path or a name resolved through PATH.
    std::string executable;
    // argv[1..]; passed verbatim as separate arguments.
    std::vector<std::string> arguments;
    // Empty inherits the parent's working directory.
    std::string working_directory;

    bool inherit_environment{true};
    std::vector<EnvEntry> environment;

    // Redirect the child's standard streams into pipes that this handle can
    // read. Requires Capability::ProcessCapture.
    bool capture_stdout{false};
    bool capture_stderr{false};
    bool redirect_stdin{false};

    // Suppress a console window for console children (Windows).
    bool create_no_window{true};
};

class IProcess {
  public:
    virtual ~IProcess() = default;

    virtual std::uint64_t id() const noexcept = 0;
    virtual bool is_running() = 0;

    // Blocks until the process terminates.
    virtual Status wait() = 0;
    // Waits up to `timeout` (negative = forever) and returns true if the child
    // exited, false on timeout.
    virtual bool wait_for(Nanoseconds timeout) = 0;

    // Valid only after the process has exited; 0 before that.
    virtual std::int32_t exit_code() const noexcept = 0;

    // Requests termination (the child cannot refuse on Windows; POSIX sends
    // SIGTERM first). kill() is unconditional.
    virtual Status terminate(std::uint32_t exit_code) = 0;
    virtual Status kill() = 0;

    // Non-blocking pipe reads. Return the bytes copied, 0 when nothing is
    // available yet, and NotSupported when the stream was not captured.
    virtual Result<std::size_t> read_stdout(void *buffer, std::size_t bytes) = 0;
    virtual Result<std::size_t> read_stderr(void *buffer, std::size_t bytes) = 0;
    virtual Status write_stdin(const void *data, std::size_t bytes) = 0;
    virtual void close_stdin() = 0;

    virtual void *native_handle() noexcept = 0;
};

} // namespace shinkou::platform
