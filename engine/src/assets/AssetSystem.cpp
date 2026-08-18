#include "shinkou/assets/AssetSystem.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace shinkou::assets {
namespace {

constexpr std::uint32_t CacheVersion = 2;
constexpr char CacheMagic[] = "SHINKOUAC1";

template <typename T>
bool write_value(std::ofstream& stream, T value) {
    stream.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return stream.good();
}

template <typename T>
bool read_value(std::ifstream& stream, T& value) {
    stream.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return stream.good();
}

bool write_string(std::ofstream& stream, std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) return false;
    const auto size = static_cast<std::uint32_t>(value.size());
    return write_value(stream, size) && (stream.write(value.data(), static_cast<std::streamsize>(value.size())), stream.good());
}

bool read_string(std::ifstream& stream, std::string& value) {
    std::uint32_t size = 0;
    if (!read_value(stream, size) || size > 64u * 1024u * 1024u) return false;
    value.resize(size);
    stream.read(value.data(), static_cast<std::streamsize>(size));
    return stream.good();
}

std::uint64_t file_timestamp(const std::filesystem::path& path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    if (error) return 0;
    return static_cast<std::uint64_t>(time.time_since_epoch().count());
}

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += character; break;
        }
    }
    return result;
}

class RawProcessor final : public IAssetProcessor {
public:
    bool process(const AssetProcessContext& context, AssetArtifact& output, std::string&) const override {
        output.payload = context.sourceBytes;
        output.format = "raw";
        return true;
    }
};

class RawLoader final : public IAssetLoader {
public:
    bool load(const AssetLoadContext& context, AssetData& output, std::string&) const override {
        output.bytes = std::make_shared<const std::vector<std::uint8_t>>(context.artifact.payload);
        output.format = context.artifact.format;
        output.dependencies = context.artifact.dependencies;
        return true;
    }
};

} // namespace

struct AssetSystem::Record {
    AssetId id{0};
    AssetKey key;
    AssetState state{AssetState::Unloaded};
    std::shared_ptr<const AssetData> data;
    AssetFuture future;
    std::size_t pinCount{0};
    std::uint64_t sourceHash{0};
    std::uint64_t sourceTimestamp{0};
    std::uintmax_t sourceSize{0};
    std::filesystem::path sourcePath;
    std::uint64_t lastUse{0};
};

struct AssetSystem::Job {
    std::shared_ptr<Record> record;
    std::shared_ptr<std::promise<AssetLoadResult>> promise;
};

std::size_t AssetKeyHash::operator()(const AssetKey& key) const noexcept {
    const auto hash_string = [](std::string_view value) {
        std::size_t hash = 1469598103934665603ull;
        for (const auto character : value) {
            hash ^= static_cast<unsigned char>(character);
            hash *= 1099511628211ull;
        }
        return hash;
    };
    return hash_string(key.uri) ^ (hash_string(key.type) + static_cast<std::size_t>(0x9e3779b9u) +
                                   (hash_string(key.uri) << 6u) + (hash_string(key.uri) >> 2u));
}

std::uint64_t hash_bytes(const std::vector<std::uint8_t>& bytes) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

AssetSystem::AssetSystem(AssetSystemConfig config) : config_(std::move(config)) {
    processors_.emplace("raw", std::make_unique<RawProcessor>());
    loaders_.emplace("raw", std::make_unique<RawLoader>());
    extensionTypes_["bin"] = "raw";
    extensionTypes_["dat"] = "raw";
}

AssetSystem::~AssetSystem() { shutdown(); }

bool AssetSystem::initialize() {
    std::lock_guard lock(mutex_);
    if (initialized_) return true;
    std::error_code error;
    if (config_.projectRoot.empty()) config_.projectRoot = std::filesystem::current_path(error);
    if (error || config_.projectRoot.empty()) return false;
    config_.projectRoot = std::filesystem::weakly_canonical(config_.projectRoot, error);
    if (error) config_.projectRoot = config_.projectRoot.lexically_normal();
    if (config_.cacheRoot.empty()) config_.cacheRoot = config_.projectRoot / ".shinkou" / "cache";
    if (mounts_.empty()) mounts_.push_back({"project", config_.projectRoot, true});
    const auto count = config_.workerCount == 0
        ? std::max<std::size_t>(1, std::thread::hardware_concurrency() == 0 ? 2 : std::thread::hardware_concurrency() - 1)
        : config_.workerCount;
    if (config_.enableDiskCache) std::filesystem::create_directories(config_.cacheRoot, error);
    stopping_ = false;
    initialized_ = true;
    workers_.reserve(count);
    for (std::size_t index = 0; index < count; ++index) workers_.emplace_back(&AssetSystem::worker_loop, this);
    return true;
}

