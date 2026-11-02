// POSIX diagnostics: stack traces through execinfo and signal-based crash
// reporting. The crash handler only builds a small report and forwards it to
// the registered callback; the callback must be async-signal-safe.

#include "PosixConfig.h"

#include "PosixBackend.h"
#include "PosixCommon.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <csignal>
#include <signal.h>
#include <unistd.h>

#if defined(SHINKOU_POSIX_HAS_EXECINFO)
#include <execinfo.h>
#endif

namespace shinkou::platform::detail {
namespace {

constexpr int kHandledSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
constexpr std::size_t kHandledSignalCount = sizeof(kHandledSignals) / sizeof(kHandledSignals[0]);

std::atomic<CrashHandler> g_crash_handler{nullptr};
std::atomic<void *> g_crash_user_data{nullptr};
std::atomic<bool> g_crash_installed{false};
struct sigaction g_previous_actions[kHandledSignalCount]{};

void crash_signal_handler(int signal_number, siginfo_t *info, void *) {
    const CrashHandler callback = g_crash_handler.load(std::memory_order_acquire);
    if (callback != nullptr) {
        CrashReport report;
        report.code = static_cast<std::uint32_t>(signal_number);
        report.address = info != nullptr ? info->si_addr : nullptr;
        report.thread_id = posix_thread_id_value();
        report.frames = nullptr;
        report.frame_count = 0;
        report.user_data = g_crash_user_data.load(std::memory_order_acquire);
        callback(report);
    }

    // Restore the default action and let the same signal terminate the
    // process; the handler must never return to the faulting instruction.
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    (void)sigaction(signal_number, &action, nullptr);
    (void)raise(signal_number);
    _exit(128 + signal_number);
}

} // namespace

std::size_t PosixSystem::capture_stack_trace(StackFrame *frames, std::size_t capacity,
                                             std::size_t skip) {
    if (frames == nullptr || capacity == 0)
        return 0;
#if defined(SHINKOU_POSIX_HAS_EXECINFO)
    constexpr std::size_t kMaxFrames = 128;
    void *raw[kMaxFrames]{};
    const std::size_t wanted = std::min(kMaxFrames, capacity + skip + 1);
    const int count = backtrace(raw, static_cast<int>(wanted));
    if (count <= 0)
        return 0;
    std::size_t written = 0;
    for (int index = static_cast<int>(skip) + 1; index < count && written < capacity; ++index)
        frames[written++].address = raw[index];
    return written;
#else
    (void)skip;
    return 0;
#endif
}

Status PosixSystem::install_crash_handler(CrashHandler handler, void *user_data) {
    if (handler == nullptr)
        return make_error(ErrorCode::InvalidArgument, 0, "crash handler is null");
    if (g_crash_installed.exchange(true, std::memory_order_acq_rel))
        return make_error(ErrorCode::AlreadyExists, 0, "a crash handler is already installed");

    g_crash_handler.store(handler, std::memory_order_release);
    g_crash_user_data.store(user_data, std::memory_order_release);

    struct sigaction action {};
    action.sa_sigaction = &crash_signal_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);

    for (std::size_t index = 0; index < kHandledSignalCount; ++index) {
        if (sigaction(kHandledSignals[index], &action, &g_previous_actions[index]) != 0) {
            const SystemError error = posix_last_error(ErrorCode::SystemFailure, "sigaction");
            for (std::size_t restore = 0; restore < index; ++restore)
                (void)sigaction(kHandledSignals[restore], &g_previous_actions[restore], nullptr);
            g_crash_handler.store(nullptr, std::memory_order_release);
            g_crash_user_data.store(nullptr, std::memory_order_release);
            g_crash_installed.store(false, std::memory_order_release);
            return error;
        }
    }
    return Status{};
}

void PosixSystem::uninstall_crash_handler() {
    if (!g_crash_installed.exchange(false, std::memory_order_acq_rel))
        return;
    for (std::size_t index = 0; index < kHandledSignalCount; ++index)
        (void)sigaction(kHandledSignals[index], &g_previous_actions[index], nullptr);
    g_crash_handler.store(nullptr, std::memory_order_release);
    g_crash_user_data.store(nullptr, std::memory_order_release);
}

} // namespace shinkou::platform::detail
