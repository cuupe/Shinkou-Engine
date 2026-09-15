#include "shinkou/assets/AssetSystem.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_set>

namespace shinkou::assets {
namespace {

constexpr std::uint32_t CacheVersion = 6;
constexpr char CacheMagic[] = "SHINKOUAC1";
constexpr std::uint64_t MaxAssetBytes = std::numeric_limits<std::uint32_t>::max() * 256ull;
constexpr std::size_t MaxAssetMetadataBytes = 1u * 1024u * 1024u;
constexpr std::size_t InvalidSlot = static_cast<std::size_t>(-1);

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
    return write_value(stream, size) &&
           (stream.write(value.data(), static_cast<std::streamsize>(value.size())), stream.good());
}

bool read_string(std::ifstream& stream, std::string& value) {
    std::uint32_t size = 0;
    if (!read_value(stream, size) || size > 64u * 1024u * 1024u) return false;
    value.resize(size);
    if (size != 0) stream.read(value.data(), static_cast<std::streamsize>(size));
    return stream.good();
}

std::uint64_t file_timestamp(const std::filesystem::path& path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    return error ? 0 : static_cast<std::uint64_t>(time.time_since_epoch().count());
}

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes,
               std::string* error = nullptr) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        if (error) *error = "asset source could not be opened: " + path.string();
        return false;
    }
    const auto length = file.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > MaxAssetBytes) {
        if (error) *error = "asset source is too large: " + path.string();
        return false;
    }
    bytes.resize(static_cast<std::size_t>(length));
    file.seekg(0, std::ios::beg);
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        if (error) *error = "asset source read failed: " + path.string();
        bytes.clear();
        return false;
    }
    return true;
}

struct DependencyFingerprint {
    AssetDependency dependency;
    std::uint64_t hash{0};
    std::uint64_t timestamp{0};
    std::uintmax_t size{0};
};

bool write_cache_atomically(const std::filesystem::path& path, const AssetKey& key,
                            std::uint64_t sourceHash, std::uint64_t sourceTimestamp,
                            std::uintmax_t sourceSize, std::uint64_t processorHash,
                            std::uint64_t loaderHash, const AssetArtifact& artifact,
                            const std::vector<DependencyFingerprint>& dependencies) {
    if (dependencies.size() > std::numeric_limits<std::uint32_t>::max() ||
        artifact.metadata.size() > MaxAssetMetadataBytes) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;

    static std::atomic_uint64_t temporarySerial{0};
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(temporarySerial.fetch_add(1, std::memory_order_relaxed));
    bool written = false;
    {
        std::ofstream cache(temporary, std::ios::binary | std::ios::trunc);
        if (cache) {
            cache.write(CacheMagic, sizeof(CacheMagic) - 1);
            written = write_value(cache, CacheVersion) && write_string(cache, key.uri) &&
                      write_string(cache, key.type) && write_value(cache, sourceHash) &&
                      write_value(cache, sourceTimestamp) && write_value(cache, sourceSize) &&
                      write_value(cache, processorHash) && write_value(cache, loaderHash) &&
                      write_value(cache, static_cast<std::uint32_t>(dependencies.size())) &&
                      write_value(cache, static_cast<std::uint64_t>(artifact.payload.size())) &&
                      write_string(cache, artifact.format) && write_string(cache, artifact.metadataFormat) &&
                      write_string(cache, artifact.metadata);
            for (const auto& dependency : dependencies) {
                written = written && write_string(cache, dependency.dependency.key.uri) &&
                          write_string(cache, dependency.dependency.key.type) &&
                          write_value(cache, static_cast<std::uint8_t>(dependency.dependency.hard ? 1 : 0)) &&
                          write_value(cache, dependency.hash) && write_value(cache, dependency.timestamp) &&
                          write_value(cache, dependency.size);
            }
            if (written && !artifact.payload.empty()) {
                cache.write(reinterpret_cast<const char*>(artifact.payload.data()),
                            static_cast<std::streamsize>(artifact.payload.size()));
                written = cache.good();
            }
            cache.flush();
            written = written && cache.good();
        }
    }
    if (!written) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    std::filesystem::rename(temporary, path, error);
    if (!error) return true;
    error.clear();
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error) std::filesystem::remove(temporary, error);
    return !error;
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

std::uint16_t read_u16_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) noexcept {
    if (offset + 2 > bytes.size()) return 0;
    return static_cast<std::uint16_t>(bytes[offset]) |
           (static_cast<std::uint16_t>(bytes[offset + 1]) << 8u);
}

std::uint32_t read_u32_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) noexcept {
    if (offset + 4 > bytes.size()) return 0;
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
}

std::uint16_t read_u16_be(const std::vector<std::uint8_t>& bytes, std::size_t offset) noexcept {
    if (offset + 2 > bytes.size()) return 0;
    return (static_cast<std::uint16_t>(bytes[offset]) << 8u) |
           static_cast<std::uint16_t>(bytes[offset + 1]);
}

