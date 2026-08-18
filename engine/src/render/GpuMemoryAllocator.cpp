#include "shinkou/render/GpuMemoryAllocator.h"

#include <algorithm>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace shinkou::render {
namespace {
bool power_of_two(std::size_t value) noexcept { return value != 0 && (value & (value - 1)) == 0; }
bool add_overflow(std::size_t a, std::size_t b, std::size_t& out) noexcept {
    if (b > std::numeric_limits<std::size_t>::max() - a) return true;
    out = a + b;
    return false;
}
bool align_up(std::size_t value, std::size_t alignment, std::size_t& out) noexcept {
    if (!power_of_two(alignment)) return true;
    const auto mask = alignment - 1;
    if (value > std::numeric_limits<std::size_t>::max() - mask) return true;
    out = (value + mask) & ~mask;
    return false;
}
struct Block {
    std::uint64_t id{0};
    std::size_t capacity{0};
    std::size_t live{0};
    std::map<std::size_t, std::size_t> freeRanges;
};
struct Pool {
    std::vector<Block> blocks;
    std::size_t committed{0};
    std::size_t committedPeak{0};
    std::size_t liveBytes{0};
    std::size_t livePeak{0};
    std::size_t liveCount{0};
    std::uint64_t allocations{0};
    std::uint64_t releases{0};
    std::uint64_t reused{0};
    std::uint64_t failures{0};
};
struct Record {
    GpuMemoryType memoryType{0};
    std::uint64_t blockId{0};
    std::size_t offset{0};
    std::size_t size{0};
    std::size_t alignment{1};
    GpuMemoryLifetime lifetime{GpuMemoryLifetime::Persistent};
};
Block* find_block(Pool& pool, std::uint64_t id) noexcept {
    const auto it = std::find_if(pool.blocks.begin(), pool.blocks.end(),
        [id](const Block& block) { return block.id == id; });
    return it == pool.blocks.end() ? nullptr : &*it;
}
} // namespace

struct GpuMemoryAllocator::Impl {
    explicit Impl(GpuMemoryAllocatorConfig value) : config(value) {
        if (config.blockSize == 0) config.blockSize = 1;
    }
    GpuMemoryAllocatorConfig config;
    std::unordered_map<GpuMemoryType, Pool> pools;
    std::unordered_map<GpuMemoryType, std::size_t> budgets;
    std::unordered_map<std::uint64_t, Record> records;
    std::uint64_t nextBlock{1};
    std::uint64_t nextAllocation{1};
};

const GpuAliasAssignment* GpuAliasPlan::assignment(std::uint64_t id) const noexcept {
    const auto it = std::find_if(assignments_.begin(), assignments_.end(),
        [id](const GpuAliasAssignment& value) { return value.resourceId == id; });
    return it == assignments_.end() ? nullptr : &*it;
}
std::uint64_t GpuAliasPlan::physical_resource(std::uint64_t id) const noexcept {
    const auto* value = assignment(id);
    return value ? value->physicalResourceId : 0;
}

std::uint64_t TransientAliasPlan::physical_resource(std::uint64_t logicalResource) const noexcept {
    const auto it = std::find_if(assignments.begin(), assignments.end(),
        [logicalResource](const TransientAliasAssignment& assignment) {
            return assignment.resourceId == logicalResource;
        });
    if (it == assignments.end() || it->slotIndex >= slots.size()) return 0;
    return materialized ? it->allocation.id : slots[it->slotIndex].physicalResourceId;
}

GpuMemoryAllocator::GpuMemoryAllocator(GpuMemoryAllocatorConfig config)
    : impl_(std::make_unique<Impl>(config)) {}
GpuMemoryAllocator::~GpuMemoryAllocator() = default;
GpuMemoryAllocator::GpuMemoryAllocator(GpuMemoryAllocator&&) noexcept = default;
GpuMemoryAllocator& GpuMemoryAllocator::operator=(GpuMemoryAllocator&&) noexcept = default;

