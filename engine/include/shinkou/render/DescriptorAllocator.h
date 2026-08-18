#pragma once

#include "shinkou/render/RenderTypes.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace shinkou::render {

// A logical descriptor handle. The index identifies a stable logical slot;
// generation makes a released handle unusable; type prevents accidental use
// of a slot from a different descriptor class. pool and slot are intentionally
// exposed so a backend adapter can resolve a handle without knowing allocator
// internals. They are not native D3D/Vulkan handles.
struct BindlessHandle {
    std::uint32_t index{0};
    std::uint32_t generation{0};
    DescriptorType type{DescriptorType::Texture};
    std::uint32_t pool{0};
    std::uint32_t slot{0};

    explicit operator bool() const noexcept { return index != 0 && generation != 0 && pool != 0; }
    friend bool operator==(const BindlessHandle& lhs, const BindlessHandle& rhs) noexcept {
        return lhs.index == rhs.index && lhs.generation == rhs.generation && lhs.type == rhs.type &&
            lhs.pool == rhs.pool && lhs.slot == rhs.slot;
    }
    friend bool operator!=(const BindlessHandle& lhs, const BindlessHandle& rhs) noexcept {
        return !(lhs == rhs);
    }
};

using DescriptorHandle = BindlessHandle;
using DescriptorPoolId = std::uint32_t;

enum class DescriptorPoolVisibility {
    CpuOnly,
    ShaderVisible,
    CpuAndShaderVisible
};

enum class DescriptorOperationResult {
    Success,
    InvalidConfig,
    InvalidHandle,
    StaleHandle,
    TypeMismatch,
    AlreadyFree,
    OutOfCapacity,
    BudgetExceeded,
    PoolLimitExceeded,
    BackendFailure
};

struct DescriptorPoolDesc {
    DescriptorType type{DescriptorType::Texture};
    std::uint32_t capacity{0};
    DescriptorPoolVisibility visibility{DescriptorPoolVisibility::ShaderVisible};
};

// This is the only native-facing value in the allocator API. A D3D12 adapter
// may use it to identify a heap/range, a Vulkan adapter may use it for a
// descriptor set, and a D3D11 adapter may use it as a logical emulation id.
// The allocator never interprets the value.
struct DescriptorPoolBackendHandle {
    std::uint64_t value{0};
    explicit operator bool() const noexcept { return value != 0; }
};

struct DescriptorPoolCreateInfo {
    DescriptorPoolId pool{0};
    std::uint32_t logicalBaseIndex{0};
    DescriptorPoolDesc description{};
};

// Optional backend hook. Calls are made while DescriptorAllocator's mutex is
// held. Implementations must be thread-safe and must not call back into the
// allocator. The hook is intentionally limited to pool lifetime: descriptor
// writes remain a renderer/backend concern because D3D12, Vulkan and D3D11
// expose different write and retirement semantics.
class IDescriptorPoolBackend {
public:
    virtual ~IDescriptorPoolBackend() = default;
    virtual bool create_pool(const DescriptorPoolCreateInfo& description,
                             DescriptorPoolBackendHandle& backendHandle) = 0;
    virtual void destroy_pool(DescriptorPoolBackendHandle backendHandle) noexcept = 0;
};

struct DescriptorLocation {
    DescriptorPoolId pool{0};
    std::uint32_t slot{0};
    std::uint32_t logicalIndex{0};
    DescriptorType type{DescriptorType::Texture};
    DescriptorPoolBackendHandle backendPool{};

    explicit operator bool() const noexcept { return pool != 0; }
};

struct DescriptorPoolStats {
    DescriptorPoolId pool{0};
    DescriptorType type{DescriptorType::Texture};
    std::uint64_t capacity{0};
    std::uint64_t allocated{0};
    std::uint64_t free{0};
    std::uint64_t highWatermark{0};
    std::uint64_t allocationCount{0};
    std::uint64_t releaseCount{0};
};

struct DescriptorAllocatorConfig {
    std::uint32_t initialPoolCapacity{256};
    std::uint32_t growthFactor{2};
    std::uint32_t maxPoolCapacity{65536};
    std::uint32_t maxPools{0}; // zero means unlimited, subject to the budget.
    std::uint64_t descriptorBudget{0}; // zero means unlimited reserved capacity.
    DescriptorPoolVisibility visibility{DescriptorPoolVisibility::ShaderVisible};
    IDescriptorPoolBackend* backend{nullptr}; // non-owning; must outlive allocator.
};

