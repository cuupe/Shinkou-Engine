// Behavioural tests for shinkou::profile.
//
// Same rules as the job-system suite: no assert() (it must stay strict in Release
// builds) and no engine dependency, so the module can be proven on its own.

#include "shinkou/profile/Profiler.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using shinkou::profile::Profiler;
using shinkou::profile::ZoneId;

int g_failures = 0;
int g_checks = 0;

void report(bool condition, const char* expression, const char* file, int line) {
    ++g_checks;
    if (condition) return;
    std::fprintf(stderr, "  FAILED: %s  (%s:%d)\n", expression, file, line);
    ++g_failures;
}

#define CHECK(...) report((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)

Profiler& profiler() { return Profiler::instance(); }

void spin_for_ns(std::uint64_t nanoseconds) {
    const std::uint64_t start = shinkou::profile::detail::clock_ns();
    while (shinkou::profile::detail::clock_ns() - start < nanoseconds) {
        std::this_thread::yield();
    }
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

std::size_t count_of(const std::string& haystack, std::string_view needle) {
    std::size_t count = 0;
    std::size_t position = haystack.find(needle);
    while (position != std::string::npos) {
        ++count;
        position = haystack.find(needle, position + needle.size());
    }
    return count;
}

bool balanced(const std::string& json) {
    int braces = 0;
    int brackets = 0;
    bool inString = false;
    bool escaped = false;
    for (const char c : json) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        switch (c) {
        case '"': inString = true; break;
        case '{': ++braces; break;
        case '}': --braces; break;
        case '[': ++brackets; break;
        case ']': --brackets; break;
        default: break;
        }
        if (braces < 0 || brackets < 0) return false;
    }
    return braces == 0 && brackets == 0 && !inString;
}

// --------------------------------------------------------------------------

void test_zone_registration() {
    profiler().reset();
    const ZoneId a = profiler().register_zone("alpha");
    const ZoneId b = profiler().register_zone("alpha");
    const ZoneId c = profiler().register_zone("beta", "cat");
    CHECK(a != shinkou::profile::kInvalidZone);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(profiler().register_zone("") == shinkou::profile::kInvalidZone);
    CHECK(profiler().zone_name(a) == "alpha");
    CHECK(profiler().zone_category(c) == "cat");
}

void test_scope_records_a_span() {
    profiler().reset();
    const ZoneId zone = profiler().register_zone("work");
    {
        shinkou::profile::Scope scope(zone);
        spin_for_ns(200000); // 0.2 ms
    }
    const auto stats = profiler().stats();
    CHECK(stats.recorded == 1);
    CHECK(stats.frames == 0);

    const std::string json = profiler().chrome_trace_json();
    CHECK(contains(json, "\"name\":\"work\""));
    CHECK(contains(json, "\"ph\":\"X\""));
}

void test_nested_scopes_record_parent_and_child() {
    profiler().reset();
    const ZoneId outer = profiler().register_zone("outer");
    const ZoneId inner = profiler().register_zone("inner");
    {
        shinkou::profile::Scope a(outer);
        spin_for_ns(100000);
        {
            shinkou::profile::Scope b(inner);
            spin_for_ns(100000);
        }
    }
    const auto stats = profiler().stats();
    CHECK(stats.recorded == 2);
    const std::string json = profiler().chrome_trace_json();
    CHECK(contains(json, "\"name\":\"outer\""));
    CHECK(contains(json, "\"name\":\"inner\""));
}

void test_counter_records_value() {
    profiler().reset();
    shinkou::profile::counter("drawcalls", 42.0);
    const auto stats = profiler().stats();
    CHECK(stats.recorded == 1);
    const std::string json = profiler().chrome_trace_json();
    CHECK(contains(json, "\"name\":\"drawcalls\""));
    CHECK(contains(json, "\"ph\":\"C\""));
    CHECK(contains(json, "\"value\":42"));
}

