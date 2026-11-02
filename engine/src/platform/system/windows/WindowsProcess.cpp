// Windows native child processes and the process environment.

#include "WindowsBackend.h"

#include "WindowsCommon.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace shinkou::platform::detail {
namespace {

// Quotes one argument using the CommandLineToArgvW rules.
void append_quoted_argument(std::wstring &command_line, const std::wstring &argument) {
    if (!command_line.empty())
        command_line.push_back(L' ');

    const bool needs_quotes = argument.empty() ||
                              argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quotes) {
        command_line += argument;
        return;
    }

    command_line.push_back(L'"');
    for (auto iterator = argument.begin();; ++iterator) {
        std::size_t backslashes = 0;
        while (iterator != argument.end() && *iterator == L'\\') {
            ++iterator;
            ++backslashes;
        }
        if (iterator == argument.end()) {
            command_line.append(backslashes * 2, L'\\');
            break;
        }
        if (*iterator == L'"') {
            command_line.append(backslashes * 2 + 1, L'\\');
            command_line.push_back(L'"');
        } else {
            command_line.append(backslashes, L'\\');
            command_line.push_back(*iterator);
        }
    }
    command_line.push_back(L'"');
}

std::wstring quote_command_line(const std::string &executable,
                                const std::vector<std::string> &arguments) {
    std::wstring command_line;
    append_quoted_argument(command_line, wide_from_utf8(executable));
    for (const std::string &argument : arguments)
        append_quoted_argument(command_line, wide_from_utf8(argument));
    return command_line;
}

std::vector<std::pair<std::wstring, std::wstring>> inherited_environment() {
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    LPWCH block = GetEnvironmentStringsW();
    if (block == nullptr)
        return entries;
    for (const wchar_t *cursor = block; *cursor != L'\0';) {
        const std::wstring entry{cursor};
        cursor += entry.size() + 1;
        const std::size_t separator = entry.find(L'=');
        // "=C:=C:\path" entries describe the current directory of a drive and
        // must not be copied into a child's explicit block.
        if (separator == std::wstring::npos || separator == 0)
            continue;
        entries.emplace_back(entry.substr(0, separator), entry.substr(separator + 1));
    }
    FreeEnvironmentStringsW(block);
    return entries;
}

