#include "shinkou/render/DescriptorAllocator.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace shinkou::render;

class FakePoolBackend final : public IDescriptorPoolBackend {
public:
    bool create_pool(const DescriptorPoolCreateInfo& description,
                     DescriptorPoolBackendHandle& backendHandle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        created.push_back(description);
        backendHandle.value = nextHandle++;
        return true;
    }

    void destroy_pool(DescriptorPoolBackendHandle backendHandle) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        destroyed.push_back(backendHandle.value);
    }

    std::vector<DescriptorPoolCreateInfo> created;
    std::vector<std::uint64_t> destroyed;

private:
    std::mutex mutex_;
    std::uint64_t nextHandle{1};
};

bool require(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}
} // namespace

int main() {
    FakePoolBackend backend;
    {
    DescriptorAllocator allocator({2, 2, 4, 0, 0, DescriptorPoolVisibility::ShaderVisible, &backend});

    const auto first = allocator.allocate(DescriptorType::Texture);
    const auto second = allocator.allocate(DescriptorType::Texture);
    if (!require(first && second, "initial descriptor allocation failed")) return 1;
    const auto firstLocation = allocator.resolve(*first);
    if (!require(firstLocation && firstLocation->slot == 0 && firstLocation->backendPool.value == 1,
                 "descriptor location did not resolve")) return 2;

    const auto third = allocator.allocate(DescriptorType::Texture);
    const auto fourth = allocator.allocate(DescriptorType::Texture);
    if (!require(third && fourth, "descriptor pool growth failed")) return 3;
    const auto stableLocation = allocator.resolve(*first);
    if (!require(stableLocation && stableLocation->pool == firstLocation->pool &&
                     stableLocation->slot == firstLocation->slot,
                 "growth invalidated an existing descriptor location")) return 4;

    if (!require(allocator.stats().capacity == 6 && allocator.stats().allocated == 4 &&
                     allocator.stats().poolCount == 2,
                 "allocator statistics did not account for growth")) return 5;

    const auto oldGeneration = first->generation;
    if (!require(allocator.release(*first) == DescriptorOperationResult::Success,
                 "descriptor release failed")) return 6;
    if (!require(!allocator.validate(*first), "released descriptor remained valid")) return 7;
    const auto recycled = allocator.allocate(DescriptorType::Texture);
    if (!require(recycled && recycled->index == first->index &&
                     recycled->generation != oldGeneration,
                 "descriptor recycling did not preserve index and advance generation")) return 8;
    if (!require(allocator.release(*first) == DescriptorOperationResult::StaleHandle,
                 "stale descriptor handle was accepted")) return 9;

    auto wrongType = *recycled;
    wrongType.type = DescriptorType::Sampler;
    if (!require(allocator.release(wrongType) == DescriptorOperationResult::TypeMismatch,
                 "descriptor type mismatch was accepted")) return 10;

    DescriptorAllocator budgeted({2, 2, 8, 0, 3});
    if (!require(budgeted.allocate(DescriptorType::Texture) &&
                     budgeted.allocate(DescriptorType::Texture) &&
                     budgeted.allocate(DescriptorType::Texture),
                 "budgeted allocator rejected an allocation within budget")) return 11;
    if (!require(!budgeted.allocate(DescriptorType::Texture) &&
                     budgeted.stats().budgetRejectCount == 1,
                 "descriptor budget was not enforced")) return 12;

    DescriptorAllocator concurrent({8, 2, 64, 0, 256});
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&] {
            for (int iteration = 0; iteration < 32; ++iteration) {
                const auto handle = concurrent.allocate(DescriptorType::StorageBuffer);
                if (!handle || concurrent.release(*handle) != DescriptorOperationResult::Success) {
                    failed.store(true);
                    return;
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    if (!require(!failed.load() && concurrent.stats().allocated == 0 &&
                     concurrent.stats().allocationCount == 256 &&
                     concurrent.stats().releaseCount == 256,
                 "concurrent descriptor allocation/release was not thread-safe")) return 13;

    }
    if (!require(backend.created.size() == backend.destroyed.size(),
                 "backend pool lifetime was not balanced")) return 14;
    return 0;
}
