#include "shinkou/assets/AssetSystem.h"

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

    shinkou::assets::AssetSystem system({root, root / "cache", 2, 4u * 1024u * 1024u,
                                         true, true, false, 10, 128, false, 1});
    system.add_mount("project", root / "assets");
    system.register_processor("text", std::make_unique<TextProcessor>());
    system.register_extension("txt", "text");
    std::atomic<int> events{0};
    system.subscribe([&events](const shinkou::assets::AssetSnapshot&, shinkou::assets::AssetEventType) { ++events; });
    assert(system.initialize());

    const shinkou::assets::AssetKey key{"project://hello.txt", "text"};
    const auto first = system.request(key, {false, false, 10});
    const auto second = system.request(key, {false, false, 1});
    const auto loaded = first.get();
    assert(loaded && second.get().id == loaded.id && loaded.data->size() == 5);
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

    std::ofstream(root / "assets" / "shared.bin", std::ios::binary | std::ios::trunc) << "changed";
    for (int index = 0; index < 4; ++index) system.poll();
    assert(system.find(key, snapshot) && snapshot.state == shinkou::assets::AssetState::Stale);
    assert(!system.is_valid(loadedAgain.handle));
    const auto reloaded = system.reload(key);
    assert(reloaded && reloaded.data->size() == 5 && system.is_valid(reloaded.handle));

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
    system.shutdown();

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
