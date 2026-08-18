#pragma once

#include "shinkou/render/RenderTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace shinkou::render {

// Vulkan uses UINT32_MAX for VK_QUEUE_FAMILY_IGNORED. Keeping the value here
// makes queue-family planning testable without Vulkan SDK headers or a device.
inline constexpr std::uint32_t kIgnoredQueueFamily = UINT32_MAX;

enum class QueueSyncPhase : std::uint8_t {
    Release,
    Acquire,
};

// RenderQueue::Copy is the transfer queue in Vulkan terminology.
struct QueueFamilyMap {
    std::uint32_t graphics{kIgnoredQueueFamily};
    std::uint32_t compute{kIgnoredQueueFamily};
    std::uint32_t transfer{kIgnoredQueueFamily};

    std::uint32_t family(RenderQueue queue) const noexcept;
    std::uint32_t resolved_family(RenderQueue queue) const noexcept;
    bool same_family(RenderQueue lhs, RenderQueue rhs) const noexcept;
    QueueFamilyMap resolved() const noexcept;
};

struct QueueBatchDependency {
    ResourceHandle resource{};
    RenderQueue sourceQueue{RenderQueue::Graphics};
    RenderQueue destinationQueue{RenderQueue::Graphics};
    std::uint32_t producerBatch{0};
    std::uint32_t consumerBatch{0};
};

// Backend-neutral description of a resource hand-off. Vulkan turns this into
// two barriers (release on the producer queue and acquire on the consumer
// queue). Other backends can use the same record for a fence or event.
struct ResourceOwnershipTransfer {
    ResourceHandle resource{};
    RenderQueue sourceQueue{RenderQueue::Graphics};
    RenderQueue destinationQueue{RenderQueue::Graphics};
    ResourceUsage before{ResourceUsage::Unknown};
    ResourceUsage after{ResourceUsage::Unknown};
    std::uint32_t producerBatch{0};
    std::uint32_t consumerBatch{0};
    // Source compatibility with the initial QueueSync API. New code should
    // use phase; the planner keeps these flags consistent with it.
    bool release{false};
    bool acquire{false};
    QueueSyncPhase phase{QueueSyncPhase::Release};
};

using QueueOwnershipTransfer = ResourceOwnershipTransfer;

struct QueueSyncRequest {
    ResourceHandle resource{};
    RenderQueue sourceQueue{RenderQueue::Graphics};
    RenderQueue destinationQueue{RenderQueue::Graphics};
    ResourceUsage before{ResourceUsage::Unknown};
    ResourceUsage after{ResourceUsage::Unknown};
    std::uint32_t producerBatch{0};
    std::uint32_t consumerBatch{0};
};

struct QueueSyncPlan {
    std::vector<ResourceOwnershipTransfer> transfers;
    std::vector<std::string> diagnostics;
    bool requiresOwnershipTransfer{false};
    // Additive fields kept after the original API members for aggregate and
    // source compatibility with the first QueueSync implementation.
    std::vector<QueueBatchDependency> dependencies;
    QueueFamilyMap queueFamilies{};
    bool requiresQueueSync{false};
};

class QueueSyncPlanner {
public:
    // Compatibility overload: true models distinct families for logical
    // queues, false models all queues sharing one family.
    static QueueSyncPlan build(const std::vector<QueueSyncRequest>& requests,
                               bool queueFamiliesDiffer);
    static QueueSyncPlan build(const std::vector<QueueSyncRequest>& requests,
                               const QueueFamilyMap& queueFamilies);
    static bool validate(const QueueSyncPlan& plan, std::string* error = nullptr);
};

