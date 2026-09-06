#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace shinkou::assets {

using AssetId = std::uint64_t;
using AssetGeneration = std::uint64_t;
using AssetEventId = std::uint64_t;

enum class AssetState : std::uint8_t { Unloaded, Queued, Loading, Ready, Failed, Stale };
enum class AssetEventType : std::uint8_t {
    Loaded, Reloaded, Failed, Invalidated, Evicted, DependencyInvalidated
};

struct AssetKey {
    std::string uri;
    std::string type;
    friend bool operator==(const AssetKey& left, const AssetKey& right) noexcept {
        return left.uri == right.uri && left.type == right.type;
    }
};

struct AssetKeyHash {
    std::size_t operator()(const AssetKey& key) const noexcept;
};

struct AssetDependency {
    AssetKey key;
    bool hard{true};
};

struct AssetHandle {
    AssetId id{0};
    AssetGeneration generation{0};
    AssetKey key;
    std::uint32_t slot{std::numeric_limits<std::uint32_t>::max()};
    bool valid() const noexcept { return id != 0 && generation != 0 && !key.uri.empty(); }
};

struct AssetLoadOptions {
    bool forceReload{false};
    bool pin{false};
    std::int32_t priority{0};
};

struct AssetArtifact {
    std::vector<std::uint8_t> payload;
    std::string format;
    std::vector<AssetDependency> dependencies;
};

struct AssetData {
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    std::string format;
    std::vector<AssetDependency> dependencies;
    std::filesystem::path sourcePath;
    std::uint64_t sourceHash{0};
    AssetGeneration generation{0};
    std::size_t size() const noexcept { return bytes ? bytes->size() : 0; }
};

struct AssetProcessContext {
    AssetKey key;
    std::filesystem::path sourcePath;
    const std::vector<std::uint8_t>& sourceBytes;
};

struct AssetLoadContext {
    AssetKey key;
    std::filesystem::path sourcePath;
    AssetArtifact& artifact;
};

class IAssetProcessor {
public:
    virtual ~IAssetProcessor() = default;
    virtual bool process(const AssetProcessContext&, AssetArtifact&, std::string& error) const = 0;
    virtual std::string version() const { return "1"; }
};

class IAssetLoader {
public:
    virtual ~IAssetLoader() = default;
    virtual bool load(const AssetLoadContext&, AssetData&, std::string& error) const = 0;
    virtual std::string version() const { return "1"; }
};

struct AssetMount {
    std::string virtualRoot;
    std::filesystem::path physicalRoot;
    bool readOnly{true};
};

struct AssetSystemConfig {
    std::filesystem::path projectRoot{};
    std::filesystem::path cacheRoot{};
    std::size_t workerCount{0};
    std::size_t memoryBudgetBytes{512u * 1024u * 1024u};
    bool enableFileWatching{true};
    bool enableDiskCache{true};
    bool enableBackgroundWatcher{true};
    std::uint32_t watchIntervalMilliseconds{100};
    std::size_t eventQueueCapacity{4096};
    // Timestamp/size validation is the fast path. Enable this when assets can
    // be rewritten in place without reliable filesystem timestamps.
    bool verifyCacheByContentHash{false};
    // Zero scans all records; otherwise watcher passes advance round-robin.
    std::size_t watcherScanBatchSize{256};
};

struct AssetLoadResult {
    AssetId id{0};
    AssetState state{AssetState::Failed};
    std::shared_ptr<const AssetData> data;
    std::string error;
    AssetHandle handle{};
    explicit operator bool() const noexcept { return state == AssetState::Ready && data != nullptr; }
};
using AssetFuture = std::shared_future<AssetLoadResult>;

struct AssetSnapshot {
    AssetId id{0};
    AssetKey key;
    AssetState state{AssetState::Unloaded};
    std::size_t sizeBytes{0};
    std::size_t pinCount{0};
    std::uint64_t sourceHash{0};
    std::filesystem::path sourcePath;
    AssetGeneration generation{0};
    std::size_t dependencyCount{0};
    std::size_t dependentCount{0};
    std::string lastError;
};

struct AssetStats {
    std::size_t records{0};
    std::size_t loading{0};
    std::size_t ready{0};
    std::size_t failed{0};
    std::size_t memoryBytes{0};
    std::size_t cacheHits{0};
    std::size_t cacheMisses{0};
    std::size_t stale{0};
    std::size_t evictions{0};
    std::size_t queuedEvents{0};
};

struct AssetManifestEntry {
    AssetKey key;
    std::filesystem::path sourcePath;
    std::uint64_t sourceHash{0};
    std::uint64_t sourceTimestamp{0};
    std::uintmax_t sourceSize{0};
};

class AssetSystem {
    struct Record;
    struct Job {
        std::uint32_t slot{0};
        std::uint64_t token{0};
        std::int32_t priority{0};
        std::uint64_t serial{0};
        std::uint64_t queueRevision{0};
        std::shared_ptr<std::promise<AssetLoadResult>> promise;
    };
    struct JobCompare {
        bool operator()(const Job& left, const Job& right) const noexcept {
            if (left.priority != right.priority) return left.priority < right.priority;
            return left.serial > right.serial;
        }
    };
    struct PendingEvent {
        AssetSnapshot snapshot;
        AssetEventType type{AssetEventType::Loaded};
    };

