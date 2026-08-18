#include "shinkou/assets/AssetSystem.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class TextProcessor final : public shinkou::assets::IAssetProcessor {
public:
    bool process(const shinkou::assets::AssetProcessContext& context,
                 shinkou::assets::AssetArtifact& output, std::string&) const override {
        output.payload = context.sourceBytes;
        output.format = "text";
        output.dependencies.push_back({{"project://shared.bin", "raw"}, true});
        return true;
    }
};

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "shinkou_asset_system_tests";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "assets", error);
    {
        std::ofstream(root / "assets" / "shared.bin", std::ios::binary) << "shared";
        std::ofstream(root / "assets" / "hello.txt", std::ios::binary) << "hello";
    }

    shinkou::assets::AssetSystem system({root, root / "cache", 2, 4u * 1024u * 1024u, true, true});
    system.add_mount("project", root / "assets");
    system.register_processor("text", std::make_unique<TextProcessor>());
    system.register_extension("txt", "text");
    std::atomic<int> loadedEvents{0};
    system.subscribe([&loadedEvents](const shinkou::assets::AssetSnapshot&, shinkou::assets::AssetEventType event) {
        if (event == shinkou::assets::AssetEventType::Loaded || event == shinkou::assets::AssetEventType::Reloaded) ++loadedEvents;
    });
    assert(system.initialize());

    const shinkou::assets::AssetKey key{"project://hello.txt", "text"};
    const auto first = system.request(key);
    const auto second = system.request(key);
    assert(first.valid() && second.valid());
    const auto loaded = first.get();
    assert(loaded && loaded.data->size() == 5 && loaded.data->format == "text");
    assert(loadedEvents.load() >= 1);
    assert(second.get().id == loaded.id);

    shinkou::assets::AssetSnapshot snapshot;
    assert(system.find(key, snapshot) && snapshot.state == shinkou::assets::AssetState::Ready);
    assert(!snapshot.sourcePath.empty());
    system.pin(key);
    system.trim();
    assert(system.find(key, snapshot) && snapshot.state == shinkou::assets::AssetState::Ready);
    system.unpin(key);

    const auto manifest = root / "manifest.json";
    assert(system.write_manifest(manifest));
    assert(std::filesystem::file_size(manifest, error) > 0);

    std::ofstream(root / "assets" / "hello.txt", std::ios::binary | std::ios::trunc) << "reload";
    system.poll();
    assert(system.find(key, snapshot) && snapshot.state == shinkou::assets::AssetState::Stale);
    const auto reloaded = system.load(key);
    assert(reloaded && reloaded.data->size() == 6);

    const auto stats = system.stats();
    assert(stats.ready >= 1 && stats.records >= 1);
    system.shutdown();
    std::filesystem::remove_all(root, error);
    return 0;
}