void AssetSystem::shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (!initialized_ && workers_.empty()) return;
        stopping_ = true;
    }
    workAvailable_.notify_all();
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
    std::lock_guard lock(mutex_);
    workers_.clear();
    jobs_.clear();
    records_.clear();
    listenerMap_.clear();
    initialized_ = false;
}

void AssetSystem::add_mount(std::string virtualRoot, std::filesystem::path physicalRoot, bool readOnly) {
    virtualRoot = normalize_uri(virtualRoot);
    if (virtualRoot.find("://") != std::string::npos) virtualRoot = virtualRoot.substr(0, virtualRoot.find("://"));
    while (!virtualRoot.empty() && virtualRoot.back() == '/') virtualRoot.pop_back();
    std::lock_guard lock(mutex_);
    mounts_.push_back({std::move(virtualRoot), std::move(physicalRoot), readOnly});
}

void AssetSystem::clear_mounts() {
    std::lock_guard lock(mutex_);
    mounts_.clear();
}

void AssetSystem::register_processor(std::string type, std::unique_ptr<IAssetProcessor> processor) {
    if (!processor || type.empty()) return;
    std::lock_guard lock(mutex_);
    processors_[std::move(type)] = std::move(processor);
}

void AssetSystem::register_loader(std::string type, std::unique_ptr<IAssetLoader> loader) {
    if (!loader || type.empty()) return;
    std::lock_guard lock(mutex_);
    loaders_[std::move(type)] = std::move(loader);
}

void AssetSystem::register_extension(std::string extension, std::string type) {
    while (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension.empty() || type.empty()) return;
    std::lock_guard lock(mutex_);
    extensionTypes_[std::move(extension)] = std::move(type);
}