struct DescriptorAllocatorStats {
    std::uint64_t capacity{0};
    std::uint64_t allocated{0};
    std::uint64_t free{0};
    std::uint64_t highWatermark{0};
    std::uint64_t budget{0};
    std::uint32_t poolCount{0};
    std::uint64_t growthCount{0};
    std::uint64_t allocationCount{0};
    std::uint64_t releaseCount{0};
    std::uint64_t failedAllocationCount{0};
    std::uint64_t budgetRejectCount{0};
    std::uint64_t poolLimitRejectCount{0};
    std::uint64_t invalidHandleCount{0};
    std::uint64_t staleHandleCount{0};
    std::uint64_t typeMismatchCount{0};
    std::uint64_t backendFailureCount{0};
};

// A fixed-size pool. DescriptorAllocator owns and grows a set of these pools;
// a pool never moves its slots or changes capacity after construction.
class DescriptorPool final {
public:
    DescriptorPool(DescriptorPoolId id, std::uint32_t logicalBaseIndex, DescriptorPoolDesc description);
    ~DescriptorPool() = default;

    DescriptorPool(const DescriptorPool&) = delete;
    DescriptorPool& operator=(const DescriptorPool&) = delete;

    std::optional<BindlessHandle> allocate();
    DescriptorOperationResult release(BindlessHandle handle);
    bool validate(BindlessHandle handle) const;
    std::optional<DescriptorLocation> resolve(BindlessHandle handle,
                                              DescriptorPoolBackendHandle backendPool = {}) const;
    DescriptorPoolStats stats() const;
    DescriptorPoolId id() const noexcept { return id_; }
    const DescriptorPoolDesc& description() const noexcept { return description_; }
    std::uint32_t logical_base_index() const noexcept { return logicalBaseIndex_; }

private:
    struct Slot {
        std::uint32_t generation{1};
        bool allocated{false};
    };

    bool matches_unlocked(BindlessHandle handle) const noexcept;
    static std::uint32_t next_generation(std::uint32_t generation) noexcept;

    const DescriptorPoolId id_;
    const std::uint32_t logicalBaseIndex_;
    const DescriptorPoolDesc description_;
    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> freeSlots_;
    std::uint64_t allocated_{0};
    std::uint64_t highWatermark_{0};
    std::uint64_t allocationCount_{0};
    std::uint64_t releaseCount_{0};
};

// Thread-safe, grow-only descriptor allocator. Growth adds a new pool, so a
// previously returned BindlessHandle and its DescriptorLocation remain stable.
// All methods are safe to call concurrently. Backend callbacks are serialized
// by the allocator mutex and must not re-enter this object.
class DescriptorAllocator final {
public:
    explicit DescriptorAllocator(DescriptorAllocatorConfig config = {});
    ~DescriptorAllocator();

    DescriptorAllocator(const DescriptorAllocator&) = delete;
    DescriptorAllocator& operator=(const DescriptorAllocator&) = delete;

    std::optional<BindlessHandle> allocate(DescriptorType type);
    DescriptorOperationResult release(BindlessHandle handle);
    bool validate(BindlessHandle handle) const;
    std::optional<DescriptorLocation> resolve(BindlessHandle handle) const;

    DescriptorAllocatorStats stats() const;
    std::vector<DescriptorPoolStats> pool_stats() const;
    std::uint64_t capacity(DescriptorType type) const;
    std::uint64_t allocated(DescriptorType type) const;
    std::string last_error() const;

private:
    DescriptorPool* find_pool_unlocked(DescriptorPoolId id) noexcept;
    const DescriptorPool* find_pool_unlocked(DescriptorPoolId id) const noexcept;
    std::uint32_t next_pool_capacity_unlocked(DescriptorType type) const noexcept;
    DescriptorPool* create_pool_unlocked(DescriptorType type, std::uint32_t capacity);
    void set_error_unlocked(DescriptorOperationResult result) const;

    const DescriptorAllocatorConfig config_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<DescriptorPool>> pools_;
    std::vector<DescriptorPoolBackendHandle> backendPools_;
    mutable std::string lastError_;
    mutable DescriptorAllocatorStats counters_{};
    DescriptorPoolId nextPoolId_{1};
    std::uint32_t nextLogicalIndex_{1};
};

} // namespace shinkou::render