std::wstring build_environment_block(bool inherit,
                                     const std::vector<EnvEntry> &overrides,
                                     bool &changed) {
    std::vector<std::pair<std::wstring, std::wstring>> entries =
        inherit ? inherited_environment() : std::vector<std::pair<std::wstring, std::wstring>>{};

    changed = !overrides.empty() || !inherit;
    for (const EnvEntry &override : overrides) {
        const std::wstring name = wide_from_utf8(override.name);
        const std::wstring value = wide_from_utf8(override.value);
        if (name.empty())
            continue;
        auto match = std::find_if(entries.begin(), entries.end(), [&](const auto &entry) {
            return entry.first.size() == name.size() &&
                   _wcsnicmp(entry.first.c_str(), name.c_str(), name.size()) == 0;
        });
        if (match != entries.end())
            match->second = value;
        else
            entries.emplace_back(name, value);
    }

    if (!changed)
        return {};

    std::sort(entries.begin(), entries.end(),
              [](const auto &left, const auto &right) { return left.first < right.first; });
    std::wstring block;
    for (const auto &entry : entries) {
        block += entry.first;
        block.push_back(L'=');
        block += entry.second;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

} // namespace

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------
class WindowsProcess final : public IProcess {
  public:
    WindowsProcess() = default;

    ~WindowsProcess() override {
        close_streams();
        if (process_ != nullptr)
            CloseHandle(process_);
    }

    bool valid() const noexcept {
        return process_ != nullptr;
    }

    const SystemError &error() const noexcept {
        return error_;
    }

    HANDLE process_handle() const noexcept {
        return process_;
    }

    void store(HANDLE process, DWORD id) {
        process_ = process;
        id_ = id;
    }

    void store_streams(HANDLE stdout_read, HANDLE stderr_read, HANDLE stdin_write) {
        stdout_read_ = stdout_read;
        stderr_read_ = stderr_read;
        stdin_write_ = stdin_write;
    }

    void store_error(const SystemError &error) {
        error_ = error;
    }

    std::uint64_t id() const noexcept override {
        return id_;
    }

    bool is_running() override {
        if (exited_)
            return false;
        if (process_ == nullptr)
            return false;
        const DWORD result = WaitForSingleObject(process_, 0);
        if (result == WAIT_TIMEOUT)
            return true;
        update_exit_state();
        return false;
    }

    Status wait() override {
        (void)wait_for(kInfiniteTimeout);
        return Status{};
    }

    bool wait_for(Nanoseconds timeout) override {
        if (process_ == nullptr) {
            exited_ = true;
            return true;
        }
        const DWORD result = WaitForSingleObject(process_, win32_timeout(timeout));
        if (result != WAIT_OBJECT_0)
            return false;
        update_exit_state();
        return true;
    }

    std::int32_t exit_code() const noexcept override {
        return exited_ ? exit_code_ : 0;
    }

    Status terminate(std::uint32_t exit_code) override {
        if (process_ == nullptr)
            return make_error(ErrorCode::ProcessError, 0, "process was not started");
        if (exited_)
            return Status{};
        if (!TerminateProcess(process_, exit_code))
            return win32_error(ErrorCode::ProcessError, "TerminateProcess");
        update_exit_state();
        return Status{};
    }

    Status kill() override {
        return terminate(1);
    }

    Result<std::size_t> read_stdout(void *buffer, std::size_t bytes) override {
        return read_pipe(stdout_read_, buffer, bytes, "stdout");
    }

    Result<std::size_t> read_stderr(void *buffer, std::size_t bytes) override {
        return read_pipe(stderr_read_, buffer, bytes, "stderr");
    }

    Status write_stdin(const void *data, std::size_t bytes) override {
        if (stdin_write_ == nullptr)
            return make_error(ErrorCode::NotSupported, 0, "stdin was not redirected");
        const auto *pointer = static_cast<const std::uint8_t *>(data);
        std::size_t remaining = bytes;
        while (remaining > 0) {
            if (remaining > 0xFFFFFFFFu)
                return make_error(ErrorCode::InvalidArgument, 0, "stdin write exceeds 4 GiB");
            DWORD written = 0;
            if (!WriteFile(stdin_write_, pointer, static_cast<DWORD>(remaining), &written, nullptr))
                return win32_error(ErrorCode::IoError, "WriteFile(stdin)");
            pointer += written;
            remaining -= written;
        }
        return Status{};
    }

    void close_stdin() override {
        if (stdin_write_ != nullptr) {
            CloseHandle(stdin_write_);
            stdin_write_ = nullptr;
        }
    }

    void *native_handle() noexcept override {
        return process_;
    }

  private:
    Result<std::size_t> read_pipe(HANDLE pipe, void *buffer, std::size_t bytes,
                                  const char *name) {
        if (pipe == nullptr)
            return make_error(ErrorCode::NotSupported, 0, std::string{"stream "} + name + " was not captured");
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            // Once the child closes its end, the pipe reports a broken pipe;
            // that is end-of-stream for the reader, not an I/O failure.
            if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA)
                return static_cast<std::size_t>(0);
            return win32_error(ErrorCode::IoError, "PeekNamedPipe");
        }
        if (available == 0)
            return static_cast<std::size_t>(0);
        const DWORD to_read = static_cast<DWORD>(std::min<std::size_t>(bytes, available));
        if (to_read == 0)
            return static_cast<std::size_t>(0);
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, to_read, &read, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA)
                return static_cast<std::size_t>(0);
            return win32_error(ErrorCode::IoError, "ReadFile(pipe)");
        }
        return static_cast<std::size_t>(read);
    }

    void update_exit_state() noexcept {
        if (exited_ || process_ == nullptr)
            return;
        DWORD code = 0;
        if (GetExitCodeProcess(process_, &code) && code != STILL_ACTIVE) {
            exit_code_ = static_cast<std::int32_t>(code);
            exited_ = true;
        }
    }

    void close_streams() noexcept {
        close_stdin();
        if (stdout_read_ != nullptr) {
            CloseHandle(stdout_read_);
            stdout_read_ = nullptr;
        }
        if (stderr_read_ != nullptr) {
            CloseHandle(stderr_read_);
            stderr_read_ = nullptr;
        }
    }

    HANDLE process_{nullptr};
    HANDLE stdout_read_{nullptr};
    HANDLE stderr_read_{nullptr};
    HANDLE stdin_write_{nullptr};
    DWORD id_{0};
    std::int32_t exit_code_{0};
    bool exited_{false};
    SystemError error_;
};

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IProcess>> make_windows_process(const ProcessOptions &options) {
    if (options.executable.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "process executable is empty");

    const std::wstring wide_executable = wide_from_utf8(options.executable);
    const std::wstring command_line = quote_command_line(options.executable, options.arguments);
    const std::wstring working_directory = wide_from_utf8(options.working_directory);

    bool environment_changed = false;
    const std::wstring environment_block =
        build_environment_block(options.inherit_environment, options.environment, environment_changed);

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE child_stdout = nullptr;
    HANDLE parent_stdout = nullptr;
    HANDLE child_stderr = nullptr;
    HANDLE parent_stderr = nullptr;
    HANDLE child_stdin = nullptr;
    HANDLE parent_stdin = nullptr;

    if (options.capture_stdout &&
        !CreatePipe(&parent_stdout, &child_stdout, &security, 0)) {
        return win32_error(ErrorCode::ProcessError, "CreatePipe(stdout)");
    }
    if (options.capture_stderr &&
        !CreatePipe(&parent_stderr, &child_stderr, &security, 0)) {
        if (parent_stdout != nullptr)
            CloseHandle(parent_stdout);
        if (child_stdout != nullptr)
            CloseHandle(child_stdout);
        return win32_error(ErrorCode::ProcessError, "CreatePipe(stderr)");
    }
    if (options.redirect_stdin &&
        !CreatePipe(&child_stdin, &parent_stdin, &security, 0)) {
        if (parent_stdout != nullptr)
            CloseHandle(parent_stdout);
        if (child_stdout != nullptr)
            CloseHandle(child_stdout);
        if (parent_stderr != nullptr)
            CloseHandle(parent_stderr);
        if (child_stderr != nullptr)
            CloseHandle(child_stderr);
        return win32_error(ErrorCode::ProcessError, "CreatePipe(stdin)");
    }

    // The parent side of each pipe must not leak into the child.
    if (parent_stdout != nullptr)
        (void)SetHandleInformation(parent_stdout, HANDLE_FLAG_INHERIT, 0);
    if (parent_stderr != nullptr)
        (void)SetHandleInformation(parent_stderr, HANDLE_FLAG_INHERIT, 0);
    if (parent_stdin != nullptr)
        (void)SetHandleInformation(parent_stdin, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (child_stdout != nullptr || child_stderr != nullptr || child_stdin != nullptr) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = child_stdout != nullptr ? child_stdout : GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = child_stderr != nullptr ? child_stderr : GetStdHandle(STD_ERROR_HANDLE);
        startup.hStdInput = child_stdin != nullptr ? child_stdin : GetStdHandle(STD_INPUT_HANDLE);
    }

    PROCESS_INFORMATION process_info{};
    DWORD creation_flags = 0;
    if (options.create_no_window)
        creation_flags |= CREATE_NO_WINDOW;
    if (environment_changed)
        creation_flags |= CREATE_UNICODE_ENVIRONMENT;

    std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');

    const BOOL created =
        CreateProcessW(wide_executable.c_str(), mutable_command_line.data(), nullptr, nullptr,
                       TRUE, creation_flags,
                       environment_changed && !environment_block.empty()
                           ? const_cast<wchar_t *>(environment_block.data())
                           : nullptr,
                       working_directory.empty() ? nullptr : working_directory.c_str(), &startup,
                       &process_info);

    if (!created) {
        const SystemError error = win32_error(ErrorCode::ProcessError, "CreateProcessW");
        if (parent_stdout != nullptr)
            CloseHandle(parent_stdout);
        if (child_stdout != nullptr)
            CloseHandle(child_stdout);
        if (parent_stderr != nullptr)
            CloseHandle(parent_stderr);
        if (child_stderr != nullptr)
            CloseHandle(child_stderr);
        if (parent_stdin != nullptr)
            CloseHandle(parent_stdin);
        if (child_stdin != nullptr)
            CloseHandle(child_stdin);
        return error;
    }

    // The child owns these copies now.
    if (child_stdout != nullptr)
        CloseHandle(child_stdout);
    if (child_stderr != nullptr)
        CloseHandle(child_stderr);
    if (child_stdin != nullptr)
        CloseHandle(child_stdin);
    CloseHandle(process_info.hThread);

    auto process = std::make_unique<WindowsProcess>();
    process->store(process_info.hProcess, process_info.dwProcessId);
    process->store_streams(parent_stdout, parent_stderr, parent_stdin);
    return std::unique_ptr<IProcess>(std::move(process));
}

