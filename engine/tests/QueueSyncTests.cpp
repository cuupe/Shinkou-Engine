#include "shinkou/render/QueueSync.h"

#include <cassert>
#include <string>

using namespace shinkou::render;

namespace {
QueueSyncRequest request(ResourceHandle resource, RenderQueue source, RenderQueue destination,
                         ResourceUsage before, ResourceUsage after, std::uint32_t producer, std::uint32_t consumer) {
    return {resource, source, destination, before, after, producer, consumer};
}
}

int main() {
    const ResourceHandle image{42, ResourceKind::Texture2D};
    const ResourceHandle buffer{43, ResourceKind::Buffer};

    QueueFamilyMap dedicated{3, 5, 7};
    assert(dedicated.family(RenderQueue::Graphics) == 3);
    assert(dedicated.family(RenderQueue::Compute) == 5);
    assert(dedicated.family(RenderQueue::Copy) == 7);
    assert(!dedicated.same_family(RenderQueue::Graphics, RenderQueue::Compute));

    const auto graphicsToCompute = request(image, RenderQueue::Graphics, RenderQueue::Compute,
        ResourceUsage::ColorAttachment, ResourceUsage::ShaderRead, 1, 3);
    const auto plan = QueueSyncPlanner::build({graphicsToCompute}, dedicated);
    assert(plan.requiresQueueSync);
    assert(plan.requiresOwnershipTransfer);
    assert(plan.dependencies.size() == 1);
    assert(plan.transfers.size() == 2);
    assert(plan.transfers[0].phase == QueueSyncPhase::Release);
    assert(plan.transfers[0].release && !plan.transfers[0].acquire);
    assert(plan.transfers[1].phase == QueueSyncPhase::Acquire);
    assert(!plan.transfers[1].release && plan.transfers[1].acquire);
    assert(plan.transfers[0].after == ResourceUsage::ShaderRead);
    std::string error;
    assert(QueueSyncPlanner::validate(plan, &error));

    const auto vulkanPlan = VulkanQueueSyncPlanner::build({graphicsToCompute}, dedicated);
    assert(vulkanPlan.requiresQueueSync());
    assert(vulkanPlan.requiresOwnershipTransfer());
    assert(vulkanPlan.barriers.size() == 2);
    assert(vulkanPlan.releaseBarriers.size() == 1);
    assert(vulkanPlan.acquireBarriers.size() == 1);
    assert(vulkanPlan.releaseBarriers.front().srcQueueFamilyIndex == 3);
    assert(vulkanPlan.releaseBarriers.front().dstQueueFamilyIndex == 5);
    assert(vulkanPlan.releaseBarriers.front().srcAccessMask == vulkan_barrier::AccessColorAttachmentWrite);
    assert(vulkanPlan.releaseBarriers.front().dstAccessMask == 0);
    assert(vulkanPlan.acquireBarriers.front().srcAccessMask == 0);
    assert(vulkanPlan.acquireBarriers.front().dstAccessMask == vulkan_barrier::AccessShaderRead);
    assert(vulkanPlan.releaseBarriers.front().oldLayout == vulkan_barrier::ImageLayout::ColorAttachmentOptimal);
    assert(vulkanPlan.acquireBarriers.front().oldLayout == vulkan_barrier::ImageLayout::ShaderReadOnlyOptimal);
    assert(vulkanPlan.acquireBarriers.front().newLayout == vulkan_barrier::ImageLayout::ShaderReadOnlyOptimal);
    assert(VulkanQueueSyncPlanner::validate(vulkanPlan, &error));

    // A dedicated compute queue may be unavailable. The logical dependency is
    // still needed, but an exclusive-resource ownership transfer is not.
    const QueueFamilyMap collapsed{3, kIgnoredQueueFamily, kIgnoredQueueFamily};
    const auto sameFamily = QueueSyncPlanner::build({graphicsToCompute}, collapsed);
    assert(sameFamily.requiresQueueSync);
    assert(!sameFamily.requiresOwnershipTransfer);
    assert(sameFamily.dependencies.size() == 1);
    assert(sameFamily.transfers.empty());
    const auto collapsedVulkan = VulkanQueueSyncPlanner::build({graphicsToCompute}, collapsed);
    assert(collapsedVulkan.barriers.empty());
    assert(VulkanQueueSyncPlanner::validate(collapsedVulkan, &error));

    // Exercise transfer queue and buffer-specific access/stage mapping.
    const auto computeToTransfer = request(buffer, RenderQueue::Compute, RenderQueue::Copy,
        ResourceUsage::ShaderWrite, ResourceUsage::CopySource, 4, 8);
    const auto transferPlan = VulkanQueueSyncPlanner::build({computeToTransfer}, QueueFamilyMap{3, 5, 7});
    assert(transferPlan.barriers.size() == 2);
    assert(transferPlan.releaseBarriers.front().resourceType == VulkanBarrierResource::Buffer);
    assert(transferPlan.releaseBarriers.front().srcQueueFamilyIndex == 5);
    assert(transferPlan.acquireBarriers.front().dstQueueFamilyIndex == 7);
    assert(transferPlan.acquireBarriers.front().dstAccessMask == vulkan_barrier::AccessTransferRead);
    assert(VulkanQueueSyncPlanner::validate(transferPlan, &error));

    // Same logical queue, same batch, and invalid requests are all no-ops with
    // diagnostics where the request is otherwise malformed.
    const auto sameQueue = QueueSyncPlanner::build(
        {request(image, RenderQueue::Graphics, RenderQueue::Graphics,
                 ResourceUsage::ShaderRead, ResourceUsage::ShaderRead, 1, 2)}, dedicated);
    assert(sameQueue.dependencies.empty() && sameQueue.transfers.empty());
    const auto sameBatch = QueueSyncPlanner::build(
        {request(image, RenderQueue::Graphics, RenderQueue::Compute,
                 ResourceUsage::ShaderRead, ResourceUsage::ShaderRead, 1, 1)}, dedicated);
    assert(sameBatch.dependencies.empty() && sameBatch.transfers.empty());
    const auto invalid = QueueSyncPlanner::build(
        {request({}, RenderQueue::Graphics, RenderQueue::Compute,
                 ResourceUsage::Unknown, ResourceUsage::ShaderRead, 1, 2)}, dedicated);
    assert(invalid.dependencies.empty() && !invalid.diagnostics.empty());

    return 0;
}