std::string AssetSystem::normalize_uri(std::string_view uri) {
    std::string value(uri);
    std::replace(value.begin(), value.end(), '\\', '/');
    while (value.find("//") != std::string::npos && value.find("://") == std::string::npos) {
        value.replace(value.find("//"), 2, "/");
    }
    const auto scheme = value.find("://");
    if (scheme != std::string::npos) {
        std::transform(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(scheme), value.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        std::transform(value.begin() + static_cast<std::ptrdiff_t>(scheme + 3), value.end(),
                       value.begin() + static_cast<std::ptrdiff_t>(scheme + 3),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    }
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

std::string AssetSystem::extension_of(std::string_view uri) {
    const auto query = uri.find_first_of("?#");
    const auto end = query == std::string_view::npos ? uri.size() : query;
    const auto slash = uri.rfind('/', end);
    const auto dot = uri.rfind('.', end);
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    std::string extension(uri.substr(dot + 1, end - dot - 1));
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

AssetId AssetSystem::make_id(const AssetKey& key) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto* value : {&key.uri, &key.type}) {
        for (const auto character : *value) {
            hash ^= static_cast<unsigned char>(character);
            hash *= 1099511628211ull;
        }
        hash ^= 0xffu;
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
}

std::filesystem::path AssetSystem::resolve_uri(std::string_view rawUri) const {
    const auto uri = normalize_uri(rawUri);
    const auto separator = uri.find("://");
    if (separator == std::string::npos) {
        const auto basePath = config_.projectRoot.lexically_normal();
        const auto candidate = (basePath / uri).lexically_normal();
        const auto relativeCandidate = candidate.lexically_relative(basePath).generic_string();
        if (relativeCandidate.empty() || relativeCandidate == ".." || relativeCandidate.rfind("../", 0) == 0) return {};
        return candidate;
    }
    const auto root = uri.substr(0, separator);
    auto relative = std::string(uri.substr(separator + 3));
    std::error_code error;
    for (const auto& mount : mounts_) {
        if (mount.virtualRoot != root) continue;
        const auto basePath = mount.physicalRoot.lexically_normal();
        const auto candidate = (basePath / relative).lexically_normal();
        const auto relativeCandidate = candidate.lexically_relative(basePath);
        const auto relativeText = relativeCandidate.generic_string();
        if (relativeCandidate.empty() || relativeText == ".." || relativeText.rfind("../", 0) == 0) return {};
        const auto resolved = std::filesystem::weakly_canonical(candidate, error);
        if (!error) {
            const auto canonicalBase = std::filesystem::weakly_canonical(basePath, error);
            const auto canonicalRelative = resolved.lexically_relative(canonicalBase).generic_string();
            if (!error && canonicalRelative != ".." && canonicalRelative.rfind("../", 0) != 0) return resolved;
        }
        return candidate;
    }
    return {};
}

AssetFuture AssetSystem::request(AssetKey key) {
    key.uri = normalize_uri(key.uri);
    if (key.type.empty()) {
        const auto extension = extension_of(key.uri);
        std::lock_guard lock(mutex_);
        const auto found = extensionTypes_.find(extension);
        key.type = found == extensionTypes_.end() ? "raw" : found->second;
    }
    std::shared_ptr<Record> record;
    std::shared_ptr<std::promise<AssetLoadResult>> promise;
    {
        std::lock_guard lock(mutex_);
        const auto found = records_.find(key);
        if (found != records_.end()) {
            record = found->second;
            record->lastUse = ++accessCounter_;
            if (record->state == AssetState::Ready) {
                std::promise<AssetLoadResult> immediate;
                immediate.set_value(make_ready_result(record));
                return immediate.get_future().share();
            }
            if (record->state == AssetState::Loading || record->state == AssetState::Queued) return record->future;
        } else {
            record = std::make_shared<Record>();
            record->id = make_id(key);
            record->key = key;
            records_.emplace(key, record);
            ++stats_.records;
        }
        promise = std::make_shared<std::promise<AssetLoadResult>>();
        record->future = promise->get_future().share();
        record->state = AssetState::Queued;
        jobs_.push_back({record, promise});
    }
    workAvailable_.notify_one();
    return record->future;
}

AssetLoadResult AssetSystem::load(AssetKey key) { return request(std::move(key)).get(); }

AssetLoadResult AssetSystem::make_ready_result(const std::shared_ptr<Record>& record) const {
    return {record->id, record->state, record->data, {}};
}

void AssetSystem::worker_loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            workAvailable_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
            if (stopping_ && jobs_.empty()) return;
            job = std::move(jobs_.back());
            jobs_.pop_back();
        }
        execute(job.record, std::move(job.promise));
    }
}

