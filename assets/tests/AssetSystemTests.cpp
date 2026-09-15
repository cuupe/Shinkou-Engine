#include "shinkou/assets/AssetSystem.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

class TextProcessor final : public shinkou::assets::IAssetProcessor {
public:
    bool process(const shinkou::assets::AssetProcessContext& context,
                 shinkou::assets::AssetArtifact& output, std::string&) const override {
        output.payload = context.sourceBytes;
        output.format = "text";
        output.dependencies.push_back({{"project://shared.bin", "raw"}, true});
        output.dependencies.push_back({{"project://shared.bin", "raw"}, true});
        return true;
    }
};

class SchedulingProcessor final : public shinkou::assets::IAssetProcessor {
    mutable std::mutex mutex_;
    mutable std::condition_variable wakeup_;
    mutable bool entered_{false};
    mutable bool released_{false};
    mutable std::vector<std::string> order_;

public:
    bool process(const shinkou::assets::AssetProcessContext& context,
                 shinkou::assets::AssetArtifact& output, std::string&) const override {
        const auto name = context.sourcePath.filename().string();
        {
            std::unique_lock lock(mutex_);
            order_.push_back(name);
            if (name == "block.sched") {
                entered_ = true;
                wakeup_.notify_all();
                wakeup_.wait(lock, [&] { return released_; });
            }
        }
        output.payload = context.sourceBytes;
        output.format = "scheduled";
        return true;
    }

    void wait_until_entered() const {
        std::unique_lock lock(mutex_);
        wakeup_.wait(lock, [&] { return entered_; });
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        wakeup_.notify_all();
    }