GpuAllocation GpuMemoryAllocator::allocate(std::size_t size, std::size_t alignment,
                                            GpuMemoryType type, GpuMemoryLifetime lifetime) noexcept {
    if (!impl_ || size == 0 || !power_of_two(alignment)) return {};
    auto& pool = impl_->pools[type];
    Block* selected = nullptr;
    std::size_t rangeStart = 0;
    std::size_t aligned = 0;
    for (auto& block : pool.blocks) {
        for (const auto& range : block.freeRanges) {
            std::size_t candidate = 0, end = 0;
            if (align_up(range.first, alignment, candidate) ||
                add_overflow(candidate, size, end) || end > range.first + range.second) continue;
            selected = &block;
            rangeStart = range.first;
            aligned = candidate;
            break;
        }
        if (selected) break;
    }
    if (!selected) {
        const std::size_t capacity = std::max(impl_->config.blockSize, size);
        const std::size_t budget = impl_->budgets.count(type) ? impl_->budgets[type] : impl_->config.budgetBytes;
        if (pool.committed > budget || capacity > budget - pool.committed) { ++pool.failures; return {}; }
        pool.blocks.push_back({impl_->nextBlock++, capacity, 0, {{0, capacity}}});
        pool.committed += capacity;
        pool.committedPeak = std::max(pool.committedPeak, pool.committed);
        selected = &pool.blocks.back();
        rangeStart = 0;
        aligned = 0;
    } else {
        ++pool.reused;
    }
    const auto rangeIt = selected->freeRanges.find(rangeStart);
    if (rangeIt == selected->freeRanges.end()) { ++pool.failures; return {}; }
    const auto rangeEnd = rangeIt->first + rangeIt->second;
    const auto allocationEnd = aligned + size;
    selected->freeRanges.erase(rangeIt);
    if (aligned > rangeStart) selected->freeRanges.emplace(rangeStart, aligned - rangeStart);
    if (allocationEnd < rangeEnd) selected->freeRanges.emplace(allocationEnd, rangeEnd - allocationEnd);
    ++selected->live;
    ++pool.liveCount;
    ++pool.allocations;
    pool.liveBytes += size;
    pool.livePeak = std::max(pool.livePeak, pool.liveBytes);
    const auto id = impl_->nextAllocation++;
    impl_->records.emplace(id, Record{type, selected->id, aligned, size, alignment, lifetime});
    return {id, selected->id, aligned, size, alignment, type, lifetime};
}

bool GpuMemoryAllocator::release(GpuAllocation allocation) noexcept {
    if (!impl_ || !allocation) return false;
    const auto it = impl_->records.find(allocation.id);
    if (it == impl_->records.end()) return false;
    const Record record = it->second;
    if (record.memoryType != allocation.memoryType || record.blockId != allocation.blockId ||
        record.offset != allocation.offset || record.size != allocation.size ||
        record.alignment != allocation.alignment || record.lifetime != allocation.lifetime) return false;
    auto& pool = impl_->pools[record.memoryType];
    auto* block = find_block(pool, record.blockId);
    if (!block) return false;
    block->freeRanges[record.offset] += record.size;
    auto current = block->freeRanges.find(record.offset);
    if (current != block->freeRanges.begin()) {
        auto previous = std::prev(current);
        if (previous->first + previous->second == current->first) {
            previous->second += current->second;
            current = block->freeRanges.erase(current);
        }
    }
    auto next = std::next(current);
    if (next != block->freeRanges.end() && current->first + current->second == next->first) {
        current->second += next->second;
        block->freeRanges.erase(next);
    }
    --block->live;
    --pool.liveCount;
    pool.liveBytes -= record.size;
    ++pool.releases;
    impl_->records.erase(it);
    return true;
}

bool GpuMemoryAllocator::owns(GpuAllocation allocation) const noexcept {
    if (!impl_ || !allocation) return false;
    const auto it = impl_->records.find(allocation.id);
    return it != impl_->records.end() && it->second.memoryType == allocation.memoryType &&
        it->second.blockId == allocation.blockId && it->second.offset == allocation.offset &&
        it->second.size == allocation.size && it->second.alignment == allocation.alignment &&
        it->second.lifetime == allocation.lifetime;
}

std::size_t GpuMemoryAllocator::block_capacity(std::uint64_t blockId) const noexcept {
    if (!impl_ || blockId == 0) return 0;
    for (const auto& pool : impl_->pools) {
        const auto block = std::find_if(pool.second.blocks.begin(), pool.second.blocks.end(),
            [blockId](const Block& value) { return value.id == blockId; });
        if (block != pool.second.blocks.end()) return block->capacity;
    }
    return 0;
}