std::uint32_t read_u32_be(const std::vector<std::uint8_t>& bytes, std::size_t offset) noexcept {
    if (offset + 4 > bytes.size()) return 0;
    return (static_cast<std::uint32_t>(bytes[offset]) << 24u) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8u) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::string source_extension(std::string_view uri) {
    const auto query = uri.find_first_of("?#");
    const auto end = query == std::string_view::npos ? uri.size() : query;
    const auto slash = uri.rfind('/', end);
    const auto dot = uri.rfind('.', end);
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    std::string extension(uri.substr(dot + 1, end - dot - 1));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

bool describe_png(const std::vector<std::uint8_t>& bytes, std::string& metadata) {
    static constexpr std::uint8_t signature[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    if (bytes.size() < 33 || !std::equal(std::begin(signature), std::end(signature), bytes.begin()) ||
        read_u32_be(bytes, 8) != 13 || bytes[12] != 'I' || bytes[13] != 'H' || bytes[14] != 'D' || bytes[15] != 'R') {
        return false;
    }
    const auto width = read_u32_be(bytes, 16);
    const auto height = read_u32_be(bytes, 20);
    if (width == 0 || height == 0) return false;
    metadata = "{\"bytes\":" + std::to_string(bytes.size()) + ",\"width\":" + std::to_string(width) +
               ",\"height\":" + std::to_string(height) + ",\"bitDepth\":" + std::to_string(bytes[24]) +
               ",\"colorType\":" + std::to_string(bytes[25]) + "}";
    return true;
}

bool describe_jpeg(const std::vector<std::uint8_t>& bytes, std::string& metadata) {
    if (bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0xd8) return false;
    std::size_t offset = 2;
    while (offset + 4 <= bytes.size()) {
        if (bytes[offset] != 0xff) { ++offset; continue; }
        while (offset < bytes.size() && bytes[offset] == 0xff) ++offset;
        if (offset >= bytes.size()) break;
        const auto marker = bytes[offset++];
        if (marker == 0xd8 || marker == 0xd9 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        const auto length = read_u16_be(bytes, offset);
        if (length < 2 || offset + length > bytes.size()) return false;
        const bool frame = (marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
                           (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf);
        if (frame && length >= 8) {
            const auto height = read_u16_be(bytes, offset + 3);
            const auto width = read_u16_be(bytes, offset + 5);
            if (width == 0 || height == 0) return false;
            metadata = "{\"bytes\":" + std::to_string(bytes.size()) + ",\"width\":" + std::to_string(width) +
                       ",\"height\":" + std::to_string(height) + ",\"components\":" +
                       std::to_string(bytes[offset + 7]) + "}";
            return true;
        }
        offset += length;
    }
    return false;
}

bool describe_wav(const std::vector<std::uint8_t>& bytes, std::string& metadata) {
    if (bytes.size() < 12 || std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) != "RIFF" ||
        std::string_view(reinterpret_cast<const char*>(bytes.data() + 8), 4) != "WAVE") return false;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t bits = 0;
    std::uint32_t dataBytes = 0;
    std::size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const auto chunkSize = read_u32_le(bytes, offset + 4);
        const auto payload = offset + 8;
        if (payload > bytes.size() || chunkSize > bytes.size() - payload) return false;
        const std::string_view chunk(reinterpret_cast<const char*>(bytes.data() + offset), 4);
        if (chunk == "fmt " && chunkSize >= 16) {
            channels = read_u16_le(bytes, payload + 2);
            sampleRate = read_u32_le(bytes, payload + 4);
            bits = read_u16_le(bytes, payload + 14);
        } else if (chunk == "data") {
            dataBytes = chunkSize;
        }
        offset = payload + chunkSize + (chunkSize & 1u);
    }
    if (channels == 0 || sampleRate == 0 || bits == 0) return false;
    metadata = "{\"bytes\":" + std::to_string(bytes.size()) + ",\"channels\":" + std::to_string(channels) +
               ",\"sampleRate\":" + std::to_string(sampleRate) + ",\"bits\":" + std::to_string(bits) +
               ",\"dataBytes\":" + std::to_string(dataBytes) + "}";
    return true;
}

bool describe_obj(const std::vector<std::uint8_t>& bytes, std::string& metadata) {
    std::size_t vertices = 0;
    std::size_t faces = 0;
    std::size_t lineStart = 0;
    // OBJ is line-oriented; only inspect a bounded prefix and count complete
    // records. The source itself remains the payload for a later importer.
    const auto limit = std::min<std::size_t>(bytes.size(), 8u * 1024u * 1024u);
    while (lineStart < limit) {
        auto lineEnd = lineStart;
        while (lineEnd < limit && bytes[lineEnd] != '\n' && bytes[lineEnd] != '\r') ++lineEnd;
        const auto length = lineEnd - lineStart;
        if (length >= 2 && bytes[lineStart] == 'v' && bytes[lineStart + 1] == ' ') ++vertices;
        if (length >= 2 && bytes[lineStart] == 'f' && bytes[lineStart + 1] == ' ') ++faces;
        while (lineEnd < limit && (bytes[lineEnd] == '\n' || bytes[lineEnd] == '\r')) ++lineEnd;
        lineStart = lineEnd;
    }
    if (vertices == 0 && faces == 0) return false;
    metadata = "{\"bytes\":" + std::to_string(bytes.size()) + ",\"vertices\":" + std::to_string(vertices) +
               ",\"faces\":" + std::to_string(faces) + "}";
    return true;
}

void describe_typed_source(const AssetProcessContext& context, std::string_view type, AssetArtifact& output) {
    output.metadataFormat = "shinkou.asset.source.v1";
    output.metadata = "{\"bytes\":" + std::to_string(context.sourceBytes.size()) + "}";
    if (type == "text") {
        std::size_t lines = context.sourceBytes.empty() ? 0 : 1;
        for (const auto byte : context.sourceBytes) if (byte == '\n') ++lines;
        output.metadataFormat = "shinkou.asset.text.v1";
        output.metadata = "{\"bytes\":" + std::to_string(context.sourceBytes.size()) +
                          ",\"lines\":" + std::to_string(lines) + "}";
    } else if (type == "texture") {
        std::string imageMetadata;
        if (describe_png(context.sourceBytes, imageMetadata) || describe_jpeg(context.sourceBytes, imageMetadata)) {
            output.metadataFormat = "shinkou.asset.texture.v1";
            output.metadata = std::move(imageMetadata);
        }
    } else if (type == "audio") {
        std::string audioMetadata;
        if (source_extension(context.key.uri) == "wav" && describe_wav(context.sourceBytes, audioMetadata)) {
            output.metadataFormat = "shinkou.asset.audio.v1";
            output.metadata = std::move(audioMetadata);
        }
    } else if (type == "model" && source_extension(context.key.uri) == "obj") {
        std::string modelMetadata;
        if (describe_obj(context.sourceBytes, modelMetadata)) {
            output.metadataFormat = "shinkou.asset.model.obj.v1";
            output.metadata = std::move(modelMetadata);
        }
    }
}

class TypedSourceProcessor final : public IAssetProcessor {
    std::string format_;

public:
    explicit TypedSourceProcessor(std::string format) : format_(std::move(format)) {}

    bool process(const AssetProcessContext& context, AssetArtifact& output, std::string&) const override {
        output.payload = context.sourceBytes;
        output.format = format_;
        describe_typed_source(context, format_, output);
        output.dependencies.clear();
        return true;
    }
};

class RawLoader final : public IAssetLoader {
public:
    bool load(const AssetLoadContext& context, AssetData& output, std::string&) const override {
        output.bytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(context.artifact.payload));
        output.format = context.artifact.format;
        output.metadataFormat = context.artifact.metadataFormat;
        output.metadata = std::make_shared<const std::string>(context.artifact.metadata);
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
    std::shared_ptr<std::promise<AssetLoadResult>> pendingPromise;
    std::size_t pinCount{0};
    std::uint64_t sourceHash{0};
    std::uint64_t sourceTimestamp{0};
    std::uintmax_t sourceSize{0};
    std::filesystem::path sourcePath;
    AssetGeneration generation{0};
    std::uint64_t loadToken{0};
    std::uint64_t lastUse{0};
    std::vector<AssetDependency> dependencies;
    std::vector<std::uint32_t> dependencySlots;
    std::vector<std::uint32_t> dependentSlots;
    std::string lastError;
    std::int32_t queuedPriority{std::numeric_limits<std::int32_t>::min()};
    std::uint64_t queueRevision{0};
    std::uint64_t visitEpoch{0};
    std::size_t lruPrevious{InvalidSlot};
    std::size_t lruNext{InvalidSlot};
    bool inLru{false};
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
    const auto uriHash = hash_string(key.uri);
    const auto typeHash = hash_string(key.type);
    return uriHash ^ (typeHash + static_cast<std::size_t>(0x9e3779b9u) + (uriHash << 6u) + (uriHash >> 2u));
}

std::uint64_t hash_bytes(const std::vector<std::uint8_t>& bytes) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t hash_string(std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}

AssetSystem::AssetSystem(AssetSystemConfig config) : config_(std::move(config)) {
    processors_.emplace("raw", std::make_shared<RawProcessor>());
    loaders_.emplace("raw", std::make_shared<RawLoader>());
    const auto register_typed_source = [this](std::string type) {
        processors_.emplace(type, std::make_shared<TypedSourceProcessor>(type));
        loaders_.emplace(type, std::make_shared<RawLoader>());
    };
    for (const auto* type : {"text", "texture", "model", "audio", "video", "font", "shader", "material", "scene"})
        register_typed_source(type);
    for (const auto* extension : {"bin", "dat"}) extensionTypes_[extension] = "raw";
    for (const auto* extension : {"txt", "json", "yaml", "yml"}) extensionTypes_[extension] = "text";
    for (const auto* extension : {"png", "jpg", "jpeg", "tga", "dds", "ktx", "ktx2"}) extensionTypes_[extension] = "texture";
    for (const auto* extension : {"obj", "gltf", "glb"}) extensionTypes_[extension] = "model";
    for (const auto* extension : {"wav", "ogg", "mp3"}) extensionTypes_[extension] = "audio";
    for (const auto* extension : {"mp4", "mov", "m4v", "avi", "mkv", "webm", "wmv"}) extensionTypes_[extension] = "video";
    for (const auto* extension : {"ttf", "otf"}) extensionTypes_[extension] = "font";
    for (const auto* extension : {"shader", "hlsl", "glsl", "vert", "frag", "comp"}) extensionTypes_[extension] = "shader";
    extensionTypes_["mat"] = "material";
    for (const auto* extension : {"scene", "prefab"}) extensionTypes_[extension] = "scene";
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
    if (config_.eventQueueCapacity == 0) config_.eventQueueCapacity = 1;
    if (config_.enableDiskCache) std::filesystem::create_directories(config_.cacheRoot, error);
    const auto hardware = std::thread::hardware_concurrency();
    const auto count = config_.workerCount == 0
        ? std::max<std::size_t>(1, hardware == 0 ? 2 : static_cast<std::size_t>(hardware - 1))
        : config_.workerCount;
    stopping_ = false;
    initialized_ = true;
    workers_.reserve(count);
    for (std::size_t index = 0; index < count; ++index) workers_.emplace_back(&AssetSystem::worker_loop, this);
    if (config_.enableFileWatching && config_.enableBackgroundWatcher) watcher_ = std::thread(&AssetSystem::watcher_loop, this);
    return true;
}

void AssetSystem::shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (!initialized_ && workers_.empty() && !watcher_.joinable()) {
            manifestReady_.store(false, std::memory_order_release);
            return;
        }
        stopping_ = true;
        for (std::uint32_t slot = 0; slot < records_.size(); ++slot) {
            auto& record = records_[slot];
            if (record.state != AssetState::Queued) continue;
            ++record.loadToken;
            transition_state_locked(record, AssetState::Failed);
            record.lastError = "asset system is shutting down";
            if (record.future.valid()) {
                // Queued work has not reached a worker, so its promise is still owned by the queue.
                // It is resolved below while draining the unique queue entries.
            }
        }
        while (!jobs_.empty()) {
            auto job = jobs_.top();
            jobs_.pop();
            if (job.slot >= records_.size()) continue;
            const auto& record = records_[job.slot];
            if (job.token + 1 != record.loadToken || job.queueRevision != record.queueRevision ||
                job.promise != record.pendingPromise) continue;
            job.promise->set_value({record.id, AssetState::Failed, {}, record.lastError,
                                    {record.id, record.generation, record.key, job.slot}});
            records_[job.slot].pendingPromise.reset();
        }
    }
    workAvailable_.notify_all();
    watcherWakeup_.notify_all();
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
    if (watcher_.joinable()) watcher_.join();
    {
        std::lock_guard lock(mutex_);
        workers_.clear();
        events_.clear();
        index_.clear();
        records_.clear();
        lruHead_ = lruTail_ = InvalidSlot;
        stats_ = {};
        initialized_ = false;
    }
    {
        std::lock_guard scanLock(manifestScanMutex_);
        manifestCache_.clear();
        manifestScanStats_ = {};
        std::atomic_store_explicit(&manifestSnapshot_,
            std::shared_ptr<const std::vector<AssetManifestEntry>>{}, std::memory_order_release);
        manifestReady_.store(false, std::memory_order_release);
    }
}

