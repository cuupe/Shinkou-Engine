#pragma once

// Shinkou Engine - CPU profiling / tracing.
//
// What this is: a marker system that turns "where did this frame go?" into a
// timeline you can open in chrome://tracing or Perfetto. It is the C++ analogue
// of Unity's Profiler CPU module, minus the GUI: markers are recorded in-process
// and exported to a trace file.
//
// Design notes (engine/docs/profiling.md):
//   * Three shapes of data, one pipeline: spans (duration), counters (a value at
//     an instant) and instants (a marker with no duration).
//   * Every thread writes into its own single-producer ring buffer. No locks, no
//     allocation, no syscall on the recording path.
//   * The frame owner drains the rings into per-thread archives. Export is a text
//     dump of those archives, so slow disk never stalls a worker.
//   * Compile-time switch: define SHINKOU_PROFILE=0 to remove every marker.
//
// Contracts:
//   * Zone ids from register_zone() stay valid for the process lifetime.
//   * Recording is safe from any thread, at any time, including while an export
//     is running (export only reads what was already drained).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#ifndef SHINKOU_PROFILE
#define SHINKOU_PROFILE 1
#endif

namespace shinkou::profile {

using ZoneId = std::uint32_t;
inline constexpr ZoneId kInvalidZone = 0;

enum class RecordType : std::uint8_t {
    Span = 0,    // duration event
    Counter = 1, // point sample
    Instant = 2, // marker
    Frame = 3,   // one whole frame; also emitted as a counter
};

// One recorded sample. Deliberately trivial and fixed size: the ring stores
// records by value and the exporter walks them without per-record allocation.
struct Record {
    std::uint64_t startNs{0};
    std::uint64_t durationNs{0};
    double value{0.0};
    ZoneId zone{kInvalidZone};
    std::uint16_t thread{0};
    RecordType type{RecordType::Span};
    std::uint8_t flags{0};
};

struct Stats {
    std::uint64_t recorded{0};
    std::uint64_t dropped{0};
    std::uint64_t frames{0};
    std::size_t threads{0};
    std::size_t zones{0};
    bool enabled{false};
    bool truncated{false};
};

namespace detail {

// Flipped by Profiler::set_enabled. Read with a relaxed load on every marker so
// a shipped build can turn tracing off without recompiling.
extern std::atomic<bool> g_enabled;

inline bool profiling_enabled() noexcept {
    return g_enabled.load(std::memory_order_relaxed);
}

inline std::uint64_t clock_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

} // namespace detail

class Profiler final {
  public:
    static Profiler &instance();

    Profiler(const Profiler &) = delete;
    Profiler &operator=(const Profiler &) = delete;

    // Interns a name. Safe to call concurrently; a repeated name returns the same
    // id, which is what lets the macros cache the id in a function-local static.
    ZoneId register_zone(std::string_view name, std::string_view category = {});

    std::string zone_name(ZoneId id) const;
    std::string zone_category(ZoneId id) const;

    void set_enabled(bool enabled) noexcept;
    bool enabled() const noexcept;

    // Names the calling thread in the trace (e.g. "job worker 3").
    void name_thread(std::string_view name);

    // Frame ownership. begin_frame/end_frame belong on the thread that drains
    // (usually the main loop). end_frame drains every ring and records the frame
    // itself, which is what draws the CPU-usage curve.
    void begin_frame();
    void end_frame();
    std::uint64_t frame_index() const noexcept;

    // Moves recorded events into the archives. Safe to call at any time; the
    // frame path already does it.
    void drain();

    // chrome://tracing / Perfetto. The string form exists for tests and for
    // shipping a trace somewhere other than a file.
    std::string chrome_trace_json();
    bool write_chrome_trace(const std::filesystem::path &path);

    Stats stats();

    // Drops everything recorded so far (archives, rings, frame counter) while
    // keeping zone and thread identities valid. Intended for tests and CI.
    void reset();

