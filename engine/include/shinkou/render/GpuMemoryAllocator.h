#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace shinkou::render {

using GpuMemoryType = std::uint32_t;

enum class GpuMemoryResourceType : std::uint8_t { Buffer, Texture, DepthStencil };
enum class GpuMemoryLifetime : std::uint8_t { Persistent, Transient, Frame };

struct GpuAllocation {
    std::uint64_t id{0};
    std::uint64_t blockId{0};
    std::size_t offset{0};
    std::size_t size{0};
    std::size_t alignment{1};
    GpuMemoryType memoryType{0};
    GpuMemoryLifetime lifetime{GpuMemoryLifetime::Persistent};
    explicit operator bool() const noexcept { return id != 0; }
};

inline bool operator==(const GpuAllocation& lhs, const GpuAllocation& rhs) noexcept {
    return lhs.id == rhs.id && lhs.blockId == rhs.blockId && lhs.offset == rhs.offset &&
        lhs.size == rhs.size && lhs.alignment == rhs.alignment && lhs.memoryType == rhs.memoryType &&
        lhs.lifetime == rhs.lifetime;
}
inline bool operator!=(const GpuAllocation& lhs, const GpuAllocation& rhs) noexcept { return !(lhs == rhs); }

struct GpuMemoryAllocatorConfig {
    std::size_t blockSize{64u * 1024u * 1024u};
    std::size_t budgetBytes{std::numeric_limits<std::size_t>::max()};
};

struct GpuMemoryStats {
    std::size_t budgetBytes{std::numeric_limits<std::size_t>::max()};
    std::size_t committedBytes{0};
    std::size_t committedPeakBytes{0};
    std::size_t liveBytes{0};
    std::size_t livePeakBytes{0};
    std::size_t freeBytes{0};
    std::size_t blockCount{0};
    std::size_t liveAllocationCount{0};
    std::uint64_t allocationCount{0};
    std::uint64_t releaseCount{0};
    std::uint64_t freeRangeReuseCount{0};
    std::uint64_t failedAllocationCount{0};
};

struct GpuAliasRequest {
    std::uint64_t resourceId{0};
    GpuMemoryResourceType resourceType{GpuMemoryResourceType::Buffer};
    std::size_t size{0};
    std::size_t alignment{1};
    GpuMemoryLifetime lifetime{GpuMemoryLifetime::Transient};
    std::size_t firstUse{0};
    std::size_t lastUse{0};
    std::size_t firstPass{0};
    std::size_t lastPass{0};
    std::uint64_t compatibilityKey{0};
};

struct GpuAliasAssignment {
    std::uint64_t resourceId{0};
    std::uint64_t physicalResourceId{0};
    GpuMemoryResourceType resourceType{GpuMemoryResourceType::Buffer};
    std::size_t offset{0};
    std::size_t size{0};
    std::size_t alignment{1};
    bool aliased{false};
};

struct GpuAliasPlanOptions {
    std::function<bool(const GpuAliasRequest&, const GpuAliasRequest&)> canAlias;
};

class GpuAliasPlan {
    std::vector<GpuAliasAssignment> assignments_;
public:
    GpuAliasPlan() = default;
    explicit GpuAliasPlan(std::vector<GpuAliasAssignment> assignments)
        : assignments_(std::move(assignments)) {}
    const std::vector<GpuAliasAssignment>& assignments() const noexcept { return assignments_; }
    const GpuAliasAssignment* assignment(std::uint64_t logicalResource) const noexcept;
    std::uint64_t physical_resource(std::uint64_t logicalResource) const noexcept;
};

struct TransientResourceRequest {
    std::uint64_t resourceId{0};
    std::size_t size{0};
    std::size_t alignment{1};
    GpuMemoryType memoryType{0};
    std::size_t firstUse{0};
    std::size_t lastUse{0};
    std::size_t firstPass{0};
    std::size_t lastPass{0};
};

struct TransientAliasPlanOptions {
    std::function<bool(const TransientResourceRequest&, const TransientResourceRequest&)> canAlias;
};

struct TransientAliasAssignment {
    std::uint64_t resourceId{0};
    std::size_t slotIndex{0};
    GpuAllocation allocation{};
};

struct TransientAliasSlot {
    std::size_t size{0};
    std::size_t alignment{1};
    GpuMemoryType memoryType{0};
    std::size_t lastUse{0};
    GpuAllocation allocation{};
    std::uint64_t physicalResourceId{0};
};

struct TransientAliasPlan {
    std::vector<TransientAliasSlot> slots;
    std::vector<TransientAliasAssignment> assignments;
    std::size_t requiredBytes{0};
    std::size_t peakBytes{0};
    std::size_t aliasCount{0};
    bool valid{true};
    bool materialized{false};
    std::string error;
    std::uint64_t physical_resource(std::uint64_t logicalResource) const noexcept;
};

class GpuMemoryAllocator {
public:
    struct Impl;
    explicit GpuMemoryAllocator(GpuMemoryAllocatorConfig config = {});
    ~GpuMemoryAllocator();
    GpuMemoryAllocator(const GpuMemoryAllocator&) = delete;
    GpuMemoryAllocator& operator=(const GpuMemoryAllocator&) = delete;
    GpuMemoryAllocator(GpuMemoryAllocator&&) noexcept;
    GpuMemoryAllocator& operator=(GpuMemoryAllocator&&) noexcept;

    GpuAllocation allocate(std::size_t size, std::size_t alignment = 1,
                           GpuMemoryType memoryType = 0,
                           GpuMemoryLifetime lifetime = GpuMemoryLifetime::Persistent) noexcept;
    bool release(GpuAllocation allocation) noexcept;
    bool owns(GpuAllocation allocation) const noexcept;
    std::size_t block_capacity(std::uint64_t blockId) const noexcept;
    std::size_t reset(GpuMemoryLifetime lifetime) noexcept;
    void set_default_budget(std::size_t budgetBytes) noexcept;
    std::size_t default_budget() const noexcept;
    void set_budget(GpuMemoryType memoryType, std::size_t budgetBytes) noexcept;
    void clear_budget(GpuMemoryType memoryType) noexcept;
    std::size_t budget(GpuMemoryType memoryType = 0) const noexcept;
    GpuMemoryStats stats() const noexcept;
    GpuMemoryStats stats(GpuMemoryType memoryType) const noexcept;
    std::size_t trim() noexcept;
    GpuAliasPlan plan_aliases(const std::vector<GpuAliasRequest>& requests,
                              const GpuAliasPlanOptions& options = {}) const;
    TransientAliasPlan plan_transient_aliases(
        const std::vector<TransientResourceRequest>& requests,
        const TransientAliasPlanOptions& options = {}) const;
    bool materialize_transient_alias_plan(TransientAliasPlan& plan) noexcept;
    void release_transient_alias_plan(TransientAliasPlan& plan) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

using GpuAllocator = GpuMemoryAllocator;
} // namespace shinkou::render