bool AssetSystem::initialized() const noexcept { return initialized_.load(std::memory_order_acquire); }

void AssetSystem::add_mount(std::string virtualRoot, std::filesystem::path physicalRoot, bool readOnly) {
    virtualRoot = normalize_uri(virtualRoot);
    if (const auto separator = virtualRoot.find("://"); separator != std::string::npos) virtualRoot.resize(separator);
    std::transform(virtualRoot.begin(), virtualRoot.end(), virtualRoot.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    while (!virtualRoot.empty() && virtualRoot.back() == '/') virtualRoot.pop_back();
    {
        std::lock_guard lock(mutex_);
        mounts_.push_back({std::move(virtualRoot), std::move(physicalRoot), readOnly});
    }
    std::lock_guard scanLock(manifestScanMutex_);
    manifestCache_.clear();
    manifestScanStats_ = {};
    std::atomic_store_explicit(&manifestSnapshot_,
        std::shared_ptr<const std::vector<AssetManifestEntry>>{}, std::memory_order_release);
    manifestReady_.store(false, std::memory_order_release);
}

void AssetSystem::clear_mounts() {
    {
        std::lock_guard lock(mutex_);
        mounts_.clear();
    }
    std::lock_guard scanLock(manifestScanMutex_);
    manifestCache_.clear();
    manifestScanStats_ = {};
    std::atomic_store_explicit(&manifestSnapshot_,
        std::shared_ptr<const std::vector<AssetManifestEntry>>{}, std::memory_order_release);
    manifestReady_.store(false, std::memory_order_release);
}

std::vector<AssetMount> AssetSystem::mounts() const {
    std::shared_lock lock(mutex_);
    return mounts_;
}

void AssetSystem::register_processor(std::string type, std::unique_ptr<IAssetProcessor> processor) {
    if (!processor || type.empty()) return;
    std::lock_guard lock(mutex_);
    processors_[std::move(type)] = std::shared_ptr<IAssetProcessor>(std::move(processor));
}

void AssetSystem::register_loader(std::string type, std::unique_ptr<IAssetLoader> loader) {
    if (!loader || type.empty()) return;
    std::lock_guard lock(mutex_);
    loaders_[std::move(type)] = std::shared_ptr<IAssetLoader>(std::move(loader));
}

void AssetSystem::register_extension(std::string extension, std::string type) {
    while (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
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
    if (const auto scheme = value.find("://"); scheme != std::string::npos) {
        std::transform(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(scheme), value.begin(),
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
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
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

AssetKey AssetSystem::canonicalize_key_locked(AssetKey key) const {
    key.uri = normalize_uri(key.uri);
    if (key.type.empty()) {
        const auto found = extensionTypes_.find(extension_of(key.uri));
        key.type = found == extensionTypes_.end() ? "raw" : found->second;
    }
    return key;
}

std::filesystem::path AssetSystem::resolve_uri(std::string_view rawUri) const {
    const auto uri = normalize_uri(rawUri);
    const auto separator = uri.find("://");
    if (separator == std::string::npos) {
        const auto base = config_.projectRoot.lexically_normal();
        const auto candidate = (base / uri).lexically_normal();
        const auto relative = candidate.lexically_relative(base).generic_string();
        if (relative.empty() || relative == ".." || relative.rfind("../", 0) == 0) return {};
        return candidate;
    }
    const auto root = uri.substr(0, separator);
    const auto relative = std::string(uri.substr(separator + 3));
    for (const auto& mount : mounts_) {
        if (mount.virtualRoot != root) continue;
        const auto base = mount.physicalRoot.lexically_normal();
        const auto candidate = (base / relative).lexically_normal();
        const auto lexicalRelative = candidate.lexically_relative(base).generic_string();
        if (lexicalRelative.empty() || lexicalRelative == ".." || lexicalRelative.rfind("../", 0) == 0) return {};
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(candidate, error);
        if (!error) {
            const auto canonicalBase = std::filesystem::weakly_canonical(base, error);
            if (!error) {
                const auto canonicalRelative = resolved.lexically_relative(canonicalBase).generic_string();
                if (canonicalRelative == ".." || canonicalRelative.rfind("../", 0) == 0) return {};
                return resolved;
            }
        }
        return candidate;
    }
    return {};
}

std::uint32_t AssetSystem::find_or_create_locked(const AssetKey& key) {
    const auto found = index_.find(key);
    if (found != index_.end()) return found->second;
    const auto slot = static_cast<std::uint32_t>(records_.size());
    Record record;
    record.id = make_id(key);
    record.key = key;
    records_.push_back(std::move(record));
    index_.emplace(records_.back().key, slot);
    ++stats_.records;
    return slot;
}

AssetFuture AssetSystem::request(AssetKey key, AssetLoadOptions options) {
    std::lock_guard lock(mutex_);
    key = canonicalize_key_locked(std::move(key));
    if (!initialized_ || stopping_) {
        std::promise<AssetLoadResult> unavailable;
        unavailable.set_value({make_id(key), AssetState::Failed, {},
                               stopping_ ? "asset system is shutting down" : "asset system is not initialized", {}});
        return unavailable.get_future().share();
    }
    const auto slot = find_or_create_locked(key);
    auto& record = records_[slot];
    if (options.pin) ++record.pinCount;
    record.lastUse = ++accessCounter_;
    if (record.state == AssetState::Ready && !options.forceReload) {
        if (record.pinCount == 0) touch_lru_locked(slot);
        else remove_lru_locked(slot);
        std::promise<AssetLoadResult> immediate;
        immediate.set_value(make_result_locked(record));
        return immediate.get_future().share();
    }
    if (record.state == AssetState::Loading) return record.future;
    if (record.state == AssetState::Queued) {
        if (options.priority > record.queuedPriority) {
            record.queuedPriority = options.priority;
            ++record.queueRevision;
            jobs_.push({slot, record.loadToken, record.queuedPriority, ++jobSerial_,
                        record.queueRevision, record.pendingPromise});
            workAvailable_.notify_one();
        }
        return record.future;
    }
    remove_lru_locked(slot);
    auto promise = std::make_shared<std::promise<AssetLoadResult>>();
    record.pendingPromise = promise;
    record.future = promise->get_future().share();
    transition_state_locked(record, AssetState::Queued);
    record.lastError.clear();
    const auto token = ++record.loadToken;
    record.queuedPriority = options.priority;
    ++record.queueRevision;
    jobs_.push({slot, token, options.priority, ++jobSerial_, record.queueRevision, promise});
    workAvailable_.notify_one();
    return record.future;
}

std::vector<AssetFuture> AssetSystem::preload(std::vector<AssetKey> keys, AssetLoadOptions options) {
    std::vector<AssetFuture> futures;
    futures.reserve(keys.size());
    for (auto& key : keys) futures.push_back(request(std::move(key), options));
    return futures;
}

AssetLoadResult AssetSystem::load(AssetKey key) { return request(std::move(key)).get(); }

AssetFuture AssetSystem::reload_async(AssetKey key, bool includeDependents) {
    std::vector<AssetKey> dependentKeys;
    {
        std::lock_guard lock(mutex_);
        key = canonicalize_key_locked(std::move(key));
        const auto root = index_.find(key);
        if (includeDependents && root != index_.end()) {
            std::vector<std::uint32_t> pending{root->second};
            const auto epoch = next_traversal_epoch_locked();
            records_[root->second].visitEpoch = epoch;
            while (!pending.empty()) {
                const auto slot = pending.back();
                pending.pop_back();
                for (const auto dependent : records_[slot].dependentSlots) {
                    if (dependent >= records_.size() || records_[dependent].visitEpoch == epoch) continue;
                    records_[dependent].visitEpoch = epoch;
                    dependentKeys.push_back(records_[dependent].key);
                    pending.push_back(dependent);
                }
            }
        }
    }
    if (includeDependents) invalidate(key, true);
    auto result = request(key, {true, false, 0});
    for (auto& dependent : dependentKeys) request(std::move(dependent), {true, false, 0});
    return result;
}

AssetLoadResult AssetSystem::reload(AssetKey key, bool includeDependents) {
    return reload_async(std::move(key), includeDependents).get();
}

AssetLoadResult AssetSystem::make_result_locked(const Record& record) const {
    const auto slot = static_cast<std::uint32_t>(&record - records_.data());
    AssetLoadResult result{record.id, record.state, record.data, record.lastError,
                           {record.id, record.generation, record.key, slot}};
    return result;
}

AssetSnapshot AssetSystem::snapshot_locked(const Record& record) const {
    return {record.id, record.key, record.state, record.data ? record.data->size() : 0, record.pinCount,
            record.sourceHash, record.sourcePath, record.generation, record.dependencies.size(),
            record.dependentSlots.size(), record.lastError};
}

void AssetSystem::queue_event_locked(std::uint32_t slot, AssetEventType event) {
    if (events_.size() >= config_.eventQueueCapacity) events_.pop_front();
    events_.push_back({snapshot_locked(records_[slot]), event});
    stats_.queuedEvents = events_.size();
}

void AssetSystem::update_dependency_graph_locked(std::uint32_t slot,
                                                 const std::vector<AssetDependency>& dependencies) {
    auto& record = records_[slot];
    for (const auto dependencySlot : record.dependencySlots) {
        if (dependencySlot >= records_.size()) continue;
        auto& list = records_[dependencySlot].dependentSlots;
        list.erase(std::remove(list.begin(), list.end(), slot), list.end());
    }
    record.dependencies = dependencies;
    record.dependencySlots.clear();
    record.dependencySlots.reserve(dependencies.size());
    for (const auto& dependency : dependencies) {
        const auto dependencySlot = find_or_create_locked(dependency.key);
        auto& dependencySlots = records_[slot].dependencySlots;
        if (std::find(dependencySlots.begin(), dependencySlots.end(), dependencySlot) !=
            dependencySlots.end()) continue;
        dependencySlots.push_back(dependencySlot);
        auto& list = records_[dependencySlot].dependentSlots;
        if (std::find(list.begin(), list.end(), slot) == list.end()) list.push_back(slot);
    }
}

void AssetSystem::transition_state_locked(Record& record, AssetState next) {
    const auto adjust = [this](AssetState state, int delta) {
        auto apply = [delta](std::size_t& value) {
            if (delta > 0) value += static_cast<std::size_t>(delta);
            else value -= static_cast<std::size_t>(-delta);
        };
        if (state == AssetState::Queued || state == AssetState::Loading) apply(stats_.loading);
        else if (state == AssetState::Ready) apply(stats_.ready);
        else if (state == AssetState::Stale) apply(stats_.stale);
    };
    if (record.state == next) return;
    adjust(record.state, -1);
    record.state = next;
    adjust(record.state, 1);
}

std::uint64_t AssetSystem::next_traversal_epoch_locked() {
    if (++traversalEpoch_ != 0) return traversalEpoch_;
    for (auto& record : records_) record.visitEpoch = 0;
    return ++traversalEpoch_;
}

void AssetSystem::remove_lru_locked(std::uint32_t slot) {
    auto& record = records_[slot];
    if (!record.inLru) return;
    if (record.lruPrevious == InvalidSlot) lruHead_ = record.lruNext;
    else records_[record.lruPrevious].lruNext = record.lruNext;
    if (record.lruNext == InvalidSlot) lruTail_ = record.lruPrevious;
    else records_[record.lruNext].lruPrevious = record.lruPrevious;
    record.lruPrevious = InvalidSlot;
    record.lruNext = InvalidSlot;
    record.inLru = false;
}

void AssetSystem::add_lru_locked(std::uint32_t slot) {
    auto& record = records_[slot];
    if (record.inLru) remove_lru_locked(slot);
    record.inLru = true;
    record.lruPrevious = InvalidSlot;
    record.lruNext = lruHead_;
    if (lruHead_ == InvalidSlot) lruTail_ = slot;
    else records_[lruHead_].lruPrevious = slot;
    lruHead_ = slot;
}

void AssetSystem::touch_lru_locked(std::uint32_t slot) {
    auto& record = records_[slot];
    if (record.state != AssetState::Ready || record.pinCount != 0 || !record.data) return;
    add_lru_locked(slot);
}

void AssetSystem::worker_loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            workAvailable_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
            if (stopping_ && jobs_.empty()) return;
            job = jobs_.top();
            jobs_.pop();
            if (job.slot >= records_.size()) continue;
            auto& record = records_[job.slot];
            if (record.state != AssetState::Queued || record.loadToken != job.token ||
                record.queueRevision != job.queueRevision || !job.promise) continue;
            transition_state_locked(record, AssetState::Loading);
        }
        execute(job.slot, std::move(job.promise), job.token);
    }
}

void AssetSystem::execute(std::uint32_t slot, std::shared_ptr<std::promise<AssetLoadResult>> promise,
                          std::uint64_t token) {
    AssetKey key;
    {
        std::lock_guard lock(mutex_);
        if (slot >= records_.size() || records_[slot].loadToken != token) {
            promise->set_value({0, AssetState::Failed, {}, "asset load superseded by a newer request", {}});
            return;
        }
        if (records_[slot].state != AssetState::Loading) {
            promise->set_value({0, AssetState::Failed, {}, "asset load is no longer active", {}});
            return;
        }
        key = records_[slot].key;
    }

    const auto sourcePath = resolve_source(key.uri);
    std::vector<std::uint8_t> sourceBytes;
    std::string error;
    std::uint64_t sourceTimestamp = 0;
    std::uintmax_t sourceSize = 0;
    if (sourcePath.empty()) {
        error = "asset URI could not be resolved: " + key.uri;
    } else {
        std::error_code sourceError;
        sourceTimestamp = file_timestamp(sourcePath);
        sourceSize = std::filesystem::file_size(sourcePath, sourceError);
        if (sourceError) error = "asset source could not be opened: " + sourcePath.string();
        else if (static_cast<std::uint64_t>(sourceSize) > MaxAssetBytes) error = "asset source is too large: " + sourcePath.string();
    }
    std::uint64_t sourceHash = 0;

    std::shared_ptr<IAssetProcessor> processor;
    std::shared_ptr<IAssetLoader> loader;
    std::string processorVersion = "1";
    std::string loaderVersion = "1";
    {
        std::lock_guard lock(mutex_);
        const auto processorIt = processors_.find(key.type);
        if (processorIt != processors_.end()) {
            processor = processorIt->second;
            processorVersion = processor->version();
        }
        const auto loaderIt = loaders_.find(key.type);
        if (loaderIt != loaders_.end()) loader = loaderIt->second;
        else if (const auto rawIt = loaders_.find("raw"); rawIt != loaders_.end()) loader = rawIt->second;
        if (loader) loaderVersion = loader->version();
    }
    const bool needCacheMetadata = config_.enableDiskCache;
    const auto processorHash = needCacheMetadata ? hash_string(processorVersion) : 0;
    const auto loaderHash = needCacheMetadata ? hash_string(loaderVersion) : 0;
    const auto cachePath = config_.cacheRoot / (std::to_string(make_id(key)) + ".wac");
    AssetArtifact artifact;
    bool cacheHit = false;
    if (error.empty() && config_.enableDiskCache) {
        std::ifstream cache(cachePath, std::ios::binary);
        char magic[sizeof(CacheMagic)]{};
        std::uint32_t version = 0;
        std::string cachedUri;
        std::string cachedType;
        std::uint64_t cachedSourceHash = 0, cachedSourceTimestamp = 0;
        std::uintmax_t cachedSourceSize = 0;
        std::uint64_t cachedProcessorHash = 0, cachedLoaderHash = 0;
        std::uint32_t dependencyCount = 0;
        std::uint64_t payloadSize = 0;
        std::string format;
        std::string metadataFormat;
        std::string metadata;
        struct CachedDependency {
            AssetDependency dependency;
            std::uint64_t hash{0};
            std::uint64_t timestamp{0};
            std::uintmax_t size{0};
        };
        std::vector<CachedDependency> dependencies;
        if (cache && cache.read(magic, sizeof(CacheMagic) - 1) &&
            std::string_view(magic, sizeof(CacheMagic) - 1) == CacheMagic && read_value(cache, version) &&
            read_string(cache, cachedUri) && read_string(cache, cachedType) &&
            cachedUri == key.uri && cachedType == key.type &&
            read_value(cache, cachedSourceHash) && read_value(cache, cachedSourceTimestamp) &&
            read_value(cache, cachedSourceSize) && read_value(cache, cachedProcessorHash) &&
            read_value(cache, cachedLoaderHash) && read_value(cache, dependencyCount) && read_value(cache, payloadSize) &&
            version == CacheVersion && cachedProcessorHash == processorHash &&
            cachedLoaderHash == loaderHash && payloadSize <= MaxAssetBytes && read_string(cache, format) &&
            read_string(cache, metadataFormat) && read_string(cache, metadata) &&
            metadata.size() <= MaxAssetMetadataBytes &&
            cachedSourceTimestamp == sourceTimestamp && cachedSourceSize == sourceSize) {
            bool sourceValid = true;
            if (config_.verifyCacheByContentHash) {
                sourceValid = read_file(sourcePath, sourceBytes) && hash_bytes(sourceBytes) == cachedSourceHash;
                if (sourceValid) sourceHash = cachedSourceHash;
            }
            dependencies.reserve(dependencyCount);
            for (std::uint32_t index = 0; index < dependencyCount; ++index) {
                CachedDependency dependency;
                std::uint8_t hard = 0;
                if (!read_string(cache, dependency.dependency.key.uri) || !read_string(cache, dependency.dependency.key.type) ||
                    !read_value(cache, hard) || !read_value(cache, dependency.hash) ||
                    !read_value(cache, dependency.timestamp) || !read_value(cache, dependency.size)) {
                    dependencies.clear();
                    break;
                }
                dependency.dependency.hard = hard != 0;
                dependencies.push_back(std::move(dependency));
            }
            bool valid = sourceValid && cache && dependencies.size() == dependencyCount;
            for (const auto& dependency : dependencies) {
                const auto dependencyPath = resolve_source(dependency.dependency.key.uri);
                std::error_code dependencyError;
                const auto currentTimestamp = file_timestamp(dependencyPath);
                const auto currentSize = std::filesystem::file_size(dependencyPath, dependencyError);
                const auto metadataMatches = !dependencyError && currentTimestamp == dependency.timestamp &&
                                             currentSize == dependency.size;
                std::uint64_t currentHash = dependency.hash;
                bool exists = !dependencyError;
                if (!metadataMatches || config_.verifyCacheByContentHash) {
                    std::vector<std::uint8_t> dependencyBytes;
                    exists = read_file(dependencyPath, dependencyBytes);
                    currentHash = exists ? hash_bytes(dependencyBytes) : 0;
                }
                if ((dependency.dependency.hard && !exists) || currentHash != dependency.hash) {
                    valid = false;
                    break;
                }
                artifact.dependencies.push_back(dependency.dependency);
            }
            if (valid) {
                artifact.payload.resize(static_cast<std::size_t>(payloadSize));
                if (payloadSize != 0) cache.read(reinterpret_cast<char*>(artifact.payload.data()), static_cast<std::streamsize>(payloadSize));
                cacheHit = cache.good() || cache.eof();
                artifact.format = std::move(format);
                artifact.metadataFormat = std::move(metadataFormat);
                artifact.metadata = std::move(metadata);
                sourceHash = cachedSourceHash;
            }
        }
    }
    if (error.empty() && !cacheHit) {
        if (!read_file(sourcePath, sourceBytes, &error)) {
            // read_file provides the actionable source error.
        } else {
            if (config_.enableDiskCache || config_.verifyCacheByContentHash) {
                sourceHash = hash_bytes(sourceBytes);
            }
        }
    }
    if (error.empty() && !cacheHit) {
        const bool rawFastPath = dynamic_cast<RawProcessor*>(processor.get()) != nullptr &&
                                 dynamic_cast<RawLoader*>(loader.get()) != nullptr;
        if (rawFastPath) {
            artifact.payload = std::move(sourceBytes);
            artifact.format = "raw";
        } else if (!processor || !processor->process({key, sourcePath, sourceBytes}, artifact, error)) {
            if (error.empty()) error = "no processor registered for asset type: " + key.type;
        }
    }
    if (error.empty()) {
        std::lock_guard lock(mutex_);
        for (auto& dependency : artifact.dependencies) dependency.key = canonicalize_key_locked(std::move(dependency.key));
    }
    std::vector<DependencyFingerprint> dependencyFingerprints;
    if (error.empty()) {
        dependencyFingerprints.reserve(artifact.dependencies.size());
        for (const auto& dependency : artifact.dependencies) {
            const auto dependencyPath = resolve_source(dependency.key.uri);
            std::error_code dependencyError;
            const auto timestamp = file_timestamp(dependencyPath);
            const auto size = std::filesystem::file_size(dependencyPath, dependencyError);
            std::vector<std::uint8_t> dependencyBytes;
            const bool needHash = config_.enableDiskCache;
            bool exists = false;
            if (needHash) {
                exists = read_file(dependencyPath, dependencyBytes);
            } else if (!dependencyError) {
                std::ifstream dependencyFile(dependencyPath, std::ios::binary);
                exists = static_cast<bool>(dependencyFile);
            }
            if (dependency.hard && !exists) {
                error = "hard asset dependency could not be loaded: " + dependency.key.uri;
                break;
            }
            dependencyFingerprints.push_back({dependency, exists && needHash ? hash_bytes(dependencyBytes) : 0,
                                              timestamp, dependencyError ? 0 : size});
        }
    }

    AssetLoadResult result;
    result.id = make_id(key);
    if (error.empty()) {
        if (!cacheHit && config_.enableDiskCache) {
            write_cache_atomically(cachePath, key, sourceHash, sourceTimestamp, sourceSize,
                                   processorHash, loaderHash, artifact, dependencyFingerprints);
        }
        AssetData loaded;
        if (!loader || !loader->load({key, sourcePath, artifact}, loaded, error)) {
            if (error.empty()) error = "no loader registered for asset type: " + key.type;
        } else {
            loaded.sourcePath = sourcePath;
            loaded.sourceHash = sourceHash;
            loaded.dependencies = artifact.dependencies;
            auto data = std::make_shared<AssetData>(std::move(loaded));
            bool committed = false;
            {
                std::lock_guard lock(mutex_);
                if (slot < records_.size() && records_[slot].loadToken == token) {
                    auto& record = records_[slot];
                    const auto event = record.data ? AssetEventType::Reloaded : AssetEventType::Loaded;
                    if (record.data) stats_.memoryBytes -= record.data->size();
                    remove_lru_locked(slot);
                    record.generation = record.generation == 0 ? 1 : record.generation + 1;
                    data->generation = record.generation;
                    record.data = std::move(data);
                    record.sourcePath = sourcePath;
                    record.sourceHash = sourceHash;
                    record.sourceTimestamp = sourceTimestamp;
                    record.sourceSize = sourceSize;
                    record.lastError.clear();
                    transition_state_locked(record, AssetState::Ready);
                    record.lastUse = ++accessCounter_;
                    stats_.memoryBytes += record.data->size();
                    if (record.pinCount == 0) add_lru_locked(slot);
                    if (cacheHit) ++stats_.cacheHits; else ++stats_.cacheMisses;
                    record.pendingPromise.reset();
                    update_dependency_graph_locked(slot, artifact.dependencies);
                    result = make_result_locked(records_[slot]);
                    queue_event_locked(slot, event);
                    committed = true;
                }
            }
            if (committed) {
                for (const auto& dependency : artifact.dependencies) request(dependency.key);
            } else {
                error = "asset load superseded by a newer request";
            }
        }
    }
    if (!error.empty()) {
        std::lock_guard lock(mutex_);
        if (slot < records_.size() && records_[slot].loadToken == token) {
            auto& record = records_[slot];
            transition_state_locked(record, record.data ? AssetState::Ready : AssetState::Failed);
            record.sourcePath = sourcePath;
            record.lastError = error;
            result.state = AssetState::Failed;
            result.data = record.data;
            result.error = error;
            result.handle = {record.id, record.generation, record.key, slot};
            ++stats_.failed;
            record.pendingPromise.reset();
            if (record.data && record.pinCount == 0) add_lru_locked(slot);
            queue_event_locked(slot, AssetEventType::Failed);
        } else {
            result.state = AssetState::Failed;
            result.error = error;
        }
    }
    promise->set_value(std::move(result));
}

void AssetSystem::invalidate_locked(std::uint32_t root, bool includeDependents,
                                    std::vector<std::pair<std::uint32_t, AssetEventType>>& output) {
    std::vector<std::uint32_t> pending{root};
    const auto epoch = next_traversal_epoch_locked();
    while (!pending.empty()) {
        const auto slot = pending.back();
        pending.pop_back();
        if (slot >= records_.size() || records_[slot].visitEpoch == epoch) continue;
        records_[slot].visitEpoch = epoch;
        auto& record = records_[slot];
        if (record.state != AssetState::Stale) {
            if (record.state == AssetState::Queued && record.pendingPromise) {
                record.pendingPromise->set_value({record.id, AssetState::Failed, {}, "asset load invalidated",
                                                  {record.id, record.generation, record.key, slot}});
                record.pendingPromise.reset();
            }
            transition_state_locked(record, AssetState::Stale);
            ++record.loadToken;
            remove_lru_locked(slot);
            output.emplace_back(slot, slot == root ? AssetEventType::Invalidated : AssetEventType::DependencyInvalidated);
        }
        if (!includeDependents) continue;
        pending.insert(pending.end(), record.dependentSlots.begin(), record.dependentSlots.end());
    }
}

bool AssetSystem::invalidate(const AssetKey& rawKey, bool includeDependents) {
    std::vector<std::pair<std::uint32_t, AssetEventType>> changed;
    {
        std::lock_guard lock(mutex_);
        const auto key = canonicalize_key_locked(rawKey);
        const auto found = index_.find(key);
        if (found == index_.end()) return false;
        invalidate_locked(found->second, includeDependents, changed);
        for (const auto& item : changed) queue_event_locked(item.first, item.second);
    }
    return !changed.empty();
}

std::size_t AssetSystem::reload_stale() {
    std::vector<AssetKey> keys;
    {
        std::lock_guard lock(mutex_);
        for (const auto& record : records_) if (record.state == AssetState::Stale) keys.push_back(record.key);
    }
    for (auto& key : keys) request(std::move(key), {true, false, 0});
    return keys.size();
}

void AssetSystem::detect_changes() {
    struct Candidate {
        std::uint32_t slot;
        std::filesystem::path path;
        std::uint64_t expectedTimestamp;
        std::uintmax_t expectedSize;
        std::uint64_t expectedHash;
    };
    std::vector<Candidate> candidates;
    const bool verifyHash = config_.verifyCacheByContentHash;
    {
        std::lock_guard lock(mutex_);
        if (records_.empty()) return;
        const auto recordCount = records_.size();
        const auto limit = config_.watcherScanBatchSize == 0
            ? recordCount : std::min(recordCount, config_.watcherScanBatchSize);
        candidates.reserve(limit);
        for (std::size_t offset = 0; offset < limit; ++offset) {
            const auto slot = static_cast<std::uint32_t>((watcherCursor_ + offset) % recordCount);
            const auto& record = records_[slot];
            if (record.state != AssetState::Ready || record.sourcePath.empty()) continue;
            candidates.push_back({slot, record.sourcePath, record.sourceTimestamp, record.sourceSize,
                                  record.sourceHash});
        }
        watcherCursor_ = (watcherCursor_ + limit) % recordCount;
    }

    struct Observation { Candidate candidate; bool changed; };
    std::vector<Observation> observations;
    observations.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        std::error_code error;
        const auto timestamp = file_timestamp(candidate.path);
        const auto size = std::filesystem::file_size(candidate.path, error);
        bool changed = error || timestamp != candidate.expectedTimestamp || size != candidate.expectedSize;
        if (!changed && verifyHash) {
            std::vector<std::uint8_t> bytes;
            changed = !read_file(candidate.path, bytes) || hash_bytes(bytes) != candidate.expectedHash;
        }
        observations.push_back({candidate, changed});
    }
    std::vector<std::pair<std::uint32_t, AssetEventType>> changed;
    {
        std::lock_guard lock(mutex_);
        for (const auto& observation : observations) {
            const auto& candidate = observation.candidate;
            if (!observation.changed || candidate.slot >= records_.size()) continue;
            const auto& record = records_[candidate.slot];
            if (record.state == AssetState::Ready && record.sourcePath == candidate.path &&
                record.sourceTimestamp == candidate.expectedTimestamp && record.sourceSize == candidate.expectedSize &&
                record.sourceHash == candidate.expectedHash) {
                invalidate_locked(candidate.slot, true, changed);
            }
        }
        for (const auto& item : changed) queue_event_locked(item.first, item.second);
    }
}

void AssetSystem::watcher_loop() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        const auto interval = std::chrono::milliseconds(std::max<std::uint32_t>(1, config_.watchIntervalMilliseconds));
        watcherWakeup_.wait_for(lock, interval, [&] { return stopping_; });
        if (stopping_) break;
        lock.unlock();
        detect_changes();
        lock.lock();
    }
}

