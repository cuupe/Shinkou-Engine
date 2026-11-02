// POSIX child processes: fork/exec with pipes, waitpid and signal-based
// termination. Arguments and the environment are copied before fork so the
// child only performs async-signal-safe operations.

#include "PosixConfig.h"

#include "PosixBackend.h"
#include "PosixCommon.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace shinkou::platform::detail {
namespace {

std::string resolve_executable(const std::string &executable) {
    if (executable.empty() || executable.find('/') != std::string::npos)
        return executable;
    const char *path_environment = std::getenv("PATH");
    if (path_environment == nullptr)
        return executable;
    const std::string path{path_environment};
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = path.find(':', start);
        const std::string directory =
            end == std::string::npos ? path.substr(start) : path.substr(start, end - start);
        const std::string candidate =
            (directory.empty() ? std::string{"."} : directory) + "/" + executable;
        if (access(candidate.c_str(), X_OK) == 0)
            return candidate;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return executable;
}

std::vector<std::string> build_environment(const ProcessOptions &options) {
    std::vector<std::string> entries;
    const bool custom = !options.inherit_environment || !options.environment.empty();
    if (!custom)
        return entries;
    if (options.inherit_environment && ::environ != nullptr) {
        for (char **cursor = ::environ; *cursor != nullptr; ++cursor)
            entries.emplace_back(*cursor);
    }
    for (const EnvEntry &override : options.environment) {
        if (override.name.empty())
            continue;
        const std::string prefix = override.name + "=";
        auto match = std::find_if(entries.begin(), entries.end(), [&prefix](const std::string &entry) {
            return entry.rfind(prefix, 0) == 0;
        });
        if (match != entries.end())
            *match = prefix + override.value;
        else
            entries.push_back(prefix + override.value);
    }
    return entries;
}

int decode_wait_status(int status) noexcept {
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return status;
}

class PosixProcess final : public IProcess {
  public:
    PosixProcess(pid_t pid, int stdout_fd, int stderr_fd, int stdin_fd)
        : pid_(pid), stdout_fd_(stdout_fd), stderr_fd_(stderr_fd), stdin_fd_(stdin_fd) {
    }

    ~PosixProcess() override {
        close_fd(stdout_fd_);
        close_fd(stderr_fd_);
        close_fd(stdin_fd_);
        if (!exited_) {
            int status = 0;
            (void)waitpid(pid_, &status, WNOHANG);
        }
    }

    std::uint64_t id() const noexcept override {
        return static_cast<std::uint64_t>(pid_);
    }

    bool is_running() override {
        if (exited_)
            return false;
        for (;;) {
            int status = 0;
            const pid_t result = waitpid(pid_, &status, WNOHANG);
            if (result == 0)
                return true;
            if (result == pid_) {
                update_exit_status(status);
                return false;
            }
            if (result < 0 && errno == EINTR)
                continue;
            exited_ = true;
            return false;
        }
    }

    Status wait() override {
        (void)wait_for(kInfiniteTimeout);
        return Status{};
    }