void AssetSystem::execute(const std::shared_ptr<Record>& record, std::shared_ptr<std::promise<AssetLoadResult>> promise) {
    AssetLoadResult result;
    result.id = record->id;
    std::string error;
    const auto sourcePath = resolve_uri(record->key.uri);
    std::ifstream source(sourcePath, std::ios::binary);
    std::vector<std::uint8_t> sourceBytes;
    if (!source) {
        error = "asset source could not be opened: " + sourcePath.string();
    } else {
        source.seekg(0, std::ios::end);
        const auto length = source.tellg();
        source.seekg(0, std::ios::beg);
        if (length < 0 || static_cast<std::uint64_t>(length) > std::numeric_limits<std::uint32_t>::max() * 256ull) {
            error = "asset source is too large: " + sourcePath.string();
        } else {
            sourceBytes.resize(static_cast<std::size_t>(length));
            source.read(reinterpret_cast<char*>(sourceBytes.data()), length);
            if (!source.good() && !source.eof()) error = "asset source read failed: " + sourcePath.string();
        }
    }

    AssetArtifact artifact;
    const auto sourceHash = error.empty() ? hash_bytes(sourceBytes) : 0;
    bool cacheHit = false;
    std::string processorVersion = "1";
    {
        std::lock_guard lock(mutex_);
        record->state = AssetState::Loading;
        const auto processor = processors_.find(record->key.type);
        if (processor != processors_.end()) processorVersion = processor->second->version();
    }
    const auto processorHash = hash_bytes(std::vector<std::uint8_t>(processorVersion.begin(), processorVersion.end()));
    const auto cachePath = config_.cacheRoot / (std::to_string(record->id) + ".wac");
    if (error.empty() && config_.enableDiskCache) {
        std::ifstream cache(cachePath, std::ios::binary);
        char magic[sizeof(CacheMagic)]{};
        std::uint32_t version = 0;
        std::uint64_t cachedSourceHash = 0;
        std::uint64_t cachedProcessorHash = 0;
        std::uint32_t dependencyCount = 0;
        std::uint64_t payloadSize = 0;
        std::string cachedFormat;
        if (cache && cache.read(magic, sizeof(CacheMagic) - 1) &&
            std::string_view(magic, sizeof(CacheMagic) - 1) == CacheMagic && read_value(cache, version) &&
            read_value(cache, cachedSourceHash) && read_value(cache, cachedProcessorHash) &&
            read_value(cache, dependencyCount) && read_value(cache, payloadSize) && version == CacheVersion &&
            cachedSourceHash == sourceHash && cachedProcessorHash == processorHash &&
            payloadSize <= std::numeric_limits<std::uint32_t>::max() * 256ull && read_string(cache, cachedFormat)) {
            artifact.dependencies.clear();
            for (std::uint32_t index = 0; index < dependencyCount; ++index) {
                AssetDependency dependency;
                std::uint8_t hard = 0;
                if (!read_string(cache, dependency.key.uri) || !read_string(cache, dependency.key.type) || !read_value(cache, hard)) {
                    artifact.dependencies.clear();
                    break;
                }
                dependency.hard = hard != 0;
                artifact.dependencies.push_back(std::move(dependency));
            }
            if (cache && artifact.dependencies.size() == dependencyCount) {
                artifact.payload.resize(static_cast<std::size_t>(payloadSize));
                cache.read(reinterpret_cast<char*>(artifact.payload.data()), static_cast<std::streamsize>(payloadSize));
                cacheHit = cache.good() || cache.eof();
                artifact.format = std::move(cachedFormat);
            }
        }
    }
    if (error.empty() && !cacheHit) {
        std::unique_ptr<IAssetProcessor>* processor = nullptr;
        {
            std::lock_guard lock(mutex_);
            const auto found = processors_.find(record->key.type);
            if (found != processors_.end()) processor = &found->second;
        }
        if (!processor || !(*processor)->process({record->key, sourcePath, sourceBytes}, artifact, error)) {
            if (error.empty()) error = "no processor registered for asset type: " + record->key.type;
        }
    }
    if (error.empty()) {
        std::unique_ptr<IAssetLoader>* loader = nullptr;
        {
            std::lock_guard lock(mutex_);
            const auto found = loaders_.find(record->key.type);
            if (found != loaders_.end()) loader = &found->second;
            else {
                const auto raw = loaders_.find("raw");
                if (raw != loaders_.end()) loader = &raw->second;
            }
        }
        AssetData loaded;
        if (!loader || !(*loader)->load({record->key, sourcePath, artifact}, loaded, error)) {
            if (error.empty()) error = "no loader registered for asset type: " + record->key.type;
        } else {
            loaded.sourcePath = sourcePath;
            loaded.sourceHash = sourceHash;
            auto data = std::make_shared<AssetData>(std::move(loaded));
            AssetEventType event = AssetEventType::Loaded;
            {
                std::lock_guard lock(mutex_);
                event = record->state == AssetState::Ready ? AssetEventType::Reloaded : AssetEventType::Loaded;
                if (record->data) stats_.memoryBytes -= record->data->size();
                record->data = std::move(data);
                record->sourcePath = sourcePath;
                record->sourceHash = sourceHash;
                record->sourceTimestamp = file_timestamp(sourcePath);
                std::error_code sizeError;
                record->sourceSize = std::filesystem::file_size(sourcePath, sizeError);
                record->state = AssetState::Ready;
                record->lastUse = ++accessCounter_;
                stats_.memoryBytes += record->data->size();
                if (cacheHit) ++stats_.cacheHits; else ++stats_.cacheMisses;
                result = make_ready_result(record);
            }
            notify(record, event);
            for (const auto& dependency : artifact.dependencies) request(dependency.key);
            if (!cacheHit && config_.enableDiskCache) {
                std::error_code cacheError;
                std::filesystem::create_directories(config_.cacheRoot, cacheError);
                std::ofstream cache(cachePath, std::ios::binary | std::ios::trunc);
                if (cache) {
                    cache.write(CacheMagic, sizeof(CacheMagic) - 1);
                    write_value(cache, CacheVersion);
                    write_value(cache, sourceHash);
                    write_value(cache, processorHash);
                    write_value(cache, static_cast<std::uint32_t>(artifact.dependencies.size()));
                    write_value(cache, static_cast<std::uint64_t>(artifact.payload.size()));
                    write_string(cache, artifact.format);
                    for (const auto& dependency : artifact.dependencies) {
                        write_string(cache, dependency.key.uri);
                        write_string(cache, dependency.key.type);
                        write_value(cache, static_cast<std::uint8_t>(dependency.hard ? 1 : 0));
                    }
                    cache.write(reinterpret_cast<const char*>(artifact.payload.data()), static_cast<std::streamsize>(artifact.payload.size()));
                }
            }
        }
    }
    if (!error.empty()) {
        {
            std::lock_guard lock(mutex_);
            record->state = AssetState::Failed;
            record->sourcePath = sourcePath;
            result.state = AssetState::Failed;
            result.error = error;
            ++stats_.failed;
        }
        notify(record, AssetEventType::Failed);
    }
    promise->set_value(result);
}