void test_frame_records_duration_and_curve() {
    profiler().reset();
    profiler().begin_frame();
    spin_for_ns(1000000); // at least 1 ms; yield may overshoot, which is fine
    profiler().end_frame();
    CHECK(profiler().frame_index() == 1);

    const std::string json = profiler().chrome_trace_json();
    CHECK(contains(json, "\"name\":\"frame\""));
    CHECK(contains(json, "\"name\":\"frame_ms\""));

    // Duration is microseconds; the exact value depends on the scheduler, so only
    // positivity is asserted.
    const std::size_t duration = json.find("\"dur\":");
    CHECK(duration != std::string::npos);
    if (duration != std::string::npos) {
        CHECK(std::strtod(json.c_str() + duration + 6, nullptr) > 0.0);
    }

    // The frame is mirrored as a millisecond counter, which draws the CPU curve.
    const std::size_t nameAt = json.find("\"name\":\"frame_ms\"");
    const std::size_t valueAt = json.find("\"value\":", nameAt);
    CHECK(valueAt != std::string::npos);
    if (valueAt != std::string::npos) {
        CHECK(std::strtod(json.c_str() + valueAt + 8, nullptr) > 0.0);
    }
}

void test_instant_marker() {
    profiler().reset();
    const ZoneId zone = profiler().register_zone("asset-loaded");
    profiler().record_instant(zone);
    const std::string json = profiler().chrome_trace_json();
    CHECK(contains(json, "\"ph\":\"i\""));
    CHECK(contains(json, "\"name\":\"asset-loaded\""));
}

