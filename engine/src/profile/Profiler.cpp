#include "shinkou/data/Json.h"
#include "shinkou/profile/Profiler.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace shinkou::profile {

namespace {

// How many samples one thread can buffer before the frame drains it. Sized so a
// worker can record a dense burst between two frames without dropping anything.
constexpr std::size_t kDefaultRecordsPerThread = 4096;

// Cap on retained samples. At 32 bytes per record this is ~64 MB, which is a lot
// of timeline but still bounded; a session that needs more should export and
// reset.
constexpr std::size_t kDefaultMaxRetained = 2u * 1024u * 1024u;

// Single-producer / single-consumer ring. The owning thread pushes, the draining
// thread pops; nobody else touches either index.
class Ring final {
  public:
    explicit Ring(std::size_t capacity) {
        std::size_t size = 1;
        while (size < capacity)
            size <<= 1;
        slots_.resize(size);
        mask_ = size - 1;
    }

    Ring(const Ring &) = delete;
    Ring &operator=(const Ring &) = delete;

    bool push(const Record &record) noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= slots_.size()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        slots_[static_cast<std::size_t>(head) & mask_] = record;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    std::size_t drain(std::vector<Record> &out) {
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        std::size_t moved = 0;
        while (tail < head) {
            out.push_back(slots_[static_cast<std::size_t>(tail) & mask_]);
            ++tail;
            ++moved;
        }
        tail_.store(tail, std::memory_order_release);
        return moved;
    }

    std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

  private:
    std::vector<Record> slots_{};
    std::size_t mask_{0};
    std::atomic<std::uint64_t> head_{0};
    std::atomic<std::uint64_t> tail_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

void append_json_string(std::string &out, std::string_view text) {
    out += data::quote_json(text);
}

// Chrome trace timestamps are microseconds and may be fractional.
void append_microseconds(std::string &out, std::uint64_t nanoseconds) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%llu.%03llu",
                  static_cast<unsigned long long>(nanoseconds / 1000),
                  static_cast<unsigned long long>(nanoseconds % 1000));
    out += buffer;
}

void append_double(std::string &out, double value) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    out += buffer;
}

} // namespace

namespace detail {

std::atomic<bool> g_enabled{true};

// One of these per thread that has ever recorded. Owned by the profiler, so a
// thread that exits leaves a valid (if no longer written) buffer behind; whatever
// it recorded is drained on the next frame.
struct ThreadContext final {
    ThreadContext(std::size_t capacity, std::uint16_t threadIndex)
        : ring(capacity), index(threadIndex) {
    }

    Ring ring;
    std::vector<Record> archive;
    std::string name;
    std::uint16_t index{0};
};

thread_local ThreadContext *tls_context = nullptr;

} // namespace detail

struct Profiler::Impl final {
    explicit Impl(std::size_t perThread) : recordsPerThread(perThread) {
    }

    detail::ThreadContext *current() noexcept;

    std::size_t recordsPerThread;
    std::size_t maxRetained{kDefaultMaxRetained};

    mutable std::mutex zonesMutex;
    std::unordered_map<std::string, ZoneId> zoneByName;
    std::vector<std::string> zoneNames;
    std::vector<std::string> zoneCategories;

    mutable std::mutex threadsMutex;
    std::vector<std::unique_ptr<detail::ThreadContext>> contexts;

    mutable std::mutex drainMutex;
    std::size_t retained{0};
    std::uint64_t frames{0};
    bool truncated{false};

    std::uint64_t frameStartNs{0};
    ZoneId frameZone{kInvalidZone};
    ZoneId frameCounterZone{kInvalidZone};

    void drain_locked();
};

detail::ThreadContext *Profiler::Impl::current() noexcept {
    if (detail::tls_context != nullptr)
        return detail::tls_context;
    try {
        std::lock_guard<std::mutex> guard(threadsMutex);
        if (detail::tls_context != nullptr)
            return detail::tls_context;
        const std::size_t index = contexts.size();
        auto context = std::make_unique<detail::ThreadContext>(
            recordsPerThread, static_cast<std::uint16_t>((std::min)(index, std::size_t{65535})));
        detail::ThreadContext *raw = context.get();
        contexts.push_back(std::move(context));
        detail::tls_context = raw;
        return raw;
    } catch (...) {
        return nullptr;
    }
}

void Profiler::Impl::drain_locked() {
    for (auto &context : contexts) {
        if (retained >= maxRetained) {
            // Keep the ring moving so it does not stay full and inflate the drop
            // counter; excess samples are discarded rather than archived.
            truncated = true;
            std::vector<Record> discard;
            context->ring.drain(discard);
            continue;
        }
        const std::size_t before = context->archive.size();
        context->ring.drain(context->archive);
        retained += context->archive.size() - before;
        while (retained > maxRetained && !context->archive.empty()) {
            context->archive.pop_back();
            --retained;
            truncated = true;
        }
    }
}