void AssetSystem::poll() {
    if (config_.enableFileWatching && !config_.enableBackgroundWatcher) detect_changes();
    std::deque<PendingEvent> pending;
    std::unordered_map<AssetEventId, std::function<void(const AssetSnapshot&, AssetEventType)>> listeners;
    {
        std::lock_guard lock(mutex_);
        pending.swap(events_);
        stats_.queuedEvents = 0;
        listeners = listeners_;
    }
    for (const auto& event : pending) {
        for (const auto& listener : listeners) if (listener.second) listener.second(event.snapshot, event.type);
    }
}

void AssetSystem::trim() {
    std::lock_guard lock(mutex_);
    while (stats_.memoryBytes > config_.memoryBudgetBytes && lruTail_ != InvalidSlot) {
        const auto slot = static_cast<std::uint32_t>(lruTail_);
        auto& record = records_[slot];
        if (!record.data || record.pinCount != 0 || record.state != AssetState::Ready) {
            remove_lru_locked(slot);
            continue;
        }
        stats_.memoryBytes -= record.data->size();
        remove_lru_locked(slot);
        record.data.reset();
        transition_state_locked(record, AssetState::Unloaded);
        ++stats_.evictions;
        queue_event_locked(slot, AssetEventType::Evicted);
    }
}

