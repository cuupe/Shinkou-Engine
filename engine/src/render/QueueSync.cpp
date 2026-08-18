#include "shinkou/render/QueueSync.h"

#include <algorithm>
#include <cstddef>

namespace shinkou::render {
namespace {

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

bool same_dependency(const QueueBatchDependency& lhs, const QueueBatchDependency& rhs) {
    return lhs.resource.id == rhs.resource.id && lhs.sourceQueue == rhs.sourceQueue &&
        lhs.destinationQueue == rhs.destinationQueue && lhs.producerBatch == rhs.producerBatch &&
        lhs.consumerBatch == rhs.consumerBatch;
}

bool same_ownership(const ResourceOwnershipTransfer& lhs, const ResourceOwnershipTransfer& rhs) {
    return lhs.resource.id == rhs.resource.id && lhs.sourceQueue == rhs.sourceQueue &&
        lhs.destinationQueue == rhs.destinationQueue && lhs.producerBatch == rhs.producerBatch &&
        lhs.consumerBatch == rhs.consumerBatch;
}

std::uint64_t image_access(ResourceUsage usage) {
    using namespace vulkan_barrier;
    if (usage == ResourceUsage::ShaderRead || usage == ResourceUsage::StorageRead) return AccessShaderRead;
    if (usage == ResourceUsage::ShaderWrite || usage == ResourceUsage::StorageWrite) return AccessShaderWrite;
    if (usage >= ResourceUsage::ColorAttachment0 && usage <= ResourceUsage::ColorAttachment7) return AccessColorAttachmentWrite;
    if (usage == ResourceUsage::DepthStencil) return AccessDepthStencilAttachmentWrite;
    if (usage == ResourceUsage::CopySource) return AccessTransferRead;
    if (usage == ResourceUsage::CopyDestination) return AccessTransferWrite;
    if (usage == ResourceUsage::Present) return AccessMemoryRead;
    return 0;
}

std::uint64_t buffer_access(ResourceUsage usage) {
    using namespace vulkan_barrier;
    if (usage == ResourceUsage::VertexBuffer) return AccessVertexAttributeRead;
    if (usage == ResourceUsage::IndexBuffer) return AccessIndexRead;
    if (usage == ResourceUsage::UniformBuffer) return AccessUniformRead;
    if (usage == ResourceUsage::IndirectArguments) return AccessIndirectCommandRead;
    return image_access(usage);
}

std::uint64_t image_stage(ResourceUsage usage) {
    using namespace vulkan_barrier;
    if (usage == ResourceUsage::ShaderRead || usage == ResourceUsage::ShaderWrite ||
        usage == ResourceUsage::StorageRead || usage == ResourceUsage::StorageWrite) return StageAllCommands;
    if (usage >= ResourceUsage::ColorAttachment0 && usage <= ResourceUsage::ColorAttachment7) return StageColorAttachmentOutput;
    if (usage == ResourceUsage::DepthStencil) return StageEarlyFragmentTests | StageLateFragmentTests;
    if (usage == ResourceUsage::CopySource || usage == ResourceUsage::CopyDestination) return StageTransfer;
    if (usage == ResourceUsage::Present) return StageBottomOfPipe;
    return StageTopOfPipe;
}

std::uint64_t buffer_stage(ResourceUsage usage) {
    using namespace vulkan_barrier;
    if (usage == ResourceUsage::VertexBuffer || usage == ResourceUsage::IndexBuffer) return StageVertexInput;
    if (usage == ResourceUsage::UniformBuffer) return StageAllCommands;
    if (usage == ResourceUsage::IndirectArguments) return StageDrawIndirect;
    return image_stage(usage);
}

vulkan_barrier::ImageLayout image_layout(ResourceUsage usage) {
    using namespace vulkan_barrier;
    if (usage == ResourceUsage::ShaderRead) return ImageLayout::ShaderReadOnlyOptimal;
    if (usage == ResourceUsage::ShaderWrite || usage == ResourceUsage::StorageRead || usage == ResourceUsage::StorageWrite) return ImageLayout::General;
    if (usage >= ResourceUsage::ColorAttachment0 && usage <= ResourceUsage::ColorAttachment7) return ImageLayout::ColorAttachmentOptimal;
    if (usage == ResourceUsage::DepthStencil) return ImageLayout::DepthStencilAttachmentOptimal;
    if (usage == ResourceUsage::CopySource) return ImageLayout::TransferSrcOptimal;
    if (usage == ResourceUsage::CopyDestination) return ImageLayout::TransferDstOptimal;
    if (usage == ResourceUsage::Present) return ImageLayout::PresentSrc;
    return ImageLayout::General;
}

ResourceOwnershipTransfer make_transfer(const QueueSyncRequest& request, QueueSyncPhase phase) {
    ResourceOwnershipTransfer transfer;
    transfer.resource = request.resource;
    transfer.sourceQueue = request.sourceQueue;
    transfer.destinationQueue = request.destinationQueue;
    transfer.before = request.before;
    transfer.after = request.after;
    transfer.producerBatch = request.producerBatch;
    transfer.consumerBatch = request.consumerBatch;
    transfer.phase = phase;
    transfer.release = phase == QueueSyncPhase::Release;
    transfer.acquire = phase == QueueSyncPhase::Acquire;
    return transfer;
}

QueueSyncPhase effective_phase(const ResourceOwnershipTransfer& transfer) {
    // An acquire created with the original API has phase's default value
    // (Release), so use the legacy flags when they clearly identify a phase.
    if (!transfer.release && transfer.acquire) return QueueSyncPhase::Acquire;
    if (transfer.release && !transfer.acquire) return QueueSyncPhase::Release;
    return transfer.phase;
}

} // namespace

std::uint32_t QueueFamilyMap::family(RenderQueue queue) const noexcept {
    switch (queue) {
    case RenderQueue::Compute: return compute;
    case RenderQueue::Copy: return transfer;
    case RenderQueue::Graphics: break;
    }
    return graphics;
}

std::uint32_t QueueFamilyMap::resolved_family(RenderQueue queue) const noexcept {
    const auto selected = family(queue);
    return selected == kIgnoredQueueFamily ? graphics : selected;
}

bool QueueFamilyMap::same_family(RenderQueue lhs, RenderQueue rhs) const noexcept {
    return resolved_family(lhs) == resolved_family(rhs);
}

QueueFamilyMap QueueFamilyMap::resolved() const noexcept {
    QueueFamilyMap result = *this;
    if (result.graphics == kIgnoredQueueFamily) result.graphics = 0;
    if (result.compute == kIgnoredQueueFamily) result.compute = result.graphics;
    if (result.transfer == kIgnoredQueueFamily) result.transfer = result.graphics;
    return result;
}

QueueSyncPlan QueueSyncPlanner::build(const std::vector<QueueSyncRequest>& requests, bool queueFamiliesDiffer) {
    QueueFamilyMap families;
    families.graphics = 0;
    families.compute = queueFamiliesDiffer ? 1u : 0u;
    families.transfer = queueFamiliesDiffer ? 2u : 0u;
    return build(requests, families);
}

QueueSyncPlan QueueSyncPlanner::build(const std::vector<QueueSyncRequest>& requests, const QueueFamilyMap& queueFamilies) {
    QueueSyncPlan plan;
    plan.queueFamilies = queueFamilies.resolved();
    plan.dependencies.reserve(requests.size());
    plan.transfers.reserve(requests.size() * 2);
    for (const auto& request : requests) {
        if (!request.resource) {
            plan.diagnostics.emplace_back("queue sync request has an invalid resource");
            continue;
        }
        if (request.sourceQueue == request.destinationQueue) continue;
        if (request.producerBatch == request.consumerBatch) {
            plan.diagnostics.emplace_back("cross-queue resource uses the same batch; dependency is redundant");
            continue;
        }
        const QueueBatchDependency dependency{request.resource, request.sourceQueue, request.destinationQueue,
            request.producerBatch, request.consumerBatch};
        if (std::none_of(plan.dependencies.begin(), plan.dependencies.end(),
                         [&](const auto& existing) { return same_dependency(existing, dependency); })) {
            plan.dependencies.push_back(dependency);
        }
        plan.requiresQueueSync = true;
        if (plan.queueFamilies.same_family(request.sourceQueue, request.destinationQueue)) continue;
        plan.requiresOwnershipTransfer = true;
        const auto release = make_transfer(request, QueueSyncPhase::Release);
        const auto acquire = make_transfer(request, QueueSyncPhase::Acquire);
        if (std::none_of(plan.transfers.begin(), plan.transfers.end(),
                         [&](const auto& existing) { return same_ownership(existing, release) && existing.phase == release.phase; })) {
            plan.transfers.push_back(release);
            plan.transfers.push_back(acquire);
        }
    }
    std::sort(plan.dependencies.begin(), plan.dependencies.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.consumerBatch != rhs.consumerBatch) return lhs.consumerBatch < rhs.consumerBatch;
        if (lhs.producerBatch != rhs.producerBatch) return lhs.producerBatch < rhs.producerBatch;
        if (lhs.resource.id != rhs.resource.id) return lhs.resource.id < rhs.resource.id;
        return static_cast<std::uint32_t>(lhs.destinationQueue) < static_cast<std::uint32_t>(rhs.destinationQueue);
    });
    std::sort(plan.transfers.begin(), plan.transfers.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.producerBatch != rhs.producerBatch) return lhs.producerBatch < rhs.producerBatch;
        if (lhs.consumerBatch != rhs.consumerBatch) return lhs.consumerBatch < rhs.consumerBatch;
        if (lhs.resource.id != rhs.resource.id) return lhs.resource.id < rhs.resource.id;
        return lhs.phase == QueueSyncPhase::Release && rhs.phase == QueueSyncPhase::Acquire;
    });
    return plan;
}

