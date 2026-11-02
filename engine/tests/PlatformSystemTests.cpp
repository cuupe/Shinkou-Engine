// Platform system contract tests.
//
// Every check runs against both backends where the backend advertises the
// capability: the Windows native backend (kernel objects) and the portable
// standard-library backend. Capability-gated sections double as documentation
// of what each backend promises.

#include "shinkou/platform/System.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace shinkou::platform;

namespace {

int g_failures = 0;
std::string g_backend = "none";
std::string g_current_test = "init";
std::atomic<int> g_unique_counter{0};

void check(bool condition, const char *expression, int line) {
    if (condition)
        return;
    ++g_failures;
    std::cerr << "[FAIL] " << g_backend << " / " << g_current_test << " (" << __FILE__ << ":"
              << line << "): " << expression << "\n";
}

#define CHECK(expression) check((expression), #expression, __LINE__)

std::string unique_suffix(const ISystem &system) {
    return std::to_string(system.info().process_id) + "_" +
           std::to_string(g_unique_counter.fetch_add(1));
}

std::string join_path(const std::string &base, const std::string &child) {
    if (base.empty())
        return child;
    if (base.back() == '/' || base.back() == '\\')
        return base + child;
    return base + "/" + child;
}

std::string read_file(ISystem &system, const std::string &path) {
    OpenOptions options;
    options.mode = OpenMode::Read;
    options.share = ShareMode::ReadWrite;
    auto file = system.open_file(path, options);
    if (!file.ok())
        return {};
    std::string content;
    char buffer[4096];
    for (;;) {
        const auto read = file.value()->read(buffer, sizeof(buffer));
        if (!read.ok() || read.value() == 0)
            break;
        content.append(buffer, read.value());
    }
    return content;
}

bool write_file(ISystem &system, const std::string &path, std::string_view content) {
    OpenOptions options;
    options.mode = OpenMode::ReadWrite;
    options.share = ShareMode::ReadWrite;
    options.create_if_missing = true;
    options.truncate = true;
    auto file = system.open_file(path, options);
    if (!file.ok())
        return false;
    return file.value()->write_all(content.data(), content.size()).ok() &&
           file.value()->flush().ok();
}

Nanoseconds seconds(std::int64_t value) {
    return value * kNanosecondsPerSecond;
}

// ---------------------------------------------------------------------------
// Crash child helper (separate process so the handler can actually run).
// ---------------------------------------------------------------------------
std::FILE *g_crash_file = nullptr;

void crash_child_handler(const CrashReport &report) {
    if (g_crash_file != nullptr) {
        std::fprintf(g_crash_file, "code=%u frames=%zu thread=%llu\n", report.code,
                     report.frame_count,
                     static_cast<unsigned long long>(report.thread_id));
        std::fflush(g_crash_file);
        std::fclose(g_crash_file);
        g_crash_file = nullptr;
    }
    std::_Exit(3);
}

int run_crash_child(const std::string &marker) {
    auto system = make_system(BackendKind::Native);
    if (!system || !system->supports(Capability::CrashHandler))
        return 2;
    if (!system->install_crash_handler(&crash_child_handler))
        return 3;
    g_crash_file = std::fopen(marker.c_str(), "wb");
    if (g_crash_file == nullptr)
        return 4;
    volatile int *invalid_pointer = nullptr;
    *invalid_pointer = 42; // intentional access violation
    return 5;
}

// ---------------------------------------------------------------------------
// Identity, time, topology
// ---------------------------------------------------------------------------
void test_identity(ISystem &system) {
    g_current_test = "identity";
    CHECK(system.backend() != BackendKind::Auto);
    CHECK(std::strlen(system.backend_name()) > 0);
    CHECK(system.supports(Capability::VirtualMemory));
    CHECK(system.supports(Capability::ThreadLocalStorage));
    CHECK(system.capabilities() != 0);

    const SystemInfo &info = system.info();
    CHECK(info.backend == system.backend());
    CHECK(info.os != OperatingSystem::Unknown);
    CHECK(info.architecture != CpuArchitecture::Unknown);
    CHECK(info.logical_cores >= 1);
    CHECK(info.physical_cores >= 1);
    CHECK(info.page_size > 0);
    CHECK(info.allocation_granularity >= info.page_size);
    CHECK(!info.executable_path.empty());
    CHECK(info.process_id != 0);
    CHECK(system.page_size() == info.page_size);
    CHECK(system.allocation_granularity() == info.allocation_granularity);
    CHECK(system.hardware_thread_count() >= 1);
    CHECK(system.current_process_id() == info.process_id);
    CHECK(system.current_thread_id() != 0);
    CHECK(system.memory_status().total_physical > 0);
}

void test_clock(ISystem &system) {
    g_current_test = "clock";
    const Nanoseconds before = system.monotonic_time();
    system.sleep_for(milliseconds(5));
    const Nanoseconds after = system.monotonic_time();
    CHECK(after > before);

    // 2020-01-01T00:00:00Z in nanoseconds.
    CHECK(system.wall_time() > 1577836800LL * kNanosecondsPerSecond);

    const CalendarTime utc = system.calendar_time(false);
    CHECK(utc.year >= 2020);
    CHECK(utc.month >= 1 && utc.month <= 12);
    CHECK(utc.day >= 1 && utc.day <= 31);
    CHECK(utc.day_of_week >= 0 && utc.day_of_week <= 6);
    CHECK(utc.day_of_year >= 1 && utc.day_of_year <= 366);

    const CalendarTime local = system.calendar_time(true);
    CHECK(local.year >= 2020);
    CHECK(system.timer_resolution() > 0);
    CHECK(system.uptime() > 0);
}

// ---------------------------------------------------------------------------
// Threads and TLS
// ---------------------------------------------------------------------------
void test_threads(ISystem &system) {
    g_current_test = "threads";
    std::atomic<int> counter{0};
    auto thread = system.create_thread(ThreadDesc{}, [&counter] { counter.fetch_add(1); });
    CHECK(thread.ok());
    if (!thread.ok())
        return;
    CHECK(thread.value()->id() != 0);
    CHECK(thread.value()->id() != system.current_thread_id());
    CHECK(thread.value()->join_for(seconds(10)));
    CHECK(!thread.value()->joinable());
    CHECK(counter.load() == 1);

    std::atomic<bool> started{false};
    auto sleeper = system.create_thread(ThreadDesc{}, [&system, &started] {
        started.store(true);
        system.sleep_for(milliseconds(80));
    });
    CHECK(sleeper.ok());
    if (sleeper.ok()) {
        while (!started.load())
            system.sleep_for(milliseconds(1));
        CHECK(!sleeper.value()->join_for(0)); // still running
        CHECK(sleeper.value()->join_for(seconds(10)));
        CHECK(!sleeper.value()->joinable());
    }

    auto local = system.create_thread_local();
    CHECK(local.ok());
    if (local.ok()) {
        auto *main_value = reinterpret_cast<void *>(static_cast<std::uintptr_t>(0x1234));
        auto *worker_value = reinterpret_cast<void *>(static_cast<std::uintptr_t>(0x5678));
        local.value()->set(main_value);
        CHECK(local.value()->get() == main_value);

        std::atomic<bool> worker_isolated{false};
        auto tls_thread = system.create_thread(ThreadDesc{}, [&local, &worker_isolated,
                                                              worker_value] {
            const bool initially_empty = local.value()->get() == nullptr;
            local.value()->set(worker_value);
            worker_isolated.store(initially_empty && local.value()->get() == worker_value);
        });
        CHECK(tls_thread.ok());
        if (tls_thread.ok()) {
            tls_thread.value()->join();
            CHECK(worker_isolated.load());
            CHECK(local.value()->get() == main_value);
        }
        local.value()->set(nullptr);
    }

    if (system.supports(Capability::ThreadPriority)) {
        CHECK(system.set_current_thread_priority(ThreadPriority::Normal));
        CHECK(system.current_thread_priority() == ThreadPriority::Normal);
    }
    if (system.supports(Capability::ThreadNaming))
        CHECK(system.set_current_thread_name("shinkou-platform-test"));
    if (system.supports(Capability::ThreadAffinity)) {
        const std::uint64_t mask = system.current_thread_affinity();
        CHECK(mask != 0);
        CHECK(system.set_current_thread_affinity(mask));
    }
}

// ---------------------------------------------------------------------------
// Synchronization
// ---------------------------------------------------------------------------
void test_sync(ISystem &system) {
    g_current_test = "sync";
    auto mutex = system.create_mutex(MutexOptions{});
    CHECK(mutex.ok());
    if (mutex.ok()) {
        mutex.value()->lock();
        mutex.value()->unlock();
        CHECK(mutex.value()->try_lock());
        mutex.value()->unlock();
    }

    MutexOptions recursive_options;
    recursive_options.recursive = true;
    auto recursive = system.create_mutex(recursive_options);
    CHECK(recursive.ok());
    if (recursive.ok()) {
        recursive.value()->lock();
        CHECK(recursive.value()->try_lock());
        recursive.value()->unlock();
        recursive.value()->unlock();
    }

    if (system.supports(Capability::NamedKernelObjects)) {
        MutexOptions named_options;
        named_options.name = "Local\\shinkou_platform_mutex_" + unique_suffix(system);
        auto named = system.create_mutex(named_options);
        CHECK(named.ok());
        if (named.ok()) {
            named.value()->lock();
            named.value()->unlock();
        }
    }

    auto condition = system.create_condition_variable();
    CHECK(condition.ok());
    CHECK(system.create_mutex(MutexOptions{}).ok());
    if (mutex.ok() && condition.ok()) {
        std::atomic<bool> ready{false};
        std::atomic<bool> go{false};
        std::atomic<bool> woke{false};
        auto worker = system.create_thread(ThreadDesc{}, [&] {
            mutex.value()->lock();
            ready.store(true);
            condition.value()->notify_all();
            while (!go.load()) {
                if (!condition.value()->wait_for(*mutex.value(), milliseconds(10)))
                    continue;
            }
            mutex.value()->unlock();
            woke.store(true);
        });
        CHECK(worker.ok());
        if (worker.ok()) {
            bool timed_out = false;
            mutex.value()->lock();
            const Nanoseconds deadline = system.monotonic_time() + seconds(10);
            while (!ready.load()) {
                if (system.monotonic_time() > deadline) {
                    timed_out = true;
                    break;
                }
                condition.value()->wait_for(*mutex.value(), milliseconds(10));
            }
            go.store(true);
            condition.value()->notify_all();
            mutex.value()->unlock();
            CHECK(!timed_out);
            CHECK(worker.value()->join_for(seconds(10)));
            CHECK(woke.load());
        }
    }

    SemaphoreOptions semaphore_options;
    semaphore_options.initial_count = 0;
    semaphore_options.maximum_count = 2;
    auto semaphore = system.create_semaphore(semaphore_options);
    CHECK(semaphore.ok());
    if (semaphore.ok()) {
        CHECK(!semaphore.value()->try_wait());
        CHECK(!semaphore.value()->wait_for(0));
        semaphore.value()->post(1);
        CHECK(semaphore.value()->wait_for(seconds(5)));
        CHECK(!semaphore.value()->try_wait());
        semaphore.value()->post(2);
        CHECK(semaphore.value()->try_wait());
        CHECK(semaphore.value()->try_wait());
        CHECK(!semaphore.value()->try_wait());
    }

    SemaphoreOptions invalid_semaphore;
    invalid_semaphore.initial_count = 3;
    invalid_semaphore.maximum_count = 2;
    CHECK(!system.create_semaphore(invalid_semaphore).ok());

    EventOptions manual_options;
    manual_options.manual_reset = true;
    auto manual = system.create_event(manual_options);
    CHECK(manual.ok());
    if (manual.ok()) {
        CHECK(!manual.value()->is_signalled());
        CHECK(!manual.value()->wait_for(0));
        manual.value()->set();
        CHECK(manual.value()->is_signalled());
        CHECK(manual.value()->wait_for(0));
        CHECK(manual.value()->wait_for(0)); // manual reset keeps the signal
        manual.value()->reset();
        CHECK(!manual.value()->wait_for(0));
    }

    EventOptions automatic_options;
    automatic_options.manual_reset = false;
    auto automatic = system.create_event(automatic_options);
    CHECK(automatic.ok());
    if (automatic.ok()) {
        automatic.value()->set();
        CHECK(automatic.value()->is_signalled());
        CHECK(automatic.value()->wait_for(0)); // consumes
        CHECK(!automatic.value()->wait_for(0));
    }

    if (system.supports(Capability::NamedKernelObjects)) {
        EventOptions named_event_options;
        named_event_options.manual_reset = true;
        named_event_options.initial_state = true;
        named_event_options.name = "Local\\shinkou_platform_event_" + unique_suffix(system);
        auto named_event = system.create_event(named_event_options);
        CHECK(named_event.ok());
        if (named_event.ok())
            CHECK(named_event.value()->is_signalled());
    }
}

// ---------------------------------------------------------------------------
// Fibers
// ---------------------------------------------------------------------------
void test_fibers(ISystem &system) {
    g_current_test = "fibers";
    if (!system.supports(Capability::Fibers)) {
        CHECK(!system.convert_current_thread_to_fiber());
        CHECK(!system.current_thread_is_fiber());
        return;
    }

    std::atomic<bool> converted{false};
    std::atomic<bool> stage_one{false};
    std::atomic<bool> stage_two{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> reverted{false};
    auto thread = system.create_thread(ThreadDesc{}, [&] {
        if (!system.convert_current_thread_to_fiber())
            return;
        converted.store(true);
        auto fiber = system.create_fiber([&] {
            stage_one.store(true);
            system.yield_fiber();
            stage_two.store(true);
        });
        if (!fiber.ok()) {
            system.revert_current_thread_from_fiber();
            return;
        }
        fiber.value()->resume();
        const bool yielded_before_second_stage = stage_one.load() && !stage_two.load();
        fiber.value()->resume();
        finished.store(fiber.value()->finished() && yielded_before_second_stage);
        fiber.value().reset();
        reverted.store(system.revert_current_thread_from_fiber());
    });
    CHECK(thread.ok());
    if (thread.ok()) {
        CHECK(thread.value()->join_for(seconds(10)));
        CHECK(converted.load());
        CHECK(stage_one.load());
        CHECK(stage_two.load());
        CHECK(finished.load());
        CHECK(reverted.load());
    }
}

// ---------------------------------------------------------------------------
// Virtual memory
// ---------------------------------------------------------------------------
void test_memory(ISystem &system) {
    g_current_test = "memory";
    const std::size_t region_bytes = system.page_size() * 4;
    auto region = system.reserve_memory(region_bytes, MemoryFlags::None);
    CHECK(region.ok());
    if (region.ok()) {
        CHECK(region.value()->address() != nullptr);
        CHECK(region.value()->size() >= region_bytes);
        CHECK(region.value()->commit(0, region_bytes).ok());
        auto *data = static_cast<unsigned char *>(region.value()->address());
        std::memset(data, 0x5A, region_bytes);
        CHECK(data[0] == 0x5A);
        CHECK(data[region_bytes - 1] == 0x5A);
        CHECK(region.value()->protect(0, region_bytes, MemoryProtection::Read).ok());
        CHECK(region.value()->protect(0, region_bytes, MemoryProtection::ReadWrite).ok());
        // Page locking is best effort: quota and privileges vary per host.
        (void)region.value()->lock(0, region_bytes);
        (void)region.value()->unlock(0, region_bytes);
        CHECK(region.value()->decommit(0, region_bytes).ok());
        CHECK(region.value()->commit(0, region_bytes).ok());
        data[0] = 0x11;
        CHECK(region.value()->release().ok());
        CHECK(region.value()->address() == nullptr);
    }

    CHECK(!system.reserve_memory(0, MemoryFlags::None).ok());

    if (system.supports(Capability::FileMapping)) {
        const std::string path =
            join_path(system.temp_directory(), "shinkou_map_" + unique_suffix(system) + ".bin");
        std::string content(system.page_size(), '\0');
        for (std::size_t index = 0; index < content.size(); ++index)
            content[index] = static_cast<char>(index % 251);
        CHECK(write_file(system, path, content));

        {
            auto mapped = system.map_file(path, 0, 0, MemoryProtection::Read);
            CHECK(mapped.ok());
            if (mapped.ok()) {
                CHECK(mapped.value()->size() == content.size());
                CHECK(std::memcmp(mapped.value()->data(), content.data(), content.size()) == 0);
                CHECK(mapped.value()->flush().ok());
            }
        }
        {
            auto mapped = system.map_file(path, 0, 0, MemoryProtection::ReadWrite);
            CHECK(mapped.ok());
            if (mapped.ok()) {
                static_cast<unsigned char *>(mapped.value()->data())[0] = 0x7E;
                CHECK(mapped.value()->flush().ok());
            }
        }
        CHECK(read_file(system, path).at(0) == '\x7E');
        CHECK(system.remove_file(path).ok());
        CHECK(!system.map_file(path, 0, 0, MemoryProtection::Read).ok());
    }

    if (system.supports(Capability::SharedMemory)) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
        const std::string name = "Local\\shinkou_platform_shm_" + unique_suffix(system);
#else
        // macOS limits POSIX shared-memory names to 31 characters including '/'.
        const std::string name = "/shinkou_shm_" + unique_suffix(system);
#endif
        const std::size_t bytes = system.page_size();
        auto first = system.create_shared_memory(name, bytes, MemoryProtection::ReadWrite);
        CHECK(first.ok());
        if (first.ok()) {
            auto second = system.open_shared_memory(name, bytes, MemoryProtection::ReadWrite);
            CHECK(second.ok());
            if (second.ok()) {
                std::memset(first.value()->data(), 0xA5, bytes);
                CHECK(static_cast<unsigned char *>(second.value()->data())[0] == 0xA5);
                static_cast<unsigned char *>(second.value()->data())[bytes - 1] = 0x3C;
                CHECK(static_cast<unsigned char *>(first.value()->data())[bytes - 1] == 0x3C);
                CHECK(first.value()->size() == bytes);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Files and directories
// ---------------------------------------------------------------------------
void test_files(ISystem &system) {
    g_current_test = "files";
    const std::string root =
        join_path(system.temp_directory(), "shinkou_platform_files_" + unique_suffix(system));
    CHECK(system.create_directory(root, true).ok());
    CHECK(system.exists(root));

    const std::string content = "hello platform";
    const std::string file = join_path(root, "hello.txt");
    CHECK(write_file(system, file, content));
    CHECK(system.exists(file));
    CHECK(read_file(system, file) == content);

    auto info = system.stat(file);
    CHECK(info.ok());
    if (info.ok()) {
        CHECK(!info.value().is_directory);
        CHECK(!info.value().is_symlink);
        CHECK(info.value().size == content.size());
        CHECK(info.value().name == "hello.txt");
        CHECK(info.value().modified_nanoseconds > 0);
    }

    auto directory = system.enumerate_directory(root);
    CHECK(directory.ok());
    if (directory.ok()) {
        bool found = false;
        std::size_t entries = 0;
        FileInfo entry;
        SystemError iteration_error;
        while (directory.value()->next(entry, iteration_error)) {
            ++entries;
            if (entry.name == "hello.txt") {
                found = true;
                CHECK(entry.size == content.size());
            }
        }
        CHECK(iteration_error.ok());
        CHECK(entries >= 1);
        CHECK(found);
    }

    const std::string nested = join_path(root, "a/b/c");
    CHECK(system.create_directory(nested, true).ok());
    CHECK(system.stat(nested).ok());

    const std::string copy = join_path(root, "copy.txt");
    CHECK(system.copy_file(file, copy, false).ok());
    CHECK(!system.copy_file(file, copy, false).ok()); // already exists
    CHECK(system.copy_file(file, copy, true).ok());
    const std::string renamed = join_path(root, "renamed.txt");
    CHECK(system.rename(copy, renamed).ok());
    CHECK(system.exists(renamed));
    CHECK(system.remove_file(renamed).ok());
    CHECK(!system.exists(renamed));
    CHECK(!system.remove_file(renamed).ok()); // already gone

    auto temporary = system.create_temp_file("shinkou_platform");
    CHECK(temporary.ok());
    if (temporary.ok()) {
        CHECK(system.exists(temporary.value()));
        CHECK(write_file(system, temporary.value(), "temp"));
        CHECK(read_file(system, temporary.value()) == "temp");
        CHECK(system.remove_file(temporary.value()).ok());
    }

    if (system.supports(Capability::FileWatch)) {
        auto watcher = system.watch_directory(root, true);
        CHECK(watcher.ok());
        if (watcher.ok()) {
            // Let the native watcher thread issue its first ReadDirectoryChangesW.
            system.sleep_for(milliseconds(50));
            const std::string watched = join_path(root, "watch_me.txt");
            CHECK(write_file(system, watched, "watched"));
            // A batch may first report the parent directory modification, so
            // accumulate events until the file itself shows up.
            bool saw_change = false;
            std::vector<FileChangeEvent> events;
            const Nanoseconds deadline = system.monotonic_time() + seconds(5);
            while (!saw_change && system.monotonic_time() < deadline) {
                if (!watcher.value()->wait_for_change(milliseconds(500)))
                    continue;
                watcher.value()->drain(events);
                for (const FileChangeEvent &event : events) {
                    if (event.path.find("watch_me.txt") != std::string::npos)
                        saw_change = true;
                }
            }
            CHECK(saw_change);
        }
    }

    const std::string original_directory = system.current_directory();
    CHECK(!original_directory.empty());
    CHECK(system.set_current_directory(root).ok());
    const std::string changed_directory = system.current_directory();
    CHECK(!changed_directory.empty());
    CHECK(changed_directory != original_directory);
    CHECK(system.set_current_directory(original_directory).ok());
    CHECK(system.current_directory() == original_directory);

    CHECK(system.remove_directory(root, true).ok());
    CHECK(!system.exists(root));
}

// ---------------------------------------------------------------------------
// Processes and environment
// ---------------------------------------------------------------------------
std::string read_captured_stream(IProcess &process, bool error_stream) {
    std::string output;
    char buffer[1024];
    for (;;) {
        const auto read = error_stream ? process.read_stderr(buffer, sizeof(buffer))
                                       : process.read_stdout(buffer, sizeof(buffer));
        if (!read.ok() || read.value() == 0)
            break;
        output.append(buffer, read.value());
    }
    return output;
}

void test_process(ISystem &system) {
    g_current_test = "process";
    const std::string variable = "SHINKOU_PLATFORM_TEST_" + unique_suffix(system);
    CHECK(system.set_environment_variable(variable, "value-42").ok());
    CHECK(system.environment_variable(variable) == "value-42");
    bool listed = false;
    for (const EnvEntry &entry : system.environment()) {
        if (entry.name == variable && entry.value == "value-42")
            listed = true;
    }
    CHECK(listed);
    CHECK(system.unset_environment_variable(variable).ok());
    CHECK(system.environment_variable(variable).empty());

    if (!system.supports(Capability::ProcessSpawn))
        return;

#if defined(SHINKOU_PLATFORM_WINDOWS)
    const std::string command_processor = system.environment_variable("ComSpec");
    const std::string shell_flag = "/c";
    const std::string child_environment_echo = "echo %SHINKOU_CHILD_VARIABLE%";
#else
    const std::string command_processor = "/bin/sh";
    const std::string shell_flag = "-c";
    const std::string child_environment_echo = "echo $SHINKOU_CHILD_VARIABLE";
#endif
    if (command_processor.empty()) {
        std::cout << "[platform] command processor is unavailable; skipping process spawn\n";
        return;
    }

    ProcessOptions exit_options;
    exit_options.executable = command_processor;
    exit_options.arguments = {shell_flag, "exit 7"};
    auto exit_process = system.spawn_process(exit_options);
    CHECK(exit_process.ok());
    if (exit_process.ok()) {
        CHECK(exit_process.value()->wait_for(seconds(15)));
        CHECK(!exit_process.value()->is_running());
        CHECK(exit_process.value()->exit_code() == 7);
    }

    if (system.supports(Capability::ProcessCapture)) {
        ProcessOptions echo_options;
        echo_options.executable = command_processor;
        echo_options.arguments = {shell_flag, "echo platform-hello"};
        echo_options.capture_stdout = true;
        auto echo_process = system.spawn_process(echo_options);
        CHECK(echo_process.ok());
        if (echo_process.ok()) {
            CHECK(echo_process.value()->wait_for(seconds(15)));
            const std::string output = read_captured_stream(*echo_process.value(), false);
            CHECK(output.find("platform-hello") != std::string::npos);
            CHECK(echo_process.value()->exit_code() == 0);
        }

        ProcessOptions error_options;
        error_options.executable = command_processor;
        error_options.arguments = {shell_flag, "echo platform-error 1>&2"};
        error_options.capture_stderr = true;
        auto error_process = system.spawn_process(error_options);
        CHECK(error_process.ok());
        if (error_process.ok()) {
            CHECK(error_process.value()->wait_for(seconds(15)));
            const std::string output = read_captured_stream(*error_process.value(), true);
            CHECK(output.find("platform-error") != std::string::npos);
        }

        ProcessOptions environment_options;
        environment_options.executable = command_processor;
        environment_options.arguments = {shell_flag, child_environment_echo};
        environment_options.capture_stdout = true;
        environment_options.environment = {{"SHINKOU_CHILD_VARIABLE", "child-value"}};
        auto child_environment = system.spawn_process(environment_options);
        CHECK(child_environment.ok());
        if (child_environment.ok()) {
            CHECK(child_environment.value()->wait_for(seconds(15)));
            const std::string output = read_captured_stream(*child_environment.value(), false);
            CHECK(output.find("child-value") != std::string::npos);
        }
    }

    ProcessOptions invalid_options;
    invalid_options.executable = "shinkou-definitely-missing-" + unique_suffix(system);
    const auto invalid = system.spawn_process(invalid_options);
    if (system.backend() == BackendKind::Native) {
#if defined(SHINKOU_PLATFORM_WINDOWS)
        CHECK(!invalid.ok());
#else
        // fork/exec reports a missing executable through the child exit code.
        if (invalid.ok()) {
            CHECK(invalid.value()->wait_for(seconds(15)));
            CHECK(invalid.value()->exit_code() != 0);
        }
#endif
    }
}

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------
void test_modules(ISystem &system) {
    g_current_test = "modules";
    if (!system.supports(Capability::DynamicModules))
        return;
#if defined(SHINKOU_PLATFORM_WINDOWS)
    auto library = system.load_library("kernel32.dll");
    CHECK(library.ok());
    if (!library.ok())
        return;
    auto symbol = library.value()->symbol("GetCurrentProcessId");
    CHECK(symbol.ok());
    void *symbol_address = symbol.ok() ? symbol.value() : nullptr;
    if (symbol.ok()) {
        using GetCurrentProcessIdFn = unsigned long(__stdcall *)();
        GetCurrentProcessIdFn function = nullptr;
        static_assert(sizeof(function) == sizeof(symbol_address));
        std::memcpy(&function, &symbol_address, sizeof(function));
        CHECK(function != nullptr);
        CHECK(static_cast<std::uint64_t>(function()) == system.current_process_id());
    }
    CHECK(!library.value()->symbol("ShinkouDefinitelyMissingSymbol").ok());
    CHECK(!library.value()->path().empty());

    if (system.supports(Capability::ModuleEnumeration)) {
        const std::vector<ModuleInfo> modules = system.modules();
        CHECK(!modules.empty());
        CHECK(!system.module_for_address(reinterpret_cast<const void *>(&g_failures)).empty());
        const std::string owner = system.module_for_address(symbol_address);
        CHECK(owner.find("kernel32") != std::string::npos ||
              owner.find("KERNEL32") != std::string::npos);
    }
#elif defined(SHINKOU_PLATFORM_APPLE)
    auto library = system.load_library("/usr/lib/libSystem.B.dylib");
    CHECK(library.ok());
    if (library.ok()) {
        CHECK(library.value()->symbol("cos").ok());
        CHECK(!library.value()->symbol("shinkou_missing_symbol").ok());
    }
#else
    auto library = system.load_library("libm.so.6");
    CHECK(library.ok());
    if (library.ok()) {
        CHECK(library.value()->symbol("cos").ok());
        CHECK(!library.value()->symbol("shinkou_missing_symbol").ok());
    }
#endif
#if defined(SHINKOU_PLATFORM_LINUX)
    if (system.supports(Capability::ModuleEnumeration)) {
        CHECK(!system.modules().empty());
        CHECK(!system.module_for_address(reinterpret_cast<const void *>(&g_failures)).empty());
    }
#endif
    CHECK(!system.load_library("").ok());
}

// ---------------------------------------------------------------------------
// Diagnostics and console
// ---------------------------------------------------------------------------
void test_diagnostics(ISystem &system) {
    g_current_test = "diagnostics";
    system.debug_output("shinkou platform test\n");
    (void)system.is_debugger_present();

    if (system.supports(Capability::StackTrace)) {
        StackFrame frames[32]{};
        const std::size_t count = system.capture_stack_trace(frames, 32, 0);
        CHECK(count > 0);
        CHECK(frames[0].address != nullptr);
        CHECK(count <= 32);
    }

    CHECK(system.write_console("shinkou platform test\n", StandardStream::Error).ok());
    (void)system.is_console_terminal(StandardStream::Output);
    (void)system.set_console_colors_enabled(false);
}

void test_crash_handler(ISystem &system) {
    g_current_test = "crash-handler";
    if (!system.supports(Capability::CrashHandler))
        return;
    const std::string marker =
        join_path(system.temp_directory(), "shinkou_crash_" + unique_suffix(system) + ".txt");
    ProcessOptions options;
    options.executable = system.info().executable_path;
    options.arguments = {"--platform-test-crash-child", marker};
    auto process = system.spawn_process(options);
    CHECK(process.ok());
    if (!process.ok())
        return;
    CHECK(process.value()->wait_for(seconds(20)));
    CHECK(process.value()->exit_code() != 0);
    CHECK(system.exists(marker));
    const std::string report = read_file(system, marker);
    CHECK(report.find("code=") != std::string::npos);
    CHECK(report.find("frames=") != std::string::npos);
    CHECK(system.remove_file(marker).ok());
}

// ---------------------------------------------------------------------------
// Factory and singleton
// ---------------------------------------------------------------------------
void test_factory() {
    g_current_test = "factory";
    g_backend = "factory";
    CHECK(backend_available(BackendKind::Portable));
    SystemError error;
    auto automatic = make_system(BackendKind::Auto, &error);
    CHECK(automatic != nullptr);
    CHECK(error.ok());

    if (backend_available(BackendKind::Native)) {
        auto native = make_system(BackendKind::Native, &error);
        CHECK(native != nullptr);
        CHECK(error.ok());
    } else {
        auto native = make_system(BackendKind::Native, &error);
        CHECK(native == nullptr);
        CHECK(!error.ok());
    }
}

void test_singleton() {
    g_current_test = "singleton";
    g_backend = "singleton";
    System::set_instance(nullptr);
    CHECK(!System::has_instance());
    CHECK(System::try_instance() == nullptr);

    ISystem &default_instance = System::instance();
    CHECK(System::has_instance());
    CHECK(default_instance.backend() != BackendKind::Auto);
    CHECK(!default_instance.info().executable_path.empty());

    auto portable = make_system(BackendKind::Portable, nullptr);
    CHECK(portable != nullptr);
    System::set_instance(std::move(portable));
    CHECK(System::instance().backend() == BackendKind::Portable);

    System::set_instance(nullptr);
    CHECK(!System::has_instance());
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------
void run_suite(ISystem &system, const std::string &name) {
    g_backend = name;
    std::cout << "[platform] " << name << ": " << system.backend_name() << "\n";
    test_identity(system);
    test_clock(system);
    test_threads(system);
    test_sync(system);
    test_fibers(system);
    test_memory(system);
    test_files(system);
    test_process(system);
    test_modules(system);
    test_diagnostics(system);
    test_crash_handler(system);
}

} // namespace

int main(int argc, char **argv) {
    if (argc >= 3 && std::string(argv[1]) == "--platform-test-crash-child")
        return run_crash_child(argv[2]);

    test_factory();

    if (auto native = make_system(BackendKind::Native, nullptr))
        run_suite(*native, "native");
    else
        std::cout << "[platform] native backend unavailable; running portable only\n";

    if (auto portable = make_system(BackendKind::Portable, nullptr))
        run_suite(*portable, "portable");

    test_singleton();

    if (g_failures != 0) {
        std::cerr << "[platform] " << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "[platform] all platform system checks passed\n";
    return 0;
}
