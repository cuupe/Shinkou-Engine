#include "shinkou/render/DescriptorAllocator.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace shinkou::render {
namespace {

const char* result_name(DescriptorOperationResult result) noexcept {
    switch (result) {
    case DescriptorOperationResult::Success: return "success";
    case DescriptorOperationResult::InvalidConfig: return "invalid descriptor allocator configuration";
    case DescriptorOperationResult::InvalidHandle: return "invalid descriptor handle";
    case DescriptorOperationResult::StaleHandle: return "stale descriptor handle";
    case DescriptorOperationResult::TypeMismatch: return "descriptor handle type mismatch";
    case DescriptorOperationResult::AlreadyFree: return "descriptor handle was already released";
    case DescriptorOperationResult::OutOfCapacity: return "descriptor allocator capacity is exhausted";
    case DescriptorOperationResult::BudgetExceeded: return "descriptor allocator budget is exhausted";
    case DescriptorOperationResult::PoolLimitExceeded: return "descriptor allocator pool limit is exhausted";
    case DescriptorOperationResult::BackendFailure: return "backend descriptor pool creation failed";
    }
    return "unknown descriptor allocator error";
}

} // namespace

DescriptorPool::DescriptorPool(DescriptorPoolId id, std::uint32_t logicalBaseIndex,
                               DescriptorPoolDesc description)
    : id_(id), logicalBaseIndex_(logicalBaseIndex), description_(description),
      slots_(description.capacity), freeSlots_() {
    freeSlots_.reserve(slots_.size());
    for (std::uint32_t slot = description.capacity; slot > 0; --slot) {
        freeSlots_.push_back(slot - 1u);
    }
}

std::uint32_t DescriptorPool::next_generation(std::uint32_t generation) noexcept {
    ++generation;
    return generation == 0 ? 1u : generation;
}

bool DescriptorPool::matches_unlocked(BindlessHandle handle) const noexcept {
    if (handle.pool != id_ || handle.type != description_.type || handle.generation == 0 ||
        handle.slot >= slots_.size() || !slots_[handle.slot].allocated) {
        return false;
    }
    const auto expectedIndex = static_cast<std::uint64_t>(logicalBaseIndex_) + handle.slot;
    return expectedIndex <= std::numeric_limits<std::uint32_t>::max() &&
        handle.index == static_cast<std::uint32_t>(expectedIndex) &&
        slots_[handle.slot].generation == handle.generation;
}

std::optional<BindlessHandle> DescriptorPool::allocate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (freeSlots_.empty()) return std::nullopt;

    const auto slot = freeSlots_.back();
    freeSlots_.pop_back();
    auto& entry = slots_[slot];
    entry.allocated = true;
    ++allocated_;
    ++allocationCount_;
    highWatermark_ = std::max(highWatermark_, allocated_);

    return BindlessHandle{
        static_cast<std::uint32_t>(static_cast<std::uint64_t>(logicalBaseIndex_) + slot),
        entry.generation,
        description_.type,
        id_,
        slot};
}

DescriptorOperationResult DescriptorPool::release(BindlessHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle.pool != id_ || handle.slot >= slots_.size()) return DescriptorOperationResult::InvalidHandle;
    if (handle.type != description_.type) return DescriptorOperationResult::TypeMismatch;

    const auto expectedIndex = static_cast<std::uint64_t>(logicalBaseIndex_) + handle.slot;
    if (expectedIndex > std::numeric_limits<std::uint32_t>::max() ||
        handle.index != static_cast<std::uint32_t>(expectedIndex) || handle.generation == 0) {
        return DescriptorOperationResult::InvalidHandle;
    }

    auto& entry = slots_[handle.slot];
    if (entry.generation != handle.generation) return DescriptorOperationResult::StaleHandle;
    if (!entry.allocated) return DescriptorOperationResult::AlreadyFree;

    entry.allocated = false;
    entry.generation = next_generation(entry.generation);
    freeSlots_.push_back(handle.slot);
    --allocated_;
    ++releaseCount_;
    return DescriptorOperationResult::Success;
}

bool DescriptorPool::validate(BindlessHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return matches_unlocked(handle);
}

std::optional<DescriptorLocation> DescriptorPool::resolve(BindlessHandle handle,
                                                          DescriptorPoolBackendHandle backendPool) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!matches_unlocked(handle)) return std::nullopt;
    return DescriptorLocation{id_, handle.slot, handle.index, description_.type, backendPool};
}

DescriptorPoolStats DescriptorPool::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return DescriptorPoolStats{
        id_, description_.type, slots_.size(), allocated_, slots_.size() - allocated_, highWatermark_,
        allocationCount_, releaseCount_};
}