// These values mirror legacy Vulkan 1.x barrier bits. Integer masks keep the
// pure planner independent from vulkan.h; a backend can cast them to Vulkan
// flags at its integration boundary.
namespace vulkan_barrier {
inline constexpr std::uint64_t StageTopOfPipe = 0x00000001ull;
inline constexpr std::uint64_t StageDrawIndirect = 0x00000002ull;
inline constexpr std::uint64_t StageVertexInput = 0x00000004ull;
inline constexpr std::uint64_t StageVertexShader = 0x00000008ull;
inline constexpr std::uint64_t StageFragmentShader = 0x00000080ull;
inline constexpr std::uint64_t StageEarlyFragmentTests = 0x00000100ull;
inline constexpr std::uint64_t StageLateFragmentTests = 0x00000200ull;
inline constexpr std::uint64_t StageColorAttachmentOutput = 0x00000400ull;
inline constexpr std::uint64_t StageComputeShader = 0x00000800ull;
inline constexpr std::uint64_t StageTransfer = 0x00001000ull;
inline constexpr std::uint64_t StageBottomOfPipe = 0x00002000ull;
inline constexpr std::uint64_t StageAllCommands = 0x00010000ull;

inline constexpr std::uint64_t AccessIndirectCommandRead = 0x00000001ull;
inline constexpr std::uint64_t AccessIndexRead = 0x00000002ull;
inline constexpr std::uint64_t AccessVertexAttributeRead = 0x00000004ull;
inline constexpr std::uint64_t AccessUniformRead = 0x00000008ull;
inline constexpr std::uint64_t AccessShaderRead = 0x00000020ull;
inline constexpr std::uint64_t AccessShaderWrite = 0x00000040ull;
inline constexpr std::uint64_t AccessColorAttachmentWrite = 0x00000100ull;
inline constexpr std::uint64_t AccessDepthStencilAttachmentWrite = 0x00000400ull;
inline constexpr std::uint64_t AccessTransferRead = 0x00000800ull;
inline constexpr std::uint64_t AccessTransferWrite = 0x00001000ull;
inline constexpr std::uint64_t AccessMemoryRead = 0x00002000ull;

enum class ImageLayout : std::int32_t {
    Undefined = 0,
    General = 1,
    ColorAttachmentOptimal = 2,
    DepthStencilAttachmentOptimal = 3,
    ShaderReadOnlyOptimal = 5,
    TransferSrcOptimal = 6,
    TransferDstOptimal = 7,
    PresentSrc = 1000001002,
};
} // namespace vulkan_barrier

enum class VulkanBarrierResource : std::uint8_t {
    Buffer,
    Image,
};

struct VulkanQueueFamilyOwnershipTransfer {
    ResourceOwnershipTransfer ownership{};
    VulkanBarrierResource resourceType{VulkanBarrierResource::Buffer};
    std::uint32_t srcQueueFamilyIndex{kIgnoredQueueFamily};
    std::uint32_t dstQueueFamilyIndex{kIgnoredQueueFamily};
    std::uint64_t srcStageMask{vulkan_barrier::StageTopOfPipe};
    std::uint64_t dstStageMask{vulkan_barrier::StageTopOfPipe};
    std::uint64_t srcAccessMask{0};
    std::uint64_t dstAccessMask{0};
    vulkan_barrier::ImageLayout oldLayout{vulkan_barrier::ImageLayout::Undefined};
    vulkan_barrier::ImageLayout newLayout{vulkan_barrier::ImageLayout::Undefined};
};

struct VulkanQueueSyncPlan {
    QueueSyncPlan logical{};
    std::vector<VulkanQueueFamilyOwnershipTransfer> barriers;
    std::vector<VulkanQueueFamilyOwnershipTransfer> releaseBarriers;
    std::vector<VulkanQueueFamilyOwnershipTransfer> acquireBarriers;

    bool requiresQueueSync() const noexcept { return logical.requiresQueueSync; }
    bool requiresOwnershipTransfer() const noexcept { return logical.requiresOwnershipTransfer; }
};

class VulkanQueueSyncPlanner {
public:
    static VulkanQueueSyncPlan build(const std::vector<QueueSyncRequest>& requests,
                                     const QueueFamilyMap& queueFamilies);
    static bool validate(const VulkanQueueSyncPlan& plan, std::string* error = nullptr);
};

} // namespace shinkou::render