    bool wait_for(Nanoseconds timeout) override {
        if (exited_)
            return true;
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            int status = 0;
            const pid_t result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                update_exit_status(status);
                return true;
            }
            if (result < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == ECHILD) {
                    exited_ = true;
                    return true;
                }
                return false;
            }
            if (timeout == 0)
                return false;
            if (timeout > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count();
                if (elapsed >= timeout)
                    return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    std::int32_t exit_code() const noexcept override {
        return exited_ ? exit_code_ : 0;
    }

    Status terminate(std::uint32_t exit_code) override {
        if (exited_)
            return Status{};
        if (::kill(pid_, SIGTERM) != 0) {
            if (errno == ESRCH) {
                exited_ = true;
                return Status{};
            }
            return posix_last_error(ErrorCode::ProcessError, "kill(SIGTERM)");
        }
        (void)exit_code;
        return Status{};
    }

    Status kill() override {
        if (exited_)
            return Status{};
        if (::kill(pid_, SIGKILL) != 0 && errno != ESRCH)
            return posix_last_error(ErrorCode::ProcessError, "kill(SIGKILL)");
        return Status{};
    }

    Result<std::size_t> read_stdout(void *buffer, std::size_t bytes) override {
        return read_stream(stdout_fd_, buffer, bytes, "stdout");
    }

    Result<std::size_t> read_stderr(void *buffer, std::size_t bytes) override {
        return read_stream(stderr_fd_, buffer, bytes, "stderr");
    }

    Status write_stdin(const void *data, std::size_t bytes) override {
        if (stdin_fd_ < 0)
            return make_error(ErrorCode::NotSupported, 0, "stdin was not redirected");
        const auto *pointer = static_cast<const std::uint8_t *>(data);
        std::size_t remaining = bytes;
        while (remaining > 0) {
            const ssize_t written = ::write(stdin_fd_, pointer, remaining);
            if (written < 0) {
                if (errno == EINTR)
                    continue;
                return posix_last_error(ErrorCode::IoError, "write(stdin)");
            }
            if (written == 0)
                return make_error(ErrorCode::IoError, 0, "write(stdin) made no progress");
            pointer += static_cast<std::size_t>(written);
            remaining -= static_cast<std::size_t>(written);
        }
        return Status{};
    }

    void close_stdin() override {
        close_fd(stdin_fd_);
    }

    void *native_handle() noexcept override {
        return nullptr;
    }

  private:
    Result<std::size_t> read_stream(int descriptor, void *buffer, std::size_t bytes,
                                    const char *name) {
        if (descriptor < 0)
            return make_error(ErrorCode::NotSupported, 0,
                              std::string{"stream "} + name + " was not captured");
        for (;;) {
            pollfd entry{};
            entry.fd = descriptor;
            entry.events = POLLIN;
            const int ready = poll(&entry, 1, 0);
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                return posix_last_error(ErrorCode::IoError, "poll(stream)");
            }
            if (ready == 0)
                return static_cast<std::size_t>(0);
            const ssize_t read_bytes = ::read(descriptor, buffer, bytes);
            if (read_bytes > 0)
                return static_cast<std::size_t>(read_bytes);
            if (read_bytes == 0)
                return static_cast<std::size_t>(0);
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return static_cast<std::size_t>(0);
            return posix_last_error(ErrorCode::IoError, "read(stream)");
        }
    }

    static void close_fd(int &descriptor) noexcept {
        if (descriptor >= 0) {
            (void)::close(descriptor);
            descriptor = -1;
        }
    }

    void update_exit_status(int status) noexcept {
        exit_code_ = static_cast<std::int32_t>(decode_wait_status(status));
        exited_ = true;
    }

    pid_t pid_{0};
    int stdout_fd_{-1};
    int stderr_fd_{-1};
    int stdin_fd_{-1};
    std::int32_t exit_code_{0};
    bool exited_{false};
};

struct ChildPipes {
    int stdout_read{-1};
    int stdout_write{-1};
    int stderr_read{-1};
    int stderr_write{-1};
    int stdin_read{-1};
    int stdin_write{-1};
};

void close_child_pipes(ChildPipes &pipes) noexcept {
    auto close_end = [](int &descriptor) {
        if (descriptor >= 0) {
            (void)::close(descriptor);
            descriptor = -1;
        }
    };
    close_end(pipes.stdout_write);
    close_end(pipes.stderr_write);
    close_end(pipes.stdin_read);
}

void close_all_pipes(ChildPipes &pipes) noexcept {
    auto close_end = [](int &descriptor) {
        if (descriptor >= 0) {
            (void)::close(descriptor);
            descriptor = -1;
        }
    };
    close_end(pipes.stdout_read);
    close_end(pipes.stdout_write);
    close_end(pipes.stderr_read);
    close_end(pipes.stderr_write);
    close_end(pipes.stdin_read);
    close_end(pipes.stdin_write);
}

bool make_nonblocking(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFL, 0);
    if (flags < 0)
        return false;
    return fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

} // namespace