void test_multithreaded_recording() {
    profiler().reset();
    const ZoneId zone = profiler().register_zone("thread work");
    constexpr int kThreads = 4;
    constexpr int kPerThread = 200;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([zone] {
            for (int i = 0; i < kPerThread; ++i) {
                shinkou::profile::Scope scope(zone);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    const auto stats = profiler().stats();
    CHECK(stats.recorded == static_cast<std::uint64_t>(kThreads) * kPerThread);
    CHECK(stats.threads >= kThreads + 1); // worker contexts plus this thread
    const std::string json = profiler().chrome_trace_json();
    CHECK(count_of(json, "\"ph\":\"X\"") == static_cast<std::size_t>(kThreads) * kPerThread);
}

void test_overflow_drops_and_counts() {
    profiler().reset();
    const ZoneId zone = profiler().register_zone("flood");
    // The per-thread ring holds 4096 samples; push well past it without draining.
    for (int i = 0; i < 6000; ++i) {
        shinkou::profile::Scope scope(zone);
    }
    const auto stats = profiler().stats();
    CHECK(stats.recorded <= 4096);
    CHECK(stats.dropped > 0);
    CHECK(stats.recorded + stats.dropped == 6000);
}

void test_disabled_records_nothing() {
    profiler().reset();
    const ZoneId zone = profiler().register_zone("while disabled");
    profiler().set_enabled(false);
    {
        shinkou::profile::Scope scope(zone);
    }
    shinkou::profile::counter("during-off", 1.0);
    profiler().set_enabled(true);
    const auto stats = profiler().stats();
    CHECK(stats.recorded == 0);
    CHECK(stats.enabled);
}

void test_macros() {
    profiler().reset();
    {
        SHINKOU_PROFILE_SCOPE("macro span");
        spin_for_ns(50000);
        SHINKOU_PROFILE_COUNTER("macro counter", 7.0);
        SHINKOU_PROFILE_INSTANT("macro instant");
    }
    const auto stats = profiler().stats();
    CHECK(stats.recorded == 3);
    const std::string json = profiler().chrome_trace_json();
    CHECK(contains(json, "\"name\":\"macro span\""));
    CHECK(contains(json, "\"name\":\"macro counter\""));
    CHECK(contains(json, "\"name\":\"macro instant\""));
}

void test_thread_names_appear_in_the_trace() {
    profiler().reset();
    std::thread worker([] {
        profiler().name_thread("worker-7");
        const ZoneId zone = profiler().register_zone("inside");
        shinkou::profile::Scope scope(zone);
    });
    worker.join();
    const std::string json = profiler().chrome_trace_json();
    CHECK(contains(json, "\"name\":\"worker-7\""));
    CHECK(contains(json, "\"name\":\"thread_name\""));
}

void test_export_is_well_formed() {
    profiler().reset();
    SHINKOU_PROFILE_SCOPE("json check");
    shinkou::profile::counter("counter \"quoted\"", 3.5);
    const std::string json = profiler().chrome_trace_json();
    CHECK(json.rfind("{\"displayTimeUnit\"", 0) == 0);
    CHECK(contains(json, "\"traceEvents\":["));
    CHECK(balanced(json));
    // Quotes inside a name must be escaped, not left raw.
    CHECK(contains(json, "counter \\\"quoted\\\""));
}

void test_write_trace_file() {
    profiler().reset();
    SHINKOU_PROFILE_SCOPE("file check");
    const auto path = std::filesystem::temp_directory_path() / "shinkou_profile_test.json";
    CHECK(profiler().write_chrome_trace(path));
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    CHECK(file != nullptr);
    if (file != nullptr) {
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::fclose(file);
        CHECK(size > 32);
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_reset_clears_everything() {
    profiler().reset();
    const ZoneId zone = profiler().register_zone("before reset");
    {
        shinkou::profile::Scope scope(zone);
    }
    profiler().begin_frame();
    profiler().end_frame();
    profiler().reset();
    const auto stats = profiler().stats();
    CHECK(stats.recorded == 0);
    CHECK(stats.frames == 0);
    CHECK(stats.truncated == false);
    // Zone identity survives a reset: the macros cache ids in statics.
    CHECK(zone != shinkou::profile::kInvalidZone);
    {
        shinkou::profile::Scope scope(zone);
    }
    CHECK(profiler().stats().recorded == 1);
}

struct TestCase {
    const char* name;
    void (*run)();
};

// `--demo [path]`: writes a small but realistic trace so the viewer can be
// checked without wiring the profiler into the engine first.
int run_demo(const std::string& path) {
    profiler().reset();
    profiler().set_enabled(true);

    const auto worker = [](int lanes) {
        profiler().name_thread("worker");
        for (int i = 0; i < lanes; ++i) {
            SHINKOU_PROFILE_SCOPE("job chunk");
            spin_for_ns(300000);
        }
    };

    for (int frame = 0; frame < 4; ++frame) {
        profiler().begin_frame();
        {
            SHINKOU_PROFILE_SCOPE("tick");
            {
                SHINKOU_PROFILE_SCOPE("cull");
                spin_for_ns(400000);
                SHINKOU_PROFILE_COUNTER("entities", 512 + frame * 13);
            }
            {
                SHINKOU_PROFILE_SCOPE("render");
                spin_for_ns(1500000);
                SHINKOU_PROFILE_COUNTER("draw calls", 220 + frame * 7);
            }
            {
                SHINKOU_PROFILE_SCOPE("physics");
                spin_for_ns(700000);
                SHINKOU_PROFILE_COUNTER("bodies", 96);
            }
        }
        profiler().end_frame();
    }

    // A second thread, so the timeline shows more than one lane.
    std::thread threads[2];
    for (auto& thread : threads) thread = std::thread(worker, 6);
    for (auto& thread : threads) thread.join();

    profiler().begin_frame();
    profiler().end_frame();

    if (!profiler().write_chrome_trace(path)) {
        std::fprintf(stderr, "could not write %s\n", path.c_str());
        return EXIT_FAILURE;
    }
    std::printf("wrote %s\n%s", path.c_str(), profiler().chrome_trace_json().substr(0, 240).c_str());
    std::printf("...\n");
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--demo") == 0) {
        const std::string path = argc >= 3 ? argv[2] : "profile-demo.trace.json";
        return run_demo(path);
    }
    const TestCase tests[] = {
        {"zone_registration", test_zone_registration},
        {"scope_records_a_span", test_scope_records_a_span},
        {"nested_scopes", test_nested_scopes_record_parent_and_child},
        {"counter_records_value", test_counter_records_value},
        {"frame_records_duration_and_curve", test_frame_records_duration_and_curve},
        {"instant_marker", test_instant_marker},
        {"multithreaded_recording", test_multithreaded_recording},
        {"overflow_drops_and_counts", test_overflow_drops_and_counts},
        {"disabled_records_nothing", test_disabled_records_nothing},
        {"macros", test_macros},
        {"thread_names_in_trace", test_thread_names_appear_in_the_trace},
        {"export_is_well_formed", test_export_is_well_formed},
        {"write_trace_file", test_write_trace_file},
        {"reset_clears_everything", test_reset_clears_everything},
    };

    for (const TestCase& test : tests) {
        std::printf("[ RUN      ] %s\n", test.name);
        const int before = g_failures;
        test.run();
        std::printf("[ %s ] %s\n", g_failures == before ? "    OK  " : "  FAILED", test.name);
        std::fflush(stdout);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