bool QueueSyncPlanner::validate(const QueueSyncPlan& plan, std::string* error) {
    for (const auto& dependency : plan.dependencies) {
        if (!dependency.resource) return fail(error, "queue dependency has an invalid resource");
        if (dependency.sourceQueue == dependency.destinationQueue) return fail(error, "queue dependency has identical queues");
        if (dependency.producerBatch == dependency.consumerBatch) return fail(error, "queue dependency has identical batches");
    }
    for (std::size_t i = 0; i < plan.transfers.size(); ++i) {
        const auto& transfer = plan.transfers[i];
        if (!transfer.resource) return fail(error, "queue transfer has an invalid resource");
        if (transfer.sourceQueue == transfer.destinationQueue) return fail(error, "queue transfer has identical queues");
        if (transfer.producerBatch == transfer.consumerBatch) return fail(error, "queue transfer has identical batches");
        if (transfer.release == transfer.acquire) return fail(error, "queue transfer phase flags are inconsistent");
        const auto phase = effective_phase(transfer);
        if (phase != QueueSyncPhase::Release && phase != QueueSyncPhase::Acquire) return fail(error, "queue transfer has an invalid phase");
        if (phase != QueueSyncPhase::Acquire) continue;
        const auto release = std::find_if(plan.transfers.begin(), plan.transfers.begin() + static_cast<std::ptrdiff_t>(i),
            [&](const auto& candidate) {
                return effective_phase(candidate) == QueueSyncPhase::Release && same_ownership(candidate, transfer);
            });
        if (release == plan.transfers.begin() + static_cast<std::ptrdiff_t>(i)) {
            return fail(error, "queue acquire has no matching release");
        }
    }
    return true;
}