DescriptorAllocator::DescriptorAllocator(DescriptorAllocatorConfig config)
    : config_(config) {
    if (config_.initialPoolCapacity == 0 || config_.growthFactor == 0 || config_.maxPoolCapacity == 0) {
        lastError_ = result_name(DescriptorOperationResult::InvalidConfig);
    }
    if (config_.initialPoolCapacity > config_.maxPoolCapacity) {
        lastError_ = result_name(DescriptorOperationResult::InvalidConfig);
    }
    counters_.budget = config_.descriptorBudget;
}

DescriptorAllocator::~DescriptorAllocator() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.backend) return;
    for (auto it = backendPools_.rbegin(); it != backendPools_.rend(); ++it) {
        if (*it) config_.backend->destroy_pool(*it);
    }
}

DescriptorPool* DescriptorAllocator::find_pool_unlocked(DescriptorPoolId id) noexcept {
    for (auto& pool : pools_) {
        if (pool->id() == id) return pool.get();
    }
    return nullptr;
}

const DescriptorPool* DescriptorAllocator::find_pool_unlocked(DescriptorPoolId id) const noexcept {
    for (const auto& pool : pools_) {
        if (pool->id() == id) return pool.get();
    }
    return nullptr;
}

std::uint32_t DescriptorAllocator::next_pool_capacity_unlocked(DescriptorType type) const noexcept {
    std::uint32_t previous = 0;
    for (const auto& pool : pools_) {
        if (pool->description().type == type) {
            previous = std::max(previous, pool->description().capacity);
        }
    }
    if (previous == 0) return std::min(config_.initialPoolCapacity, config_.maxPoolCapacity);
    const auto factor = std::max(1u, config_.growthFactor);
    const auto grown = static_cast<std::uint64_t>(previous) * factor;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        std::max<std::uint64_t>(1, grown), config_.maxPoolCapacity));
}

DescriptorPool* DescriptorAllocator::create_pool_unlocked(DescriptorType type, std::uint32_t poolCapacity) {
    if (poolCapacity == 0 || poolCapacity > config_.maxPoolCapacity) {
        set_error_unlocked(DescriptorOperationResult::InvalidConfig);
        return nullptr;
    }
    if (config_.maxPools != 0 && pools_.size() >= config_.maxPools) {
        ++counters_.poolLimitRejectCount;
        set_error_unlocked(DescriptorOperationResult::PoolLimitExceeded);
        return nullptr;
    }
    if (config_.descriptorBudget != 0 &&
        (counters_.capacity > config_.descriptorBudget ||
         poolCapacity > config_.descriptorBudget - counters_.capacity)) {
        ++counters_.budgetRejectCount;
        set_error_unlocked(DescriptorOperationResult::BudgetExceeded);
        return nullptr;
    }
    if (nextPoolId_ == 0 || nextLogicalIndex_ == 0 ||
        static_cast<std::uint64_t>(nextLogicalIndex_) + poolCapacity >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1ull) {
        set_error_unlocked(DescriptorOperationResult::OutOfCapacity);
        return nullptr;
    }

    const DescriptorPoolId poolId = nextPoolId_++;
    const auto logicalBaseIndex = nextLogicalIndex_;
    DescriptorPoolCreateInfo description{
        poolId, logicalBaseIndex, DescriptorPoolDesc{type, poolCapacity, config_.visibility}};
    DescriptorPoolBackendHandle backendHandle{};
    if (config_.backend && !config_.backend->create_pool(description, backendHandle)) {
        ++counters_.backendFailureCount;
        set_error_unlocked(DescriptorOperationResult::BackendFailure);
        return nullptr;
    }

    auto pool = std::make_unique<DescriptorPool>(poolId, logicalBaseIndex, description.description);
    auto* result = pool.get();
    pools_.push_back(std::move(pool));
    backendPools_.push_back(backendHandle);
    nextLogicalIndex_ += poolCapacity;
    counters_.capacity += poolCapacity;
    ++counters_.poolCount;
    if (counters_.poolCount > 1) ++counters_.growthCount;
    counters_.free += poolCapacity;
    set_error_unlocked(DescriptorOperationResult::Success);
    return result;
}

void DescriptorAllocator::set_error_unlocked(DescriptorOperationResult result) const {
    if (result == DescriptorOperationResult::Success) lastError_.clear();
    else lastError_ = result_name(result);
}