Profiler &Profiler::instance() {
    static Profiler profiler;
    return profiler;
}

Profiler::Profiler() : impl_(std::make_unique<Impl>(kDefaultRecordsPerThread)) {
    impl_->frameZone = register_zone("frame", "frame");
    impl_->frameCounterZone = register_zone("frame_ms", "frame");
}

Profiler::~Profiler() {
    // Stop every marker before the storage disappears: static destructors and
    // joining threads can still run marker code.
    detail::g_enabled.store(false, std::memory_order_release);
}

ZoneId Profiler::register_zone(std::string_view name, std::string_view category) {
    if (name.empty())
        return kInvalidZone;
    std::lock_guard<std::mutex> guard(impl_->zonesMutex);
    const std::string key(name);
    const auto found = impl_->zoneByName.find(key);
    if (found != impl_->zoneByName.end())
        return found->second;
    if (impl_->zoneNames.size() >= 0xFFFFFFFEu)
        return kInvalidZone;
    const ZoneId id = static_cast<ZoneId>(impl_->zoneNames.size() + 1);
    impl_->zoneNames.push_back(key);
    impl_->zoneCategories.emplace_back(category);
    impl_->zoneByName.emplace(key, id);
    return id;
}

std::string Profiler::zone_name(ZoneId id) const {
    std::lock_guard<std::mutex> guard(impl_->zonesMutex);
    if (id == kInvalidZone || id > impl_->zoneNames.size())
        return {};
    return impl_->zoneNames[id - 1];
}

std::string Profiler::zone_category(ZoneId id) const {
    std::lock_guard<std::mutex> guard(impl_->zonesMutex);
    if (id == kInvalidZone || id > impl_->zoneCategories.size())
        return {};
    return impl_->zoneCategories[id - 1];
}

void Profiler::set_enabled(bool enabled) noexcept {
    detail::g_enabled.store(enabled, std::memory_order_release);
}

bool Profiler::enabled() const noexcept {
    return detail::g_enabled.load(std::memory_order_relaxed);
}

void Profiler::name_thread(std::string_view name) {
    detail::ThreadContext *context = impl_->current();
    if (context == nullptr)
        return;
    std::lock_guard<std::mutex> guard(impl_->threadsMutex);
    context->name.assign(name);
}

void Profiler::begin_frame() {
    impl_->frameStartNs = detail::clock_ns();
}

void Profiler::end_frame() {
    const std::uint64_t end = detail::clock_ns();
    const std::uint64_t start = impl_->frameStartNs;
    drain();

    std::lock_guard<std::mutex> guard(impl_->drainMutex);
    detail::ThreadContext *context = impl_->current();
    if (context == nullptr)
        return;
    if (impl_->retained >= impl_->maxRetained) {
        impl_->truncated = true;
        return;
    }
    Record frame;
    frame.type = RecordType::Frame;
    frame.zone = impl_->frameZone;
    frame.thread = context->index;
    frame.startNs = start;
    frame.durationNs = end > start ? end - start : 0;
    frame.value = static_cast<double>(frame.durationNs) / 1.0e6; // milliseconds
    context->archive.push_back(frame);
    ++impl_->retained;
    ++impl_->frames;
}

std::uint64_t Profiler::frame_index() const noexcept {
    return impl_->frames;
}

void Profiler::drain() {
    std::lock_guard<std::mutex> guard(impl_->drainMutex);
    impl_->drain_locked();
}

Stats Profiler::stats() {
    drain();
    Stats result;
    result.enabled = enabled();
    result.frames = impl_->frames;
    result.truncated = impl_->truncated;
    {
        std::lock_guard<std::mutex> guard(impl_->zonesMutex);
        result.zones = impl_->zoneNames.size();
    }
    {
        std::lock_guard<std::mutex> guard(impl_->threadsMutex);
        result.threads = impl_->contexts.size();
    }
    std::lock_guard<std::mutex> guard(impl_->drainMutex);
    for (const auto &context : impl_->contexts) {
        result.recorded += context->archive.size();
        result.dropped += context->ring.dropped();
    }
    return result;
}