    AssetSystemConfig config_;
    std::vector<AssetMount> mounts_;
    std::vector<Record> records_;
    std::unordered_map<AssetKey, std::uint32_t, AssetKeyHash> index_;
    std::unordered_map<std::string, std::shared_ptr<IAssetProcessor>> processors_;
    std::unordered_map<std::string, std::shared_ptr<IAssetLoader>> loaders_;
    std::unordered_map<std::string, std::string> extensionTypes_;
    std::vector<std::thread> workers_;
    std::thread watcher_;
    std::priority_queue<Job, std::vector<Job>, JobCompare> jobs_;
    std::deque<PendingEvent> events_;
    std::unordered_map<AssetEventId, std::function<void(const AssetSnapshot&, AssetEventType)>> listeners_;
    mutable std::shared_mutex mutex_;
    std::condition_variable_any workAvailable_;
    std::condition_variable_any watcherWakeup_;
    bool stopping_{false};
    std::atomic_bool initialized_{false};
    std::uint64_t accessCounter_{0};
    std::uint64_t jobSerial_{0};
    std::uint64_t traversalEpoch_{0};
    std::size_t watcherCursor_{0};
    AssetEventId nextListenerId_{1};
    std::size_t lruHead_{static_cast<std::size_t>(-1)};
    std::size_t lruTail_{static_cast<std::size_t>(-1)};
    AssetStats stats_{};

    void worker_loop();
    void watcher_loop();
    void execute(std::uint32_t slot, std::shared_ptr<std::promise<AssetLoadResult>>, std::uint64_t token);
    AssetLoadResult make_result_locked(const Record& record) const;
    AssetSnapshot snapshot_locked(const Record& record) const;
    void queue_event_locked(std::uint32_t slot, AssetEventType event);
    void invalidate_locked(std::uint32_t root, bool includeDependents,
                           std::vector<std::pair<std::uint32_t, AssetEventType>>& output);
    void update_dependency_graph_locked(std::uint32_t slot, const std::vector<AssetDependency>& dependencies);
    void transition_state_locked(Record& record, AssetState next);
    std::uint64_t next_traversal_epoch_locked();
    void touch_lru_locked(std::uint32_t slot);
    void remove_lru_locked(std::uint32_t slot);
    void add_lru_locked(std::uint32_t slot);
    std::uint32_t find_or_create_locked(const AssetKey& key);
    AssetKey canonicalize_key_locked(AssetKey key) const;
    std::filesystem::path resolve_uri(std::string_view uri) const;
    void detect_changes();
    static std::string normalize_uri(std::string_view uri);
    static std::string extension_of(std::string_view uri);
    static AssetId make_id(const AssetKey& key) noexcept;

public:
    explicit AssetSystem(AssetSystemConfig config = {});
    ~AssetSystem();
    AssetSystem(const AssetSystem&) = delete;
    AssetSystem& operator=(const AssetSystem&) = delete;

    bool initialize();
    void shutdown();
    bool initialized() const noexcept;

    void add_mount(std::string virtualRoot, std::filesystem::path physicalRoot, bool readOnly = true);
    void clear_mounts();
    std::vector<AssetMount> mounts() const;
    void register_processor(std::string type, std::unique_ptr<IAssetProcessor> processor);
    void register_loader(std::string type, std::unique_ptr<IAssetLoader> loader);
    void register_extension(std::string extension, std::string type);

    AssetFuture request(AssetKey key, AssetLoadOptions options = {});
    std::vector<AssetFuture> preload(std::vector<AssetKey> keys, AssetLoadOptions options = {});
    AssetLoadResult load(AssetKey key);
    AssetFuture reload_async(AssetKey key, bool includeDependents = true);
    AssetLoadResult reload(AssetKey key, bool includeDependents = true);
    bool invalidate(const AssetKey& key, bool includeDependents = true);
    std::size_t reload_stale();
    void poll();
    void trim();
    void pin(const AssetKey& key);
    void unpin(const AssetKey& key);
    void set_memory_budget(std::size_t bytes);

    bool make_handle(const AssetKey& key, AssetHandle& output) const;
    bool is_valid(const AssetHandle& handle) const;
    std::shared_ptr<const AssetData> get(const AssetHandle& handle) const;
    std::vector<AssetDependency> dependencies(const AssetKey& key) const;
    std::vector<AssetKey> dependents(const AssetKey& key) const;
    std::filesystem::path resolve_source(std::string_view uri) const;

    bool find(const AssetKey& key, AssetSnapshot& output) const;
    std::vector<AssetSnapshot> snapshot() const;
    AssetStats stats() const;
    std::vector<AssetManifestEntry> scan_sources() const;
    bool write_manifest(const std::filesystem::path& path) const;

    AssetEventId subscribe(std::function<void(const AssetSnapshot&, AssetEventType)> callback);
    void unsubscribe(AssetEventId id);
};

std::uint64_t hash_bytes(const std::vector<std::uint8_t>& bytes) noexcept;

} // namespace shinkou::assets
