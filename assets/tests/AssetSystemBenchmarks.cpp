#include "shinkou/assets/AssetSystem.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

class CopyingRawProcessor final : public shinkou::assets::IAssetProcessor {
public:
    bool process(const shinkou::assets::AssetProcessContext& context,
                 shinkou::assets::AssetArtifact& output, std::string&) const override {
        output.payload = context.sourceBytes;
        output.format = "copying-raw";
        return true;
    }
};

class CopyingRawLoader final : public shinkou::assets::IAssetLoader {
public:
    bool load(const shinkou::assets::AssetLoadContext& context,
              shinkou::assets::AssetData& output, std::string&) const override {
        output.bytes = std::make_shared<const std::vector<std::uint8_t>>(context.artifact.payload);
        output.format = context.artifact.format;
        return true;
    }
};

double raw_pipeline_benchmark(const std::filesystem::path& root, std::string type) {
    shinkou::assets::AssetSystem system({root, {}, 1, 256u * 1024u * 1024u,
                                         false, false, false, 10, 128});
    system.add_mount("project", root);
    if (type == "copying") {
        system.register_processor(type, std::make_unique<CopyingRawProcessor>());
        system.register_loader(type, std::make_unique<CopyingRawLoader>());
    }
    system.initialize();
    std::vector<shinkou::assets::AssetKey> keys;
    for (int index = 0; index < 8; ++index) {
        keys.push_back({"project://payload" + std::to_string(index) + ".bin", type});
    }
    const auto start = Clock::now();
    for (const auto& future : system.preload(std::move(keys))) {
        if (!future.get()) return -1.0;
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    system.shutdown();
    return elapsed;
}

double lookup_benchmark(shinkou::assets::AssetSystem& system,
                        const std::vector<shinkou::assets::AssetKey>& keys,
                        std::size_t threadCount, std::size_t iterationsPerThread) {
    std::atomic<std::uint64_t> checksum{0};
    const auto start = Clock::now();
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (std::size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        threads.emplace_back([&, threadIndex] {
            std::uint64_t local = 0;
            shinkou::assets::AssetSnapshot snapshot;
            for (std::size_t iteration = 0; iteration < iterationsPerThread; ++iteration) {
                const auto& key = keys[(iteration * 17 + threadIndex * 101) % keys.size()];
                if (system.find(key, snapshot)) local += snapshot.id;
            }
            checksum.fetch_add(local, std::memory_order_relaxed);
        });
    }
    for (auto& thread : threads) thread.join();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    const auto operations = static_cast<double>(threadCount * iterationsPerThread);
    std::cout << "lookup_threads=" << threadCount
              << " operations=" << static_cast<std::uint64_t>(operations)
              << " elapsed_ms=" << elapsed * 1000.0
              << " throughput_mops=" << operations / elapsed / 1.0e6
              << " checksum=" << checksum.load(std::memory_order_relaxed) << '\n';
    return elapsed;
}

double handle_benchmark(const shinkou::assets::AssetSystem& system,
                        const shinkou::assets::AssetHandle& handle,
                        std::size_t threadCount, std::size_t iterationsPerThread) {
    std::atomic<std::uint64_t> checksum{0};
    const auto start = Clock::now();
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (std::size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        threads.emplace_back([&] {
            std::uint64_t local = 0;
            for (std::size_t iteration = 0; iteration < iterationsPerThread; ++iteration) {
                local += system.is_valid(handle) ? 1u : 0u;
            }
            checksum.fetch_add(local, std::memory_order_relaxed);
        });
    }
    for (auto& thread : threads) thread.join();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    const auto operations = static_cast<double>(threadCount * iterationsPerThread);
    std::cout << "handle_threads=" << threadCount
              << " operations=" << static_cast<std::uint64_t>(operations)
              << " elapsed_ms=" << elapsed * 1000.0
              << " throughput_mops=" << operations / elapsed / 1.0e6
              << " checksum=" << checksum.load(std::memory_order_relaxed) << '\n';
    return elapsed;
}

} // namespace

int main() {
    constexpr std::size_t KeyCount = 20000;
    constexpr std::size_t TotalOperations = 1000000;
    shinkou::assets::AssetSystem system;
    std::vector<shinkou::assets::AssetKey> keys;
    keys.reserve(KeyCount);
    for (std::size_t index = 0; index < KeyCount; ++index) {
        keys.push_back({"project://generated/asset_" + std::to_string(index) + ".bin", "raw"});
        system.pin(keys.back());
    }

    lookup_benchmark(system, keys, 1, TotalOperations);
    const auto hardware = std::thread::hardware_concurrency();
    const auto threads = std::max<std::size_t>(2, std::min<std::size_t>(8, hardware == 0 ? 4 : hardware));
    lookup_benchmark(system, keys, threads, TotalOperations / threads);

    const auto root = std::filesystem::temp_directory_path() / "shinkou_assets_benchmark";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    std::ofstream(root / "handle.bin", std::ios::binary) << "handle";
    shinkou::assets::AssetSystem loaded({root, root / "cache", 1, 1024 * 1024,
                                         false, false, false, 100, 16});
    loaded.add_mount("project", root);
    loaded.initialize();
    const auto result = loaded.load({"project://handle.bin", "raw"});
    if (!result) return 2;
    handle_benchmark(loaded, result.handle, 1, TotalOperations);
    handle_benchmark(loaded, result.handle, threads, TotalOperations / threads);
    loaded.shutdown();

    std::vector<std::uint8_t> payload(4u * 1024u * 1024u, 0x5a);
    for (int index = 0; index < 8; ++index) {
        std::ofstream output(root / ("payload" + std::to_string(index) + ".bin"), std::ios::binary);
        output.write(reinterpret_cast<const char*>(payload.data()),
                     static_cast<std::streamsize>(payload.size()));
    }
    const auto copyingMilliseconds = raw_pipeline_benchmark(root, "copying");
    const auto zeroCopyMilliseconds = raw_pipeline_benchmark(root, "raw");
    std::cout << "raw_pipeline_mib=32 copying_ms=" << copyingMilliseconds
              << " zero_copy_ms=" << zeroCopyMilliseconds << '\n';
    std::filesystem::remove_all(root, error);
    return 0;
}