VulkanQueueSyncPlan VulkanQueueSyncPlanner::build(const std::vector<QueueSyncRequest>& requests,
                                                  const QueueFamilyMap& queueFamilies) {
    VulkanQueueSyncPlan result;
    result.logical = QueueSyncPlanner::build(requests, queueFamilies);
    for (const auto& ownership : result.logical.transfers) {
        VulkanQueueFamilyOwnershipTransfer barrier;
        barrier.ownership = ownership;
        barrier.resourceType = ownership.resource.kind == ResourceKind::Buffer ? VulkanBarrierResource::Buffer : VulkanBarrierResource::Image;
        barrier.srcQueueFamilyIndex = result.logical.queueFamilies.resolved_family(ownership.sourceQueue);
        barrier.dstQueueFamilyIndex = result.logical.queueFamilies.resolved_family(ownership.destinationQueue);
        const bool buffer = barrier.resourceType == VulkanBarrierResource::Buffer;
        barrier.srcAccessMask = ownership.phase == QueueSyncPhase::Release ? (buffer ? buffer_access(ownership.before) : image_access(ownership.before)) : 0;
        barrier.dstAccessMask = ownership.phase == QueueSyncPhase::Acquire ? (buffer ? buffer_access(ownership.after) : image_access(ownership.after)) : 0;
        barrier.srcStageMask = ownership.phase == QueueSyncPhase::Release ? (buffer ? buffer_stage(ownership.before) : image_stage(ownership.before)) : vulkan_barrier::StageTopOfPipe;
        barrier.dstStageMask = ownership.phase == QueueSyncPhase::Acquire ? (buffer ? buffer_stage(ownership.after) : image_stage(ownership.after)) : vulkan_barrier::StageBottomOfPipe;
        if (!buffer) {
            // The release performs the layout transition.  The acquire half
            // must observe the already-transitioned layout and only carries
            // ownership/memory visibility to the destination queue.
            barrier.oldLayout = ownership.phase == QueueSyncPhase::Acquire
                ? image_layout(ownership.after) : image_layout(ownership.before);
            barrier.newLayout = image_layout(ownership.after);
        }
        result.barriers.push_back(barrier);
        if (ownership.phase == QueueSyncPhase::Release) result.releaseBarriers.push_back(barrier);
        else result.acquireBarriers.push_back(barrier);
    }
    return result;
}