void AssetSystem::pin(const AssetKey& rawKey) {
    std::lock_guard lock(mutex_);
    const auto key = canonicalize_key_locked(rawKey);
    const auto slot = find_or_create_locked(key);
    auto& record = records_[slot];
    ++record.pinCount;
    remove_lru_locked(slot);
}

void AssetSystem::unpin(const AssetKey& rawKey) {
    std::lock_guard lock(mutex_);
    const auto key = canonicalize_key_locked(rawKey);
    const auto found = index_.find(key);
    if (found == index_.end()) return;
    auto& record = records_[found->second];
    if (record.pinCount != 0) --record.pinCount;
    if (record.pinCount == 0) touch_lru_locked(found->second);
}

void AssetSystem::set_memory_budget(std::size_t bytes) {
    {
        std::lock_guard lock(mutex_);
        config_.memoryBudgetBytes = bytes;
    }
    trim();
}

bool AssetSystem::make_handle(const AssetKey& rawKey, AssetHandle& output) const {
    std::shared_lock lock(mutex_);
    const auto key = canonicalize_key_locked(rawKey);
    const auto found = index_.find(key);
    if (found == index_.end() || records_[found->second].generation == 0) return false;
    const auto& record = records_[found->second];
    output = {record.id, record.generation, record.key, found->second};
    return true;
}