std::size_t GpuMemoryAllocator::reset(GpuMemoryLifetime lifetime) noexcept {
    std::vector<GpuAllocation> allocations;
    for (const auto& entry : impl_->records) if (entry.second.lifetime == lifetime) {
        const auto& r = entry.second;
        allocations.push_back({entry.first, r.blockId, r.offset, r.size, r.alignment, r.memoryType, r.lifetime});
    }
    for (const auto allocation : allocations) release(allocation);
    return allocations.size();
}

void GpuMemoryAllocator::set_default_budget(std::size_t bytes) noexcept { impl_->config.budgetBytes = bytes; }
std::size_t GpuMemoryAllocator::default_budget() const noexcept { return impl_->config.budgetBytes; }
void GpuMemoryAllocator::set_budget(GpuMemoryType type, std::size_t bytes) noexcept { impl_->budgets[type] = bytes; }
void GpuMemoryAllocator::clear_budget(GpuMemoryType type) noexcept { impl_->budgets.erase(type); }
std::size_t GpuMemoryAllocator::budget(GpuMemoryType type) const noexcept {
    const auto it = impl_->budgets.find(type);
    return it == impl_->budgets.end() ? impl_->config.budgetBytes : it->second;
}

GpuMemoryStats GpuMemoryAllocator::stats(GpuMemoryType type) const noexcept {
    GpuMemoryStats result;
    result.budgetBytes = budget(type);
    const auto it = impl_->pools.find(type);
    if (it == impl_->pools.end()) return result;
    const auto& pool = it->second;
    result.committedBytes = pool.committed;
    result.committedPeakBytes = pool.committedPeak;
    result.liveBytes = pool.liveBytes;
    result.livePeakBytes = pool.livePeak;
    result.blockCount = pool.blocks.size();
    result.liveAllocationCount = pool.liveCount;
    result.allocationCount = pool.allocations;
    result.releaseCount = pool.releases;
    result.freeRangeReuseCount = pool.reused;
    result.failedAllocationCount = pool.failures;
    for (const auto& block : pool.blocks) for (const auto& range : block.freeRanges) result.freeBytes += range.second;
    return result;
}
GpuMemoryStats GpuMemoryAllocator::stats() const noexcept {
    GpuMemoryStats result;
    result.budgetBytes = impl_->config.budgetBytes;
    for (const auto& pair : impl_->pools) {
        const auto value = stats(pair.first);
        result.committedBytes += value.committedBytes;
        result.committedPeakBytes += value.committedPeakBytes;
        result.liveBytes += value.liveBytes;
        result.livePeakBytes += value.livePeakBytes;
        result.freeBytes += value.freeBytes;
        result.blockCount += value.blockCount;
        result.liveAllocationCount += value.liveAllocationCount;
        result.allocationCount += value.allocationCount;
        result.releaseCount += value.releaseCount;
        result.freeRangeReuseCount += value.freeRangeReuseCount;
        result.failedAllocationCount += value.failedAllocationCount;
    }
    return result;
}

std::size_t GpuMemoryAllocator::trim() noexcept {
    std::size_t released = 0;
    for (auto& pair : impl_->pools) {
        auto& pool = pair.second;
        auto it = pool.blocks.begin();
        while (it != pool.blocks.end()) {
            if (it->live != 0) { ++it; continue; }
            released += it->capacity;
            pool.committed -= it->capacity;
            it = pool.blocks.erase(it);
        }
    }
    return released;
}

