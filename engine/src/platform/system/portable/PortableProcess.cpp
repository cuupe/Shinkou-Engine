// Portable child processes and environment access. Spawning goes through the C
// runtime's system(), so pipes are not available and exit codes are decoded
// back into the portable contract.

#include "PortableBackend.h"

#include "PortableCommon.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace shinkou::platform::detail {
namespace {

std::string shell_quote(std::string_view argument) {
    if (!argument.empty() && argument.find_first_of(" \t\"") == std::string_view::npos)
        return std::string{argument};
    std::string quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back('"');
    for (const char character : argument) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
        // cmd.exe treats backslashes literally; only quotes need doubling.
        if (character == '"')
            quoted.push_back('"');
#else
        if (character == '"' || character == '\\')
            quoted.push_back('\\');
#endif
        quoted.push_back(character);
    }
    quoted.push_back('"');
    return quoted;
}

std::string build_command_line(const ProcessOptions &options) {
    std::string command_line = shell_quote(options.executable);
    for (const std::string &argument : options.arguments) {
        command_line.push_back(' ');
        command_line += shell_quote(argument);
    }
    return command_line;
}

int decode_system_status(int status) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
    return status;
#else
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return status;
#endif
}

struct PortableProcessState {
    std::atomic<bool> finished{false};
    std::atomic<int> exit_code{0};
};

} // namespace

class PortableProcess final : public IProcess {
  public:
    PortableProcess(std::shared_ptr<PortableProcessState> state, std::string command)
        : state_(std::move(state)) {
        worker_ = std::thread([state = state_, command = std::move(command)] {
            const int status = std::system(command.c_str());
            state->exit_code.store(decode_system_status(status), std::memory_order_release);
            state->finished.store(true, std::memory_order_release);
        });
    }

    ~PortableProcess() override {
        if (worker_.joinable())
            worker_.detach();
    }

    std::uint64_t id() const noexcept override {
        return 0;
    }

    bool is_running() override {
        return !state_->finished.load(std::memory_order_acquire);
    }

    Status wait() override {
        (void)wait_for(kInfiniteTimeout);
        return Status{};
    }

    bool wait_for(Nanoseconds timeout) override {
        if (timeout < 0) {
            if (worker_.joinable())
                worker_.join();
            return true;
        }
        const auto start = std::chrono::steady_clock::now();
        while (!state_->finished.load(std::memory_order_acquire)) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            if (elapsed >= timeout)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        if (worker_.joinable())
            worker_.join();
        return true;
    }

    std::int32_t exit_code() const noexcept override {
        return state_->finished.load(std::memory_order_acquire) ? state_->exit_code.load()
                                                                : 0;
    }

    Status terminate(std::uint32_t) override {
        return make_error(ErrorCode::NotSupported, 0,
                          "the portable backend cannot terminate a system() child");
    }

    Status kill() override {
        return terminate(1);
    }

    Result<std::size_t> read_stdout(void *, std::size_t) override {
        return make_error(ErrorCode::NotSupported, 0, "stdout capture is not supported");
    }

    Result<std::size_t> read_stderr(void *, std::size_t) override {
        return make_error(ErrorCode::NotSupported, 0, "stderr capture is not supported");
    }

    Status write_stdin(const void *, std::size_t) override {
        return make_error(ErrorCode::NotSupported, 0, "stdin redirection is not supported");
    }

    void close_stdin() override {
    }

    void *native_handle() noexcept override {
        return nullptr;
    }

  private:
    std::shared_ptr<PortableProcessState> state_;
    std::thread worker_;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
Result<std::unique_ptr<IProcess>> make_portable_process(const ProcessOptions &options) {
    if (options.executable.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "process executable is empty");
    auto state = std::make_shared<PortableProcessState>();
    return std::unique_ptr<IProcess>(
        std::make_unique<PortableProcess>(std::move(state), build_command_line(options)));
}

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------
std::string PortableSystem::environment_variable(const std::string &name) const {
    if (name.empty())
        return {};
#if defined(SHINKOU_PLATFORM_WINDOWS)
    const std::wstring wide_name = wide_from_utf8(name);
    const DWORD size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (size == 0)
        return {};
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(wide_name.c_str(), value.data(), size);
    if (written == 0 || written >= size)
        return {};
    value.resize(written);
    return utf8_from_wide(value);
#else
    const char *value = std::getenv(name.c_str());
    return value != nullptr ? std::string{value} : std::string{};
#endif
}

Status PortableSystem::set_environment_variable(const std::string &name,
                                                const std::string &value) {
    if (name.empty() || name.find('=') != std::string::npos)
        return make_error(ErrorCode::InvalidArgument, 0, "environment name is invalid");
#if defined(SHINKOU_PLATFORM_WINDOWS)
    const std::wstring wide_name = wide_from_utf8(name);
    const std::wstring wide_value = wide_from_utf8(value);
    if (!SetEnvironmentVariableW(wide_name.c_str(), wide_value.c_str()))
        return win32_error(ErrorCode::SystemFailure, "SetEnvironmentVariableW");
    return Status{};
#else
    if (setenv(name.c_str(), value.c_str(), 1) != 0)
        return portable_last_error(ErrorCode::SystemFailure, "setenv");
    return Status{};
#endif
}

Status PortableSystem::unset_environment_variable(const std::string &name) {
    if (name.empty())
        return make_error(ErrorCode::InvalidArgument, 0, "environment name is empty");
#if defined(SHINKOU_PLATFORM_WINDOWS)
    const std::wstring wide_name = wide_from_utf8(name);
    if (!SetEnvironmentVariableW(wide_name.c_str(), nullptr) &&
        GetLastError() != ERROR_ENVVAR_NOT_FOUND)
        return win32_error(ErrorCode::SystemFailure, "SetEnvironmentVariableW(unset)");
    return Status{};
#else
    if (unsetenv(name.c_str()) != 0)
        return portable_last_error(ErrorCode::SystemFailure, "unsetenv");
    return Status{};
#endif
}

std::vector<EnvEntry> PortableSystem::environment() const {
    std::vector<EnvEntry> entries;
#if defined(SHINKOU_PLATFORM_WINDOWS)
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
#else
    extern char **environ;
    for (char **cursor = environ; cursor != nullptr && *cursor != nullptr; ++cursor) {
        const std::string entry{*cursor};
        const std::size_t separator = entry.find('=');
        if (separator == std::string::npos || separator == 0)
            continue;
        EnvEntry value;
        value.name = entry.substr(0, separator);
        value.value = entry.substr(separator + 1);
        entries.push_back(std::move(value));
    }
#endif
    return entries;
}

} // namespace shinkou::platform::detail