bool AssetSystem::is_valid(const AssetHandle& handle) const {
    std::shared_lock lock(mutex_);
    if (!handle.valid()) return false;
    auto slot = handle.slot;
    if (slot >= records_.size()) {
        const auto found = index_.find(handle.key);
        if (found == index_.end()) return false;
        slot = found->second;
    }
    const auto& record = records_[slot];
    return record.id == handle.id && record.key == handle.key && record.generation == handle.generation &&
           record.state == AssetState::Ready && record.data != nullptr;
}

std::shared_ptr<const AssetData> AssetSystem::get(const AssetHandle& handle) const {
    std::shared_lock lock(mutex_);
    if (!handle.valid()) return {};
    auto slot = handle.slot;
    if (slot >= records_.size()) {
        const auto found = index_.find(handle.key);
        if (found == index_.end()) return {};
        slot = found->second;
    }
    const auto& record = records_[slot];
    if (record.id != handle.id || !(record.key == handle.key) || record.generation != handle.generation ||
        record.state != AssetState::Ready) return {};
    return record.data;
}

std::vector<AssetDependency> AssetSystem::dependencies(const AssetKey& rawKey) const {
    std::shared_lock lock(mutex_);
    const auto key = canonicalize_key_locked(rawKey);
    const auto found = index_.find(key);
    return found == index_.end() ? std::vector<AssetDependency>{} : records_[found->second].dependencies;
}

std::vector<AssetKey> AssetSystem::dependents(const AssetKey& rawKey) const {
    std::shared_lock lock(mutex_);
    const auto key = canonicalize_key_locked(rawKey);
    const auto found = index_.find(key);
    if (found == index_.end()) return {};
    std::vector<AssetKey> result;
    const auto& slots = records_[found->second].dependentSlots;
    result.reserve(slots.size());
    for (const auto slot : slots) if (slot < records_.size()) result.push_back(records_[slot].key);
    return result;
}

std::filesystem::path AssetSystem::resolve_source(std::string_view uri) const {
    std::shared_lock lock(mutex_);
    return resolve_uri(uri);
}

bool AssetSystem::find(const AssetKey& rawKey, AssetSnapshot& output) const {
    std::shared_lock lock(mutex_);
    const auto key = canonicalize_key_locked(rawKey);
    const auto found = index_.find(key);
    if (found == index_.end()) return false;
    output = snapshot_locked(records_[found->second]);
    return true;
}