void AssetSystem::notify(const std::shared_ptr<Record>& record, AssetEventType event) {
    AssetSnapshot snapshot;
    std::vector<std::function<void(const AssetSnapshot&, AssetEventType)>> callbacks;
    {
        std::lock_guard lock(mutex_);
        snapshot = {record->id, record->key, record->state, record->data ? record->data->size() : 0,
                    record->pinCount, record->sourceHash, record->sourcePath};
        callbacks.reserve(listenerMap_.size());
        for (const auto& entry : listenerMap_) if (entry.second) callbacks.push_back(entry.second);
    }
    for (const auto& callback : callbacks) callback(snapshot, event);
}

bool AssetSystem::invalidate(const AssetKey& rawKey) {
    AssetKey key = rawKey;
    key.uri = normalize_uri(key.uri);
    std::shared_ptr<Record> record;
    {
        std::lock_guard lock(mutex_);
        const auto found = records_.find(key);
        if (found == records_.end()) return false;
        record = found->second;
        if (record->data) stats_.memoryBytes -= record->data->size();
        record->state = AssetState::Stale;
        record->data.reset();
    }
    notify(record, AssetEventType::Invalidated);
    return true;
}

void AssetSystem::poll() {
    if (!config_.enableFileWatching) return;
    std::vector<AssetKey> stale;
    {
        std::lock_guard lock(mutex_);
        for (const auto& entry : records_) {
            const auto& record = entry.second;
            if (record->state != AssetState::Ready || record->sourcePath.empty()) continue;
            const auto timestamp = file_timestamp(record->sourcePath);
            std::error_code error;
            const auto size = std::filesystem::file_size(record->sourcePath, error);
            if (timestamp != record->sourceTimestamp || error || size != record->sourceSize) stale.push_back(record->key);
        }
    }
    for (const auto& key : stale) invalidate(key);
}

void AssetSystem::trim() {
    for (;;) {
        std::shared_ptr<Record> evicted;
        {
            std::lock_guard lock(mutex_);
            if (stats_.memoryBytes <= config_.memoryBudgetBytes) return;
            auto candidate = records_.end();
            for (auto it = records_.begin(); it != records_.end(); ++it) {
                const auto& record = it->second;
                if (record->state != AssetState::Ready || record->pinCount != 0 || !record->data) continue;
                if (candidate == records_.end() || record->lastUse < candidate->second->lastUse) candidate = it;
            }
            if (candidate == records_.end()) return;
            stats_.memoryBytes -= candidate->second->data->size();
            candidate->second->data.reset();
            candidate->second->state = AssetState::Unloaded;
            evicted = candidate->second;
        }
        notify(evicted, AssetEventType::Evicted);
    }
}

void AssetSystem::pin(const AssetKey& rawKey) {
    AssetKey key = rawKey;
    key.uri = normalize_uri(key.uri);
    std::lock_guard lock(mutex_);
    const auto found = records_.find(key);
    if (found != records_.end()) ++found->second->pinCount;
}