    std::vector<std::string> order() const {
        std::lock_guard lock(mutex_);
        return order_;
    }
};

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "shinkou_assets_standalone_tests";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "assets", error);
    std::ofstream(root / "assets" / "shared.bin", std::ios::binary) << "shared";
    std::ofstream(root / "assets" / "hello.txt", std::ios::binary) << "hello";
    const std::uint8_t previewPng[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x20,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    {
        std::ofstream image(root / "assets" / "preview.png", std::ios::binary);
        image.write(reinterpret_cast<const char*>(previewPng), static_cast<std::streamsize>(sizeof(previewPng)));
    }
    const std::uint8_t previewWav[] = {
        'R', 'I', 'F', 'F', 36, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
        0x80, 0xbb, 0x00, 0x00, 0x00, 0xee, 0x02, 0x00,
        4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    {
        std::ofstream audio(root / "assets" / "preview.wav", std::ios::binary);
        audio.write(reinterpret_cast<const char*>(previewWav), static_cast<std::streamsize>(sizeof(previewWav)));
    }
    std::ofstream(root / "assets" / "preview.obj") << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    std::ofstream(root / "assets" / "preview.avi", std::ios::binary) << "video";

    shinkou::assets::AssetSystem system({root, root / "cache", 2, 4u * 1024u * 1024u,
                                         true, true, false, 10, 128, false, 1});
    system.add_mount("project", root / "assets");
    system.register_processor("text", std::make_unique<TextProcessor>());
    system.register_extension("txt", "text");
    system.register_extension("bin", "raw");
    std::atomic<int> events{0};
    system.subscribe([&events](const shinkou::assets::AssetSnapshot&, shinkou::assets::AssetEventType) { ++events; });
    assert(system.initialize());

    const shinkou::assets::AssetKey key{"project://hello.txt", "text"};
    const auto first = system.request(key, {false, false, 10});
    const auto second = system.request(key, {false, false, 1});
    const auto loaded = first.get();
    assert(loaded && second.get().id == loaded.id && loaded.data->size() == 5);
    const auto typedTexture = system.load({"project://preview.png", ""});
    assert(typedTexture && typedTexture.data->format == "texture" &&
           typedTexture.data->metadataFormat == "shinkou.asset.texture.v1" &&
           typedTexture.data->metadata && typedTexture.data->metadata->find("\"width\":64") != std::string::npos &&
           typedTexture.data->metadata->find("\"height\":32") != std::string::npos);
    const auto typedAudio = system.load({"project://preview.wav", ""});
    assert(typedAudio && typedAudio.data->format == "audio" &&
           typedAudio.data->metadataFormat == "shinkou.asset.audio.v1" && typedAudio.data->metadata &&
           typedAudio.data->metadata->find("\"sampleRate\":48000") != std::string::npos);
    const auto typedModel = system.load({"project://preview.obj", ""});
    assert(typedModel && typedModel.data->format == "model" &&
           typedModel.data->metadataFormat == "shinkou.asset.model.obj.v1" && typedModel.data->metadata &&
           typedModel.data->metadata->find("\"vertices\":3") != std::string::npos &&
           typedModel.data->metadata->find("\"faces\":1") != std::string::npos);
    const auto typedVideo = system.load({"project://preview.avi", ""});
    assert(typedVideo && typedVideo.data->format == "video" &&
           typedVideo.data->metadataFormat == "shinkou.asset.source.v1");
    system.poll();
    assert(events.load() >= 1);

    shinkou::assets::AssetHandle oldHandle;
    assert(system.make_handle(key, oldHandle) && system.is_valid(oldHandle));
    assert(system.dependents({"project://shared.bin", "raw"}).size() == 1);

    system.pin(key);
    system.set_memory_budget(0);
    shinkou::assets::AssetSnapshot snapshot;
    assert(system.find(key, snapshot) && snapshot.state == shinkou::assets::AssetState::Ready);
    system.unpin(key);
    system.trim();
    assert(system.find(key, snapshot) && snapshot.state == shinkou::assets::AssetState::Unloaded);

    const auto loadedAgain = system.load(key);
    assert(loadedAgain && loadedAgain.data->size() == 5);
    assert(system.stats().cacheHits >= 1);
    const auto typedTextureFromCache = system.load({"project://preview.png", "texture"});
    assert(typedTextureFromCache && typedTextureFromCache.data->metadataFormat == "shinkou.asset.texture.v1" &&
           typedTextureFromCache.data->metadata && typedTextureFromCache.data->metadata->find("\"width\":64") != std::string::npos);

    std::vector<std::filesystem::path> cacheFiles;
    for (const auto& entry : std::filesystem::directory_iterator(root / "cache")) {
        if (entry.path().extension() == ".wac") cacheFiles.push_back(entry.path());
        assert(entry.path().filename().string().find(".tmp-") == std::string::npos);
    }
    assert(!cacheFiles.empty());
    for (const auto& cacheFile : cacheFiles) {
        std::ofstream(cacheFile, std::ios::binary | std::ios::trunc) << "damaged cache";
    }
    system.trim();
    const auto recovered = system.load(key);
    assert(recovered && recovered.data->size() == 5);

    const auto preloaded = system.preload({{"project://hello.txt", "text"},
                                           {"project://shared.bin", "raw"}},
                                          {false, false, -10});
    assert(preloaded.size() == 2);
    for (const auto& future : preloaded) assert(future.get());

    const auto baselineManifestEntries = system.scan_sources();
    const auto baselineManifestStats = system.last_manifest_scan_stats();
    assert(baselineManifestEntries.size() == 6 && baselineManifestStats.cacheMisses >= 6);
    assert(system.manifest_ready());
    shinkou::assets::AssetManifestEntry manifestById;
    assert(system.find_manifest(baselineManifestEntries.front().id, manifestById) &&
           manifestById.key == baselineManifestEntries.front().key);

    std::ofstream(root / "assets" / "shared.bin", std::ios::binary | std::ios::trunc) << "changed";
    for (int index = 0; index < 4; ++index) system.poll();
    assert(system.find(key, snapshot) && snapshot.state == shinkou::assets::AssetState::Stale);
    assert(!system.is_valid(loadedAgain.handle));
    const auto reloaded = system.reload(key);
    assert(reloaded && reloaded.data->size() == 5 && system.is_valid(reloaded.handle));

    const auto manifestEntries = system.scan_sources();
    const auto manifestEntry = std::find_if(manifestEntries.begin(), manifestEntries.end(),
        [&](const shinkou::assets::AssetManifestEntry& entry) { return entry.key == key; });
    assert(manifestEntry != manifestEntries.end());
    assert(manifestEntry->id == reloaded.id && manifestEntry->sourceHash != 0);
    const auto firstManifestStats = system.last_manifest_scan_stats();
    assert(firstManifestStats.entries == manifestEntries.size() && firstManifestStats.cacheMisses >= 1 &&
           firstManifestStats.cacheHits >= 1);
    const auto changedShared = std::find_if(manifestEntries.begin(), manifestEntries.end(),
        [](const shinkou::assets::AssetManifestEntry& entry) { return entry.key.uri == "project://shared.bin"; });
    assert(changedShared != manifestEntries.end() && changedShared->sourceSize == 7);
    const auto cachedManifestEntries = system.scan_sources();
    const auto cachedManifestStats = system.last_manifest_scan_stats();
    assert(cachedManifestEntries.size() == manifestEntries.size());
    assert(cachedManifestStats.cacheHits == cachedManifestEntries.size() && cachedManifestStats.cacheMisses == 0);

    std::vector<std::thread> readers;
    std::atomic<int> successfulReads{0};
    for (int threadIndex = 0; threadIndex < 8; ++threadIndex) {
        readers.emplace_back([&] {
            for (int iteration = 0; iteration < 2000; ++iteration) {
                if (system.get(reloaded.handle) && system.is_valid(reloaded.handle)) ++successfulReads;
            }
        });
    }
    for (auto& reader : readers) reader.join();
    assert(successfulReads == 16000);
    const auto currentStats = system.stats();
    assert(currentStats.ready >= 1 && currentStats.loading == 0);

    const auto manifest = root / "manifest.json";
    assert(system.write_manifest(manifest));
    assert(std::filesystem::file_size(manifest, error) > 0);
    std::ifstream manifestInput(manifest);
    const std::string manifestText((std::istreambuf_iterator<char>(manifestInput)), std::istreambuf_iterator<char>());
    assert(manifestText.find("\"id\":") != std::string::npos);
    const auto restoredManifest = shinkou::assets::AssetSystem::read_manifest(manifest);
    assert(restoredManifest && restoredManifest.entries.size() == 6);
    const auto restoredEntry = std::find_if(restoredManifest.entries.begin(), restoredManifest.entries.end(),
        [&](const shinkou::assets::AssetManifestEntry& entry) { return entry.key == key; });
    assert(restoredEntry != restoredManifest.entries.end() && restoredEntry->id == reloaded.id);
    std::string seedError;
    assert(system.seed_manifest_cache(restoredManifest.entries, &seedError));
    assert(system.manifest_ready() && system.find_manifest(reloaded.id, manifestById));
    const auto seededManifestEntries = system.scan_sources();
    const auto seededManifestStats = system.last_manifest_scan_stats();
    assert(seededManifestEntries.size() == restoredManifest.entries.size() &&
           seededManifestStats.cacheHits == seededManifestEntries.size());

    const auto invalidManifest = root / "invalid-manifest.json";
    std::ofstream(invalidManifest) <<
        "{\"version\":1,\"assets\":[{\"id\":1,\"uri\":\"project://hello.txt\",\"type\":\"text\","
        "\"source\":\"hello.txt\",\"hash\":1,\"timestamp\":1,\"size\":5}]}";
    const auto invalidResult = shinkou::assets::AssetSystem::read_manifest(invalidManifest);
    assert(!invalidResult && invalidResult.error.find("ID") != std::string::npos);
    system.shutdown();
    assert(!system.manifest_ready());

    // A shutdown system can be initialized again without retaining stale slots,
    // handles, queue entries, LRU links, or counters from its previous lifetime.
    assert(system.initialize());
    const auto afterRestart = system.load(key);
    assert(afterRestart && afterRestart.handle.slot == 0);
    system.shutdown();

    const auto schedulingRoot = root / "scheduling";
    std::filesystem::create_directories(schedulingRoot, error);
    std::ofstream(schedulingRoot / "block.sched", std::ios::binary) << "block";
    std::ofstream(schedulingRoot / "low.sched", std::ios::binary) << "low";
    std::ofstream(schedulingRoot / "high.sched", std::ios::binary) << "high";
    shinkou::assets::AssetSystem scheduler({schedulingRoot, schedulingRoot / "cache", 1,
                                            1024 * 1024, false, false, false, 10, 32});
    scheduler.add_mount("project", schedulingRoot);
    auto schedulingProcessor = std::make_unique<SchedulingProcessor>();
    auto* schedulingProbe = schedulingProcessor.get();
    scheduler.register_processor("scheduled", std::move(schedulingProcessor));
    scheduler.register_extension("sched", "scheduled");
    assert(scheduler.initialize());
    const auto blocker = scheduler.request({"project://block.sched", "scheduled"});
    schedulingProbe->wait_until_entered();
    const auto low = scheduler.request({"project://low.sched", "scheduled"}, {false, false, -10});
    const auto high = scheduler.request({"project://high.sched", "scheduled"}, {false, false, 10});
    const auto mergedLow = scheduler.request({"project://low.sched", "scheduled"}, {false, false, 20});
    schedulingProbe->release();
    assert(blocker.get() && low.get() && high.get() && mergedLow.get());
    const auto executionOrder = schedulingProbe->order();
    assert(executionOrder.size() == 3);
    assert(executionOrder[0] == "block.sched");
    assert(executionOrder[1] == "low.sched");
    assert(executionOrder[2] == "high.sched");
    scheduler.shutdown();

    std::filesystem::remove_all(root, error);
    return 0;
}