std::vector<AssetSnapshot> AssetSystem::snapshot() const {
    std::shared_lock lock(mutex_);
    std::vector<AssetSnapshot> result;
    result.reserve(records_.size());
    for (const auto& record : records_) result.push_back(snapshot_locked(record));
    return result;
}

AssetStats AssetSystem::stats() const {
    std::shared_lock lock(mutex_);
    return stats_;
}

std::vector<AssetManifestEntry> AssetSystem::scan_sources() const {
    manifestReady_.store(false, std::memory_order_release);
    std::lock_guard scanLock(manifestScanMutex_);
    manifestScanStats_ = {};
    std::vector<AssetMount> mounts;
    std::unordered_map<std::string, std::string> extensions;
    {
        std::shared_lock lock(mutex_);
        mounts = mounts_;
        extensions = extensionTypes_;
    }
    std::vector<AssetManifestEntry> result;
    std::unordered_set<AssetKey, AssetKeyHash> seen;
    std::error_code error;
    for (const auto& mount : mounts) {
        if (!std::filesystem::exists(mount.physicalRoot, error)) continue;
        for (std::filesystem::recursive_directory_iterator iterator(mount.physicalRoot, error), end;
             iterator != end && !error; iterator.increment(error)) {
            if (error || !iterator->is_regular_file(error)) continue;
            const auto relative = std::filesystem::relative(iterator->path(), mount.physicalRoot, error).generic_string();
            if (error) continue;
            const auto found = extensions.find(extension_of(relative));
            if (found == extensions.end()) continue;
            const AssetKey key{normalize_uri(mount.virtualRoot + "://" + relative), found->second};
            const auto sourcePath = iterator->path().lexically_normal();
            const auto sourceTimestamp = file_timestamp(sourcePath);
            const auto sourceSize = iterator->file_size(error);
            if (error) continue;
            seen.insert(key);

            const auto cached = manifestCache_.find(key);
            if (!config_.verifyCacheByContentHash && cached != manifestCache_.end() &&
                cached->second.sourcePath.lexically_normal() == sourcePath &&
                cached->second.sourceTimestamp == sourceTimestamp &&
                cached->second.sourceSize == sourceSize) {
                result.push_back(cached->second);
                ++manifestScanStats_.cacheHits;
                continue;
            }

            std::vector<std::uint8_t> bytes;
            if (!read_file(sourcePath, bytes)) continue;
            AssetManifestEntry entry{make_id(key), key, sourcePath, hash_bytes(bytes),
                                     sourceTimestamp, sourceSize};
            manifestCache_[key] = entry;
            result.push_back(std::move(entry));
            ++manifestScanStats_.cacheMisses;
        }
    }
    for (auto cached = manifestCache_.begin(); cached != manifestCache_.end();) {
        if (seen.find(cached->first) == seen.end()) cached = manifestCache_.erase(cached);
        else ++cached;
    }
    manifestScanStats_.entries = result.size();
    std::shared_ptr<const std::vector<AssetManifestEntry>> published =
        std::make_shared<const std::vector<AssetManifestEntry>>(result);
    std::atomic_store_explicit(&manifestSnapshot_, std::move(published), std::memory_order_release);
    manifestReady_.store(true, std::memory_order_release);
    return result;
}

struct ManifestJsonValue {
    enum class Kind : std::uint8_t { Null, Boolean, Number, String, Object, Array };
    Kind kind{Kind::Null};
    bool boolean{false};
    double number{0.0};
    std::string numberText;
    std::string string;
    std::map<std::string, ManifestJsonValue, std::less<>> object;
    std::vector<ManifestJsonValue> array;
};

class ManifestJsonParser final {
    std::string_view input_;
    std::size_t position_{0};

    static constexpr std::size_t MaxDepth = 32;
    static constexpr std::size_t MaxMembers = 4096;
    static constexpr std::size_t MaxArrayItems = 65536;
    static constexpr std::size_t MaxStringBytes = 1u * 1024u * 1024u;

    void whitespace() noexcept {
        while (position_ < input_.size()) {
            const auto value = static_cast<unsigned char>(input_[position_]);
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') break;
            ++position_;
        }
    }

    bool consume(char value) noexcept {
        whitespace();
        if (position_ >= input_.size() || input_[position_] != value) return false;
        ++position_;
        return true;
    }

    static bool hex_digit(char value, std::uint32_t& output) noexcept {
        if (value >= '0' && value <= '9') { output = static_cast<std::uint32_t>(value - '0'); return true; }
        if (value >= 'a' && value <= 'f') { output = static_cast<std::uint32_t>(value - 'a' + 10); return true; }
        if (value >= 'A' && value <= 'F') { output = static_cast<std::uint32_t>(value - 'A' + 10); return true; }
        return false;
    }

    static void append_utf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7fu) output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7ffu) {
            output.push_back(static_cast<char>(0xc0u | (codepoint >> 6u)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else if (codepoint <= 0xffffu) {
            output.push_back(static_cast<char>(0xe0u | (codepoint >> 12u)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else {
            output.push_back(static_cast<char>(0xf0u | (codepoint >> 18u)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        }
    }

    bool string_value(std::string& output, std::string& error) {
        whitespace();
        if (position_ >= input_.size() || input_[position_] != '"') {
            error = "manifest JSON string is missing";
            return false;
        }
        ++position_;
        output.clear();
        while (position_ < input_.size()) {
            const auto value = static_cast<unsigned char>(input_[position_++]);
            if (value == '"') return true;
            if (value < 0x20u) { error = "manifest JSON string contains a control character"; return false; }
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
            } else {
                if (position_ >= input_.size()) { error = "manifest JSON escape is truncated"; return false; }
                const char escaped = input_[position_++];
                switch (escaped) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codepoint = 0;
                    for (int index = 0; index < 4; ++index) {
                        if (position_ >= input_.size()) { error = "manifest unicode escape is truncated"; return false; }
                        std::uint32_t digit = 0;
                        if (!hex_digit(input_[position_++], digit)) { error = "manifest unicode escape is invalid"; return false; }
                        codepoint = (codepoint << 4u) | digit;
                    }
                    if (codepoint >= 0xd800u && codepoint <= 0xdfffu) {
                        error = "manifest surrogate unicode escapes are unsupported";
                        return false;
                    }
                    append_utf8(output, codepoint);
                    break;
                }
                default: error = "manifest JSON escape is invalid"; return false;
                }
            }
            if (output.size() > MaxStringBytes) { error = "manifest JSON string exceeds the size limit"; return false; }
        }
        error = "manifest JSON string is unterminated";
        return false;
    }

    bool number_value(ManifestJsonValue& output, std::string& error) {
        whitespace();
        const auto begin = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) { error = "manifest JSON number is truncated"; return false; }
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                error = "manifest JSON number has a leading zero";
                return false;
            }
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') { error = "manifest JSON number is invalid"; return false; }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const auto fractionBegin = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            if (fractionBegin == position_) { error = "manifest JSON fraction is invalid"; return false; }
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            const auto exponentBegin = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            if (exponentBegin == position_) { error = "manifest JSON exponent is invalid"; return false; }
        }
        output.numberText = std::string(input_.substr(begin, position_ - begin));
        char* parsedEnd = nullptr;
        const auto parsed = std::strtod(output.numberText.c_str(), &parsedEnd);
        if (parsedEnd != output.numberText.c_str() + output.numberText.size() || !std::isfinite(parsed)) {
            error = "manifest JSON number is not finite";
            return false;
        }
        output.kind = ManifestJsonValue::Kind::Number;
        output.number = parsed;
        return true;
    }

    bool value(ManifestJsonValue& output, std::string& error, std::size_t depth) {
        if (depth > MaxDepth) { error = "manifest JSON nesting exceeds the limit"; return false; }
        whitespace();
        if (position_ >= input_.size()) { error = "manifest JSON value is missing"; return false; }
        switch (input_[position_]) {
        case '"': output.kind = ManifestJsonValue::Kind::String; return string_value(output.string, error);
        case '{': return object_value(output, error, depth + 1);
        case '[': return array_value(output, error, depth + 1);
        case 't': if (input_.substr(position_, 4) == "true") { position_ += 4; output.kind = ManifestJsonValue::Kind::Boolean; output.boolean = true; return true; } break;
        case 'f': if (input_.substr(position_, 5) == "false") { position_ += 5; output.kind = ManifestJsonValue::Kind::Boolean; output.boolean = false; return true; } break;
        case 'n': if (input_.substr(position_, 4) == "null") { position_ += 4; output.kind = ManifestJsonValue::Kind::Null; return true; } break;
        default: if (input_[position_] == '-' || (input_[position_] >= '0' && input_[position_] <= '9')) return number_value(output, error);
        }
        error = "manifest JSON value is invalid";
        return false;
    }

    bool object_value(ManifestJsonValue& output, std::string& error, std::size_t depth) {
        if (!consume('{')) { error = "manifest JSON object is invalid"; return false; }
        output.kind = ManifestJsonValue::Kind::Object;
        whitespace();
        if (consume('}')) return true;
        for (std::size_t count = 0; count < MaxMembers; ++count) {
            std::string key;
            if (!string_value(key, error) || !consume(':')) { if (error.empty()) error = "manifest JSON object member is invalid"; return false; }
            ManifestJsonValue child;
            if (!value(child, error, depth)) return false;
            if (!output.object.emplace(std::move(key), std::move(child)).second) { error = "manifest JSON object contains a duplicate key"; return false; }
            if (consume('}')) return true;
            if (!consume(',')) { error = "manifest JSON object separator is missing"; return false; }
        }
        error = "manifest JSON object has too many members";
        return false;
    }

    bool array_value(ManifestJsonValue& output, std::string& error, std::size_t depth) {
        if (!consume('[')) { error = "manifest JSON array is invalid"; return false; }
        output.kind = ManifestJsonValue::Kind::Array;
        whitespace();
        if (consume(']')) return true;
        for (std::size_t count = 0; count < MaxArrayItems; ++count) {
            ManifestJsonValue child;
            if (!value(child, error, depth)) return false;
            output.array.push_back(std::move(child));
            if (consume(']')) return true;
            if (!consume(',')) { error = "manifest JSON array separator is missing"; return false; }
        }
        error = "manifest JSON array has too many values";
        return false;
    }

public:
    explicit ManifestJsonParser(std::string_view input) : input_(input) {}

    bool parse(ManifestJsonValue& output, std::string& error) {
        if (!value(output, error, 0)) return false;
        whitespace();
        if (position_ != input_.size()) { error = "manifest JSON has trailing data"; return false; }
        return true;
    }
};