// ---------------------------------------------------------------------------
// WindowsSystem entry points
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IProcess>> WindowsSystem::spawn_process(const ProcessOptions &options) {
    return make_windows_process(options);
}

// ---------------------------------------------------------------------------
// Environment (WindowsSystem entry points)
// ---------------------------------------------------------------------------
std::string WindowsSystem::environment_variable(const std::string &name) const {
    const std::wstring wide_name = wide_from_utf8(name);
    if (wide_name.empty())
        return {};
    const DWORD size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (size == 0)
        return {};
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(wide_name.c_str(), value.data(), size);
    if (written == 0 || written >= size)
        return {};
    value.resize(written);
    return utf8_from_wide(value);
}

Status WindowsSystem::set_environment_variable(const std::string &name, const std::string &value) {
    const std::wstring wide_name = wide_from_utf8(name);
    const std::wstring wide_value = wide_from_utf8(value);
    if (wide_name.empty() || wide_name.find(L'=') != std::wstring::npos)
        return make_error(ErrorCode::InvalidArgument, 0, "environment name is invalid");
    if (!SetEnvironmentVariableW(wide_name.c_str(), wide_value.c_str()))
        return win32_error(ErrorCode::SystemFailure, "SetEnvironmentVariableW");
    return Status{};
}

Status WindowsSystem::unset_environment_variable(const std::string &name) {
    const std::wstring wide_name = wide_from_utf8(name);
    if (wide_name.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "environment name is empty");
    if (!SetEnvironmentVariableW(wide_name.c_str(), nullptr) && GetLastError() != ERROR_ENVVAR_NOT_FOUND)
        return win32_error(ErrorCode::SystemFailure, "SetEnvironmentVariableW(unset)");
    return Status{};
}

std::vector<EnvEntry> WindowsSystem::environment() const {
    std::vector<EnvEntry> entries;
    LPWCH block = GetEnvironmentStringsW();
    if (block == nullptr)
        return entries;
    for (const wchar_t *cursor = block; *cursor != L'\0';) {
        const std::wstring entry{cursor};
        cursor += entry.size() + 1;
        const std::size_t separator = entry.find(L'=');
        if (separator == std::wstring::npos || separator == 0)
            continue;
        EnvEntry value;
        value.name = utf8_from_wide(entry.substr(0, separator));
        value.value = utf8_from_wide(entry.substr(separator + 1));
        entries.push_back(std::move(value));
    }
    FreeEnvironmentStringsW(block);
    return entries;
}

} // namespace shinkou::platform::detail