GpuAliasPlan GpuMemoryAllocator::plan_aliases(const std::vector<GpuAliasRequest>& requests,
                                               const GpuAliasPlanOptions& options) const {
    struct Slot { GpuAliasRequest owner; };
    std::vector<std::size_t> order;
    std::unordered_set<std::uint64_t> ids;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& request = requests[i];
        if (!request.resourceId || !request.size || !power_of_two(request.alignment) ||
            request.firstUse > request.lastUse || !ids.insert(request.resourceId).second) return {};
        order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&requests](std::size_t a, std::size_t b) {
        return requests[a].firstUse != requests[b].firstUse ? requests[a].firstUse < requests[b].firstUse
                                                              : requests[a].resourceId < requests[b].resourceId;
    });
    std::vector<Slot> slots;
    std::vector<GpuAliasAssignment> assignments;
    for (const auto index : order) {
        const auto& request = requests[index];
        auto slot = std::find_if(slots.begin(), slots.end(), [&](const Slot& candidate) {
            const auto& owner = candidate.owner;
            if (owner.lifetime == GpuMemoryLifetime::Persistent || request.lifetime == GpuMemoryLifetime::Persistent ||
                owner.resourceType != request.resourceType || owner.compatibilityKey != request.compatibilityKey ||
                owner.size < request.size || owner.lastUse >= request.firstUse) return false;
            return !options.canAlias || options.canAlias(owner, request);
        });
        if (slot == slots.end()) {
            slots.push_back({request});
            assignments.push_back({request.resourceId, request.resourceId, request.resourceType, 0, request.size, request.alignment, false});
        } else {
            assignments.push_back({request.resourceId, slot->owner.resourceId, request.resourceType, 0, request.size, request.alignment, true});
            slot->owner = request;
        }
    }
    return GpuAliasPlan(std::move(assignments));
}

TransientAliasPlan GpuMemoryAllocator::plan_transient_aliases(
    const std::vector<TransientResourceRequest>& requests,
    const TransientAliasPlanOptions& options) const {
    TransientAliasPlan plan;
    plan.assignments.resize(requests.size());
    std::vector<std::size_t> order(requests.size());
    std::unordered_set<std::uint64_t> ids;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& request = requests[i];
        if (!request.resourceId || !request.size || !power_of_two(request.alignment) ||
            request.firstUse > request.lastUse || !ids.insert(request.resourceId).second) {
            plan.valid = false; plan.error = "invalid transient resource request"; return plan;
        }
        order[i] = i;
        plan.assignments[i].resourceId = request.resourceId;
    }
    std::sort(order.begin(), order.end(), [&requests](std::size_t a, std::size_t b) {
        return requests[a].firstUse != requests[b].firstUse ? requests[a].firstUse < requests[b].firstUse
                                                             : requests[a].resourceId < requests[b].resourceId;
    });
    std::vector<std::size_t> owners;
    for (const auto index : order) {
        const auto& request = requests[index];
        std::size_t selected = plan.slots.size();
        for (std::size_t i = 0; i < plan.slots.size(); ++i) {
            const auto& slot = plan.slots[i];
            const auto ownerIndex = owners[i];
            if (slot.memoryType != request.memoryType || slot.lastUse >= request.firstUse ||
                slot.size < request.size || slot.alignment < request.alignment) continue;
            if (options.canAlias && !options.canAlias(requests[ownerIndex], request)) continue;
            selected = i; break;
        }
        if (selected == plan.slots.size()) { selected = plan.slots.size(); plan.slots.push_back({request.size, request.alignment, request.memoryType, request.lastUse, {}, request.resourceId}); owners.push_back(index); }
        else { ++plan.aliasCount; plan.slots[selected].lastUse = request.lastUse; owners[selected] = index; }
        plan.assignments[index].slotIndex = selected;
    }
    for (const auto& slot : plan.slots) plan.requiredBytes += slot.size;
    plan.peakBytes = plan.requiredBytes;
    return plan;
}

bool GpuMemoryAllocator::materialize_transient_alias_plan(TransientAliasPlan& plan) noexcept {
    if (!plan.valid || plan.materialized) return plan.valid;
    for (auto& slot : plan.slots) {
        slot.allocation = allocate(slot.size, slot.alignment, slot.memoryType, GpuMemoryLifetime::Transient);
        if (!slot.allocation) { release_transient_alias_plan(plan); return false; }
    }
    for (auto& assignment : plan.assignments) assignment.allocation = plan.slots[assignment.slotIndex].allocation;
    plan.materialized = true;
    return true;
}
void GpuMemoryAllocator::release_transient_alias_plan(TransientAliasPlan& plan) noexcept {
    if (!plan.materialized) return;
    for (auto& slot : plan.slots) { release(slot.allocation); slot.allocation = {}; }
    for (auto& assignment : plan.assignments) assignment.allocation = {};
    plan.materialized = false;
}
} // namespace shinkou::render
