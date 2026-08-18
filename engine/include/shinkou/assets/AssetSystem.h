#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace shinkou::assets {

using AssetId = std::uint64_t;
using AssetEventId = std::uint64_t;

enum class AssetState : std::uint8_t {
    Unloaded,
    Queued,
    Loading,
    Ready,
    Failed,
    Stale
};

enum class AssetEventType : std::uint8_t {
    Loaded,
    Reloaded,
    Failed,
    Invalidated,
    Evicted
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
    const AssetArtifact& artifact;
};

class IAssetProcessor {
public:
    virtual ~IAssetProcessor() = default;
    virtual bool process(const AssetProcessContext& context, AssetArtifact& output,
                         std::string& error) const = 0;
    virtual std::string version() const { return "1"; }
};

class IAssetLoader {
public:
    virtual ~IAssetLoader() = default;
    virtual bool load(const AssetLoadContext& context, AssetData& output,
                      std::string& error) const = 0;
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
};

struct AssetLoadResult {
    AssetId id{0};
    AssetState state{AssetState::Failed};
    std::shared_ptr<const AssetData> data;
    std::string error;
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
};

struct AssetStats {
    std::size_t records{0};
    std::size_t loading{0};
    std::size_t ready{0};
    std::size_t failed{0};
    std::size_t memoryBytes{0};
    std::size_t cacheHits{0};
    std::size_t cacheMisses{0};
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
    struct Job;

    AssetSystemConfig config_;
    std::vector<AssetMount> mounts_;
    std::unordered_map<AssetKey, std::shared_ptr<Record>, AssetKeyHash> records_;
    std::unordered_map<std::string, std::unique_ptr<IAssetProcessor>> processors_;
    std::unordered_map<std::string, std::unique_ptr<IAssetLoader>> loaders_;
    std::unordered_map<std::string, std::string> extensionTypes_;
    std::vector<std::thread> workers_;
    std::vector<std::function<void(const AssetSnapshot&, AssetEventType)>> listeners_;
    mutable std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::vector<Job> jobs_;
    bool stopping_{false};
    bool initialized_{false};
    std::uint64_t accessCounter_{0};
    AssetEventId nextListenerId_{1};
    std::unordered_map<AssetEventId, std::function<void(const AssetSnapshot&, AssetEventType)>> listenerMap_;
    AssetStats stats_{};

    void worker_loop();
    void execute(const std::shared_ptr<Record>& record, std::shared_ptr<std::promise<AssetLoadResult>> promise);
    AssetLoadResult make_ready_result(const std::shared_ptr<Record>& record) const;
    std::filesystem::path resolve_uri(std::string_view uri) const;
    static std::string normalize_uri(std::string_view uri);
    static std::string extension_of(std::string_view uri);
    static AssetId make_id(const AssetKey& key) noexcept;
    void notify(const std::shared_ptr<Record>& record, AssetEventType event);

public:
    explicit AssetSystem(AssetSystemConfig config = {});
    ~AssetSystem();
    AssetSystem(const AssetSystem&) = delete;
    AssetSystem& operator=(const AssetSystem&) = delete;

    bool initialize();
    void shutdown();
    bool initialized() const noexcept { return initialized_; }

    void add_mount(std::string virtualRoot, std::filesystem::path physicalRoot, bool readOnly = true);
    void clear_mounts();
    const std::vector<AssetMount>& mounts() const noexcept { return mounts_; }

    void register_processor(std::string type, std::unique_ptr<IAssetProcessor> processor);
    void register_loader(std::string type, std::unique_ptr<IAssetLoader> loader);
    void register_extension(std::string extension, std::string type);

    AssetFuture request(AssetKey key);
    AssetLoadResult load(AssetKey key);
    bool invalidate(const AssetKey& key);
    void poll();
    void trim();
    void pin(const AssetKey& key);
    void unpin(const AssetKey& key);

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
