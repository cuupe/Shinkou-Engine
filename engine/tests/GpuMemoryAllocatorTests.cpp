#include "shinkou/render/GpuMemoryAllocator.h"

#include <cassert>
#include <iostream>
#include <utility>
#include <vector>

int main() {
    using namespace shinkou::render;

    GpuMemoryAllocator allocator({1024, 2048});
    const auto first = allocator.allocate(200, 64, 1);
    const auto second = allocator.allocate(128, 128, 1);
    assert(first && second);
    assert(first.offset % 64 == 0);
    assert(second.offset % 128 == 0);
    assert(allocator.owns(first));
    assert(allocator.stats(1).liveBytes == 328);
    assert(allocator.stats(1).livePeakBytes == 328);

    assert(allocator.release(first));
    assert(!allocator.owns(first));
    const auto reused = allocator.allocate(160, 32, 1);
    assert(reused);
    assert(reused.offset == first.offset);
    assert(allocator.stats(1).freeRangeReuseCount >= 1);
    assert(!allocator.release(first));

    allocator.set_budget(2, 1024);
    const auto budgeted = allocator.allocate(900, 16, 2);
    assert(budgeted);
    assert(!allocator.allocate(200, 16, 2));
    assert(allocator.stats(2).failedAllocationCount == 1);
    assert(allocator.release(budgeted));
    assert(allocator.trim() == 1024);
    assert(allocator.stats(2).committedBytes == 0);

    const std::vector<TransientResourceRequest> requests{
        {1, 256, 64, 0, 0, 2},
        {2, 128, 32, 0, 3, 4},
        {3, 128, 32, 0, 2, 3},
        {4, 128, 32, 1, 0, 4},
    };
    auto plan = allocator.plan_transient_aliases(requests);
    assert(plan.valid);
    assert(plan.slots.size() == 3);
    assert(plan.aliasCount == 1);
    assert(plan.assignments[0].slotIndex == plan.assignments[1].slotIndex);
    assert(plan.assignments[0].slotIndex != plan.assignments[2].slotIndex);
    assert(plan.assignments[0].slotIndex != plan.assignments[3].slotIndex);
    assert(plan.requiredBytes == 512);
    assert(plan.peakBytes == 512);
    assert(allocator.materialize_transient_alias_plan(plan));
    assert(plan.assignments[0].allocation == plan.assignments[1].allocation);
    assert(plan.assignments[0].allocation);
    allocator.release_transient_alias_plan(plan);
    assert(!plan.materialized);

    const std::vector<TransientResourceRequest> incompatibleRequests{
        {10, 128, 32, 0, 0, 0, 0, 0, 100},
        {11, 128, 32, 0, 1, 1, 1, 1, 200},
    };
    TransientAliasPlanOptions compatibilityOptions;
    compatibilityOptions.canAlias = [](const auto& left, const auto& right) {
        return left.compatibilityKey == right.compatibilityKey;
    };
    const auto incompatiblePlan = allocator.plan_transient_aliases(incompatibleRequests, compatibilityOptions);
    assert(incompatiblePlan.valid);
    assert(incompatiblePlan.aliasCount == 0);
    assert(incompatiblePlan.slots.size() == 2);

    const auto invalid = allocator.plan_transient_aliases({{5, 8, 3, 0, 0, 0}});
    assert(!invalid.valid);
    assert(!invalid.error.empty());

    GpuMemoryAllocator movedFrom({256, 512});
    GpuMemoryAllocator movedTo(std::move(movedFrom));
    assert(movedFrom.reset(GpuMemoryLifetime::Transient) == 0);
    assert(movedFrom.stats().committedBytes == 0);
    assert(movedTo.allocate(32, 16));

    std::cout << "gpu memory allocator tests passed\n";
    return 0;
}