bool VulkanQueueSyncPlanner::validate(const VulkanQueueSyncPlan& plan, std::string* error) {
    if (!QueueSyncPlanner::validate(plan.logical, error)) return false;
    if (plan.releaseBarriers.size() != plan.acquireBarriers.size() ||
        plan.barriers.size() != plan.releaseBarriers.size() + plan.acquireBarriers.size()) {
        return fail(error, "Vulkan ownership barrier phase counts do not match");
    }
    for (const auto& barrier : plan.barriers) {
        if (barrier.srcQueueFamilyIndex == kIgnoredQueueFamily || barrier.dstQueueFamilyIndex == kIgnoredQueueFamily) return fail(error, "Vulkan ownership barrier has an unresolved queue family");
        if (barrier.srcQueueFamilyIndex == barrier.dstQueueFamilyIndex) return fail(error, "Vulkan ownership barrier has identical queue families");
        if (barrier.ownership.phase == QueueSyncPhase::Release && (barrier.dstAccessMask != 0 || barrier.dstStageMask != vulkan_barrier::StageBottomOfPipe)) return fail(error, "Vulkan release barrier has acquire synchronization state");
        if (barrier.ownership.phase == QueueSyncPhase::Acquire && (barrier.srcAccessMask != 0 || barrier.srcStageMask != vulkan_barrier::StageTopOfPipe)) return fail(error, "Vulkan acquire barrier has release synchronization state");
    }
    return true;
}

} // namespace shinkou::render