const ManifestJsonValue* manifest_member(const ManifestJsonValue& value, std::string_view name) {
    if (value.kind != ManifestJsonValue::Kind::Object) return nullptr;
    const auto found = value.object.find(std::string(name));
    return found == value.object.end() ? nullptr : &found->second;
}

bool manifest_uint(const ManifestJsonValue* value, std::uint64_t& output) {
    if (!value || value->kind != ManifestJsonValue::Kind::Number || value->numberText.empty() ||
        value->numberText.find_first_of(".eE") != std::string::npos || value->numberText.front() == '-') return false;
    const auto parsed = std::from_chars(value->numberText.data(), value->numberText.data() + value->numberText.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == value->numberText.data() + value->numberText.size();
}

bool manifest_string(const ManifestJsonValue* value, std::string& output) {
    if (!value || value->kind != ManifestJsonValue::Kind::String || value->string.empty()) return false;
    output = value->string;
    return true;
}

AssetManifestScanStats AssetSystem::last_manifest_scan_stats() const {
    std::lock_guard scanLock(manifestScanMutex_);
    return manifestScanStats_;
}

bool AssetSystem::manifest_ready() const noexcept {
    return manifestReady_.load(std::memory_order_acquire);
}

bool AssetSystem::find_manifest(AssetId id, AssetManifestEntry& output) const {
    if (id == 0 || !manifest_ready()) return false;
    const auto snapshot = std::atomic_load_explicit(&manifestSnapshot_, std::memory_order_acquire);
    if (!snapshot || !manifest_ready()) return false;
    for (const auto& entry : *snapshot) {
        if (entry.id != id) continue;
        output = entry;
        return true;
    }
    return false;
}

AssetManifestReadResult AssetSystem::read_manifest(const std::filesystem::path& path) {
    constexpr std::uintmax_t maxManifestBytes = 64u * 1024u * 1024u;
    constexpr std::size_t maxEntries = 65536;
    AssetManifestReadResult result;
    if (path.empty()) { result.error = "manifest path is empty"; return result; }

    std::error_code fileError;
    const auto fileSize = std::filesystem::file_size(path, fileError);
    if (fileError || fileSize > maxManifestBytes) {
        result.error = fileError ? "manifest file size is unavailable" : "manifest file exceeds 64 MiB";
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) { result.error = "manifest file could not be opened"; return result; }
    std::string json(static_cast<std::size_t>(fileSize), '\0');
    if (!json.empty() && !input.read(json.data(), static_cast<std::streamsize>(json.size()))) {
        result.error = "manifest file could not be read";
        return result;
    }

    ManifestJsonValue root;
    if (!ManifestJsonParser(json).parse(root, result.error) || root.kind != ManifestJsonValue::Kind::Object) {
        if (result.error.empty()) result.error = "manifest root must be a JSON object";
        return result;
    }
    std::uint64_t version = 0;
    if (!manifest_uint(manifest_member(root, "version"), version) || version != 1) {
        result.error = "manifest version is unsupported";
        return result;
    }
    const auto* assets = manifest_member(root, "assets");
    if (!assets || assets->kind != ManifestJsonValue::Kind::Array || assets->array.size() > maxEntries) {
        result.error = "manifest assets array is missing or too large";
        return result;
    }

    std::unordered_set<AssetKey, AssetKeyHash> keys;
    std::unordered_set<AssetId> ids;
    result.entries.reserve(assets->array.size());
    for (std::size_t index = 0; index < assets->array.size(); ++index) {
        const auto& item = assets->array[index];
        if (item.kind != ManifestJsonValue::Kind::Object) {
            result.error = "manifest asset entry is not an object at index " + std::to_string(index);
            return result;
        }
        std::uint64_t id = 0;
        std::uint64_t sourceHash = 0;
        std::uint64_t sourceTimestamp = 0;
        std::uint64_t sourceSize = 0;
        std::string uri;
        std::string type;
        std::string source;
        if (!manifest_uint(manifest_member(item, "id"), id) || id == 0 ||
            !manifest_string(manifest_member(item, "uri"), uri) ||
            !manifest_string(manifest_member(item, "type"), type) ||
            !manifest_string(manifest_member(item, "source"), source) ||
            !manifest_uint(manifest_member(item, "hash"), sourceHash) ||
            !manifest_uint(manifest_member(item, "timestamp"), sourceTimestamp) ||
            !manifest_uint(manifest_member(item, "size"), sourceSize)) {
            result.error = "manifest asset entry has missing or invalid fields at index " + std::to_string(index);
            return result;
        }
        const auto canonicalUri = normalize_uri(uri);
        if (canonicalUri != uri || type.empty()) {
            result.error = "manifest asset key is not canonical at index " + std::to_string(index);
            return result;
        }
        const AssetKey key{canonicalUri, type};
        if (id != make_id(key)) {
            result.error = "manifest asset ID does not match its key at index " + std::to_string(index);
            return result;
        }
        if (!keys.insert(key).second || !ids.insert(id).second) {
            result.error = "manifest contains a duplicate asset identity at index " + std::to_string(index);
            return result;
        }
        result.entries.push_back({id, key, std::filesystem::path(source), sourceHash,
                                  sourceTimestamp, static_cast<std::uintmax_t>(sourceSize)});
    }
    result.valid = true;
    return result;
}

bool AssetSystem::seed_manifest_cache(const std::vector<AssetManifestEntry>& entries, std::string* error) {
    std::unordered_map<AssetKey, AssetManifestEntry, AssetKeyHash> seeded;
    std::unordered_set<AssetId> ids;
    seeded.reserve(entries.size());
    ids.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.id == 0 || entry.key.type.empty() || entry.key.uri.empty() ||
            normalize_uri(entry.key.uri) != entry.key.uri || entry.sourcePath.empty() ||
            entry.id != make_id(entry.key) || !ids.insert(entry.id).second ||
            !seeded.emplace(entry.key, entry).second) {
            if (error) *error = "manifest cache seed contains an invalid or duplicate entry";
            return false;
        }
    }
    {
        std::lock_guard scanLock(manifestScanMutex_);
        manifestCache_ = std::move(seeded);
    }
    std::shared_ptr<const std::vector<AssetManifestEntry>> published =
        std::make_shared<const std::vector<AssetManifestEntry>>(entries);
    std::atomic_store_explicit(&manifestSnapshot_, std::move(published), std::memory_order_release);
    manifestReady_.store(true, std::memory_order_release);
    return true;
}

bool AssetSystem::write_manifest(const std::filesystem::path& path) const {
    const auto entries = scan_sources();
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::trunc);
    if (!output) return false;
    output << "{\n  \"version\": 1,\n  \"assets\": [\n";
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        output << "    {\"id\": " << entry.id << ", \"uri\": \"" << json_escape(entry.key.uri) << "\", \"type\": \""
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
    listeners_.emplace(id, std::move(callback));
    return id;
}

void AssetSystem::unsubscribe(AssetEventId id) {
    std::lock_guard lock(mutex_);
    listeners_.erase(id);
}

} // namespace shinkou::assets