std::optional<BindlessHandle> DescriptorAllocator::allocate(DescriptorType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pool : pools_) {
        if (pool->description().type != type) continue;
        if (auto handle = pool->allocate()) {
            ++counters_.allocated;
            --counters_.free;
            ++counters_.allocationCount;
            counters_.highWatermark = std::max(counters_.highWatermark, counters_.allocated);
            set_error_unlocked(DescriptorOperationResult::Success);
            return handle;
        }
    }

    auto capacityToCreate = next_pool_capacity_unlocked(type);
    if (capacityToCreate == 0) {
        ++counters_.failedAllocationCount;
        set_error_unlocked(DescriptorOperationResult::InvalidConfig);
        return std::nullopt;
    }
    if (config_.descriptorBudget != 0) {
        const auto remaining = config_.descriptorBudget > counters_.capacity
            ? config_.descriptorBudget - counters_.capacity : 0;
        if (remaining == 0) {
            ++counters_.budgetRejectCount;
            ++counters_.failedAllocationCount;
            set_error_unlocked(DescriptorOperationResult::BudgetExceeded);
            return std::nullopt;
        }
        capacityToCreate = static_cast<std::uint32_t>(std::min<std::uint64_t>(capacityToCreate, remaining));
    }
    auto* pool = create_pool_unlocked(type, capacityToCreate);
    if (!pool) {
        ++counters_.failedAllocationCount;
        return std::nullopt;
    }
    auto handle = pool->allocate();
    if (!handle) {
        ++counters_.failedAllocationCount;
        set_error_unlocked(DescriptorOperationResult::OutOfCapacity);
        return std::nullopt;
    }
    ++counters_.allocated;
    --counters_.free;
    ++counters_.allocationCount;
    counters_.highWatermark = std::max(counters_.highWatermark, counters_.allocated);
    set_error_unlocked(DescriptorOperationResult::Success);
    return handle;
}

DescriptorOperationResult DescriptorAllocator::release(BindlessHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* pool = find_pool_unlocked(handle.pool);
    if (!pool) {
        ++counters_.invalidHandleCount;
        set_error_unlocked(DescriptorOperationResult::InvalidHandle);
        return DescriptorOperationResult::InvalidHandle;
    }
    const auto result = pool->release(handle);
    switch (result) {
    case DescriptorOperationResult::Success:
        --counters_.allocated;
        ++counters_.free;
        ++counters_.releaseCount;
        break;
    case DescriptorOperationResult::InvalidHandle: ++counters_.invalidHandleCount; break;
    case DescriptorOperationResult::StaleHandle: ++counters_.staleHandleCount; break;
    case DescriptorOperationResult::TypeMismatch: ++counters_.typeMismatchCount; break;
    default: break;
    }
    set_error_unlocked(result);
    return result;
}

bool DescriptorAllocator::validate(BindlessHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* pool = find_pool_unlocked(handle.pool);
    if (!pool) {
        ++counters_.invalidHandleCount;
        set_error_unlocked(DescriptorOperationResult::InvalidHandle);
        return false;
    }
    if (handle.type != pool->description().type) {
        ++counters_.typeMismatchCount;
        set_error_unlocked(DescriptorOperationResult::TypeMismatch);
        return false;
    }
    if (pool->validate(handle)) return true;
    ++counters_.staleHandleCount;
    set_error_unlocked(DescriptorOperationResult::StaleHandle);
    return false;
}

std::optional<DescriptorLocation> DescriptorAllocator::resolve(BindlessHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* pool = find_pool_unlocked(handle.pool);
    if (!pool) {
        ++counters_.invalidHandleCount;
        set_error_unlocked(DescriptorOperationResult::InvalidHandle);
        return std::nullopt;
    }
    std::size_t poolIndex = 0;
    for (; poolIndex < pools_.size(); ++poolIndex) {
        if (pools_[poolIndex]->id() == handle.pool) break;
    }
    const auto backendHandle = poolIndex < backendPools_.size() ? backendPools_[poolIndex] : DescriptorPoolBackendHandle{};
    if (handle.type != pool->description().type) {
        ++counters_.typeMismatchCount;
        set_error_unlocked(DescriptorOperationResult::TypeMismatch);
        return std::nullopt;
    }
    auto location = pool->resolve(handle, backendHandle);
    if (!location) {
        ++counters_.staleHandleCount;
        set_error_unlocked(DescriptorOperationResult::StaleHandle);
        return std::nullopt;
    }
    return location;
}

DescriptorAllocatorStats DescriptorAllocator::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counters_;
}

std::vector<DescriptorPoolStats> DescriptorAllocator::pool_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DescriptorPoolStats> result;
    result.reserve(pools_.size());
    for (const auto& pool : pools_) result.push_back(pool->stats());
    return result;
}

std::uint64_t DescriptorAllocator::capacity(DescriptorType type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t result = 0;
    for (const auto& pool : pools_) {
        if (pool->description().type == type) result += pool->description().capacity;
    }
    return result;
}

std::uint64_t DescriptorAllocator::allocated(DescriptorType type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t result = 0;
    for (const auto& pool : pools_) {
        if (pool->description().type == type) result += pool->stats().allocated;
    }
    return result;
}

std::string DescriptorAllocator::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

} // namespace shinkou::render