void AssetSystem::unpin(const AssetKey& rawKey) {
    AssetKey key = rawKey;
    key.uri = normalize_uri(key.uri);
    std::lock_guard lock(mutex_);
    const auto found = records_.find(key);
    if (found != records_.end() && found->second->pinCount != 0) --found->second->pinCount;
}

bool AssetSystem::find(const AssetKey& rawKey, AssetSnapshot& output) const {
    AssetKey key = rawKey;
    key.uri = normalize_uri(key.uri);
    std::lock_guard lock(mutex_);
    const auto found = records_.find(key);
    if (found == records_.end()) return false;
    const auto& record = found->second;
    output = {record->id, record->key, record->state, record->data ? record->data->size() : 0,
              record->pinCount, record->sourceHash, record->sourcePath};
    return true;
}

std::vector<AssetSnapshot> AssetSystem::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<AssetSnapshot> result;
    result.reserve(records_.size());
    for (const auto& entry : records_) {
        const auto& record = entry.second;
        result.push_back({record->id, record->key, record->state, record->data ? record->data->size() : 0,
                          record->pinCount, record->sourceHash, record->sourcePath});
    }
    return result;
}

AssetStats AssetSystem::stats() const {
    std::lock_guard lock(mutex_);
    AssetStats result = stats_;
    result.loading = 0;
    result.ready = 0;
    for (const auto& entry : records_) {
        if (entry.second->state == AssetState::Loading || entry.second->state == AssetState::Queued) ++result.loading;
        if (entry.second->state == AssetState::Ready) ++result.ready;
    }
    return result;
}

std::vector<AssetManifestEntry> AssetSystem::scan_sources() const {
    std::vector<AssetManifestEntry> result;
    std::error_code error;
    std::lock_guard lock(mutex_);
    for (const auto& mount : mounts_) {
        if (!std::filesystem::exists(mount.physicalRoot, error)) continue;
        for (std::filesystem::recursive_directory_iterator iterator(mount.physicalRoot, error), end; iterator != end && !error; iterator.increment(error)) {
            if (error || !iterator->is_regular_file(error)) continue;
            const auto relative = std::filesystem::relative(iterator->path(), mount.physicalRoot, error).generic_string();
            if (error) continue;
            const auto extension = extension_of(relative);
            const auto type = extensionTypes_.find(extension);
            if (type == extensionTypes_.end()) continue;
            std::ifstream file(iterator->path(), std::ios::binary);
            if (!file) continue;
            file.seekg(0, std::ios::end);
            const auto length = file.tellg();
            file.seekg(0, std::ios::beg);
            if (length < 0 || length > static_cast<std::streamoff>(std::numeric_limits<std::uint32_t>::max() * 256ull)) continue;
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
            file.read(reinterpret_cast<char*>(bytes.data()), length);
            const auto uri = mount.virtualRoot + "://" + relative;
            result.push_back({{normalize_uri(uri), type->second}, iterator->path(), hash_bytes(bytes),
                              file_timestamp(iterator->path()), iterator->file_size(error)});
        }
    }
    return result;
}

bool AssetSystem::write_manifest(const std::filesystem::path& path) const {
    const auto entries = scan_sources();
    std::ofstream output(path, std::ios::trunc);
    if (!output) return false;
    output << "{\n  \"version\": 1,\n  \"assets\": [\n";
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        output << "    {\"uri\": \"" << json_escape(entry.key.uri) << "\", \"type\": \""
               << json_escape(entry.key.type) << "\", \"source\": \"" << json_escape(entry.sourcePath.generic_string())
               << "\", \"hash\": " << entry.sourceHash << ", \"timestamp\": " << entry.sourceTimestamp
               << ", \"size\": " << entry.sourceSize << "}" << (index + 1 == entries.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.good();
}

AssetEventId AssetSystem::subscribe(std::function<void(const AssetSnapshot&, AssetEventType)> callback) {
    if (!callback) return 0;
    std::lock_guard lock(mutex_);
    const auto id = nextListenerId_++;
    listenerMap_.emplace(id, std::move(callback));
    return id;
}

void AssetSystem::unsubscribe(AssetEventId id) {
    std::lock_guard lock(mutex_);
    listenerMap_.erase(id);
}

} // namespace shinkou::assets