Result<std::unique_ptr<IProcess>> make_posix_process(const ProcessOptions &options) {
    if (options.executable.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "process executable is empty");

    // Build every string before fork so the child only uses already-touched
    // memory and async-signal-safe calls.
    const std::string executable = resolve_executable(options.executable);
    std::vector<std::string> arguments;
    arguments.reserve(options.arguments.size() + 1);
    arguments.push_back(executable);
    for (const std::string &argument : options.arguments)
        arguments.push_back(argument);

    std::vector<std::string> environment_storage = build_environment(options);
    std::vector<char *> environment_pointers;
    environment_pointers.reserve(environment_storage.size() + 1);
    for (std::string &entry : environment_storage)
        environment_pointers.push_back(entry.data());
    environment_pointers.push_back(nullptr);

    std::vector<char *> argument_pointers;
    argument_pointers.reserve(arguments.size() + 1);
    for (std::string &argument : arguments)
        argument_pointers.push_back(argument.data());
    argument_pointers.push_back(nullptr);

    ChildPipes pipes;
    if (options.capture_stdout) {
        int descriptors[2] = {-1, -1};
        if (pipe(descriptors) != 0)
            return posix_last_error(ErrorCode::ProcessError, "pipe(stdout)");
        pipes.stdout_read = descriptors[0];
        pipes.stdout_write = descriptors[1];
    }
    if (options.capture_stderr) {
        int descriptors[2] = {-1, -1};
        if (pipe(descriptors) != 0) {
            close_all_pipes(pipes);
            return posix_last_error(ErrorCode::ProcessError, "pipe(stderr)");
        }
        pipes.stderr_read = descriptors[0];
        pipes.stderr_write = descriptors[1];
    }
    if (options.redirect_stdin) {
        int descriptors[2] = {-1, -1};
        if (pipe(descriptors) != 0) {
            close_all_pipes(pipes);
            return posix_last_error(ErrorCode::ProcessError, "pipe(stdin)");
        }
        pipes.stdin_read = descriptors[0];
        pipes.stdin_write = descriptors[1];
    }

    const pid_t pid = fork();
    if (pid < 0) {
        const SystemError error = posix_last_error(ErrorCode::ProcessError, "fork");
        close_all_pipes(pipes);
        return error;
    }

    if (pid == 0) {
        if (pipes.stdin_read >= 0)
            (void)dup2(pipes.stdin_read, STDIN_FILENO);
        if (pipes.stdout_write >= 0)
            (void)dup2(pipes.stdout_write, STDOUT_FILENO);
        if (pipes.stderr_write >= 0)
            (void)dup2(pipes.stderr_write, STDERR_FILENO);

        const int all_pipes[] = {pipes.stdout_read,  pipes.stdout_write, pipes.stderr_read,
                                 pipes.stderr_write, pipes.stdin_read,   pipes.stdin_write};
        for (const int descriptor : all_pipes) {
            if (descriptor >= 0 && descriptor > STDERR_FILENO)
                (void)::close(descriptor);
        }
        if (!options.working_directory.empty()) {
            if (chdir(options.working_directory.c_str()) != 0)
                _exit(127);
        }
        char **environment = options.inherit_environment && options.environment.empty()
                                 ? ::environ
                                 : environment_pointers.data();
        execve(executable.c_str(), argument_pointers.data(), environment);
        _exit(127);
    }

    // Parent: hand the child ends over and keep the read/write ends.
    close_child_pipes(pipes);
    if (pipes.stdout_read >= 0)
        (void)make_nonblocking(pipes.stdout_read);
    if (pipes.stderr_read >= 0)
        (void)make_nonblocking(pipes.stderr_read);

    return std::unique_ptr<IProcess>(new PosixProcess(pid, pipes.stdout_read, pipes.stderr_read,
                                                      pipes.stdin_write));
}

Result<std::unique_ptr<IProcess>> PosixSystem::spawn_process(const ProcessOptions &options) {
    return make_posix_process(options);
}

} // namespace shinkou::platform::detail