std::string Profiler::chrome_trace_json() {
    drain();

    // Snapshot the names once and drop the zone lock before walking the archive;
    // a trace can hold millions of records and per-record locking would show up
    // as the dominant cost of exporting.
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> guard(impl_->zonesMutex);
        names = impl_->zoneNames;
    }

    std::string out;
    out.reserve(impl_->retained * 96 + 512);

    std::lock_guard<std::mutex> guard(impl_->drainMutex);

    out += "{\"displayTimeUnit\":\"ns\",\"traceEvents\":[";
    out += "{\"ph\":\"M\",\"name\":\"process_name\",\"pid\":1,\"args\":{\"name\":";
    append_json_string(out, "Shinkou");
    out += "}}";

    for (const auto &context : impl_->contexts) {
        out += ",{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":1,\"tid\":";
        out += std::to_string(context->index);
        out += ",\"args\":{\"name\":";
        if (!context->name.empty()) {
            append_json_string(out, context->name);
        } else {
            append_json_string(out, "thread " + std::to_string(context->index));
        }
        out += "}}";
    }

    const auto zone_of = [&names](ZoneId id) -> std::string_view {
        if (id == kInvalidZone || id > names.size())
            return "<invalid zone>";
        return names[id - 1];
    };

    for (const auto &context : impl_->contexts) {
        const std::string thread = std::to_string(context->index);
        for (const Record &record : context->archive) {
            const std::string_view name = zone_of(record.zone);
            out += ',';
            switch (record.type) {
            case RecordType::Span:
            case RecordType::Frame: {
                out += "{\"ph\":\"X\",\"pid\":1,\"tid\":";
                out += thread;
                out += ",\"ts\":";
                append_microseconds(out, record.startNs);
                out += ",\"dur\":";
                append_microseconds(out, record.durationNs);
                out += ",\"name\":";
                append_json_string(out, name);
                out += '}';
                if (record.type == RecordType::Frame) {
                    // A frame is also a counter: that is what draws the CPU-usage
                    // curve in the trace viewer.
                    out += ",{\"ph\":\"C\",\"pid\":1,\"tid\":";
                    out += thread;
                    out += ",\"ts\":";
                    append_microseconds(out, record.startNs);
                    out += ",\"name\":\"frame_ms\",\"args\":{\"value\":";
                    append_double(out, record.value);
                    out += "}}";
                }
                break;
            }
            case RecordType::Counter: {
                out += "{\"ph\":\"C\",\"pid\":1,\"tid\":";
                out += thread;
                out += ",\"ts\":";
                append_microseconds(out, record.startNs);
                out += ",\"name\":";
                append_json_string(out, name);
                out += ",\"args\":{\"value\":";
                append_double(out, record.value);
                out += "}}";
                break;
            }
            case RecordType::Instant: {
                out += "{\"ph\":\"i\",\"s\":\"t\",\"pid\":1,\"tid\":";
                out += thread;
                out += ",\"ts\":";
                append_microseconds(out, record.startNs);
                out += ",\"name\":";
                append_json_string(out, name);
                out += '}';
                break;
            }
            }
        }
    }

    out += "]}\n";
    return out;
}

bool Profiler::write_chrome_trace(const std::filesystem::path &path) {
    const std::string json = chrome_trace_json();
    std::FILE *file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr)
        return false;
    const std::size_t written = std::fwrite(json.data(), 1, json.size(), file);
    const bool flushed = std::fclose(file) == 0;
    return flushed && written == json.size();
}

void Profiler::reset() {
    std::lock_guard<std::mutex> drainGuard(impl_->drainMutex);
    std::lock_guard<std::mutex> threadsGuard(impl_->threadsMutex);
    for (auto &context : impl_->contexts) {
        context->archive.clear();
        std::vector<Record> discard;
        context->ring.drain(discard);
    }
    impl_->retained = 0;
    impl_->frames = 0;
    impl_->truncated = false;
    impl_->frameStartNs = 0;
}

// --------------------------------------------------------------------------
// Recording path
// --------------------------------------------------------------------------

void Profiler::record_span(ZoneId zone, std::uint64_t startNs, std::uint64_t durationNs) noexcept {
    if (!detail::g_enabled.load(std::memory_order_relaxed))
        return;
    detail::ThreadContext *context = impl_->current();
    if (context == nullptr)
        return;
    Record record;
    record.type = RecordType::Span;
    record.zone = zone;
    record.thread = context->index;
    record.startNs = startNs;
    record.durationNs = durationNs;
    context->ring.push(record);
}

void Profiler::record_counter(ZoneId zone, double value) noexcept {
    if (!detail::g_enabled.load(std::memory_order_relaxed))
        return;
    detail::ThreadContext *context = impl_->current();
    if (context == nullptr)
        return;
    Record record;
    record.type = RecordType::Counter;
    record.zone = zone;
    record.thread = context->index;
    record.startNs = detail::clock_ns();
    record.value = value;
    context->ring.push(record);
}

void Profiler::record_instant(ZoneId zone) noexcept {
    if (!detail::g_enabled.load(std::memory_order_relaxed))
        return;
    detail::ThreadContext *context = impl_->current();
    if (context == nullptr)
        return;
    Record record;
    record.type = RecordType::Instant;
    record.zone = zone;
    record.thread = context->index;
    record.startNs = detail::clock_ns();
    context->ring.push(record);
}

} // namespace shinkou::profile