    // ----------------------------------------------------------------
    // Recording path. Internal: use the macros below. Public only because the
    // macros and Scope need to reach it without friending half the namespace.
    // ----------------------------------------------------------------
    void record_span(ZoneId zone, std::uint64_t startNs, std::uint64_t durationNs) noexcept;
    void record_counter(ZoneId zone, double value) noexcept;
    void record_instant(ZoneId zone) noexcept;

  private:
    Profiler();
    ~Profiler();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

inline Profiler &profiler() {
    return Profiler::instance();
}

inline ZoneId register_zone(std::string_view name, std::string_view category = {}) {
    return Profiler::instance().register_zone(name, category);
}

// Counter with a name that is interned on the call: meant for once-per-frame
// samples (draw calls, entities, queued jobs), not for per-item loops.
inline void counter(std::string_view name, double value) {
    if (!detail::profiling_enabled())
        return;
    Profiler &p = Profiler::instance();
    p.record_counter(p.register_zone(name), value);
}

// RAII span. Holds no reference to the profiler, so it is harmless to destroy one
// during static teardown.
class Scope final {
  public:
    explicit Scope(ZoneId zone) noexcept {
        if (zone == kInvalidZone || !detail::profiling_enabled())
            return;
        zone_ = zone;
        startNs_ = detail::clock_ns();
    }

    ~Scope() {
        if (zone_ == kInvalidZone || !detail::profiling_enabled())
            return;
        Profiler::instance().record_span(zone_, startNs_, detail::clock_ns() - startNs_);
    }

    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
    Scope(Scope &&) = delete;
    Scope &operator=(Scope &&) = delete;

  private:
    ZoneId zone_{kInvalidZone};
    std::uint64_t startNs_{0};
};

} // namespace shinkou::profile

// --------------------------------------------------------------------------
// Markers
// --------------------------------------------------------------------------

#define SHINKOU_PROFILE_CONCAT_INNER(a, b) a##b
#define SHINKOU_PROFILE_CONCAT(a, b) SHINKOU_PROFILE_CONCAT_INNER(a, b)

#if SHINKOU_PROFILE

// One span per statement block. The static caches the interned id, so only the
// first execution of this line pays for the name lookup.
#define SHINKOU_PROFILE_SCOPE(name)                                                                \
    static const ::shinkou::profile::ZoneId SHINKOU_PROFILE_CONCAT(shinkou_zone_, __LINE__) =      \
        ::shinkou::profile::register_zone(name);                                                   \
    ::shinkou::profile::Scope SHINKOU_PROFILE_CONCAT(shinkou_scope_, __LINE__)(                    \
        SHINKOU_PROFILE_CONCAT(shinkou_zone_, __LINE__))

#define SHINKOU_PROFILE_COUNTER(name, value)                                                       \
    do {                                                                                           \
        static const ::shinkou::profile::ZoneId SHINKOU_PROFILE_CONCAT(                            \
            shinkou_counter_, __LINE__) = ::shinkou::profile::register_zone(name);                 \
        ::shinkou::profile::Profiler::instance().record_counter(                                   \
            SHINKOU_PROFILE_CONCAT(shinkou_counter_, __LINE__), (value));                          \
    } while (false)

#define SHINKOU_PROFILE_INSTANT(name)                                                              \
    do {                                                                                           \
        static const ::shinkou::profile::ZoneId SHINKOU_PROFILE_CONCAT(                            \
            shinkou_instant_, __LINE__) = ::shinkou::profile::register_zone(name);                 \
        ::shinkou::profile::Profiler::instance().record_instant(                                   \
            SHINKOU_PROFILE_CONCAT(shinkou_instant_, __LINE__));                                   \
    } while (false)

#else

#define SHINKOU_PROFILE_SCOPE(name) ((void)0)
#define SHINKOU_PROFILE_COUNTER(name, value) ((void)0)
#define SHINKOU_PROFILE_INSTANT(name) ((void)0)

#endif
