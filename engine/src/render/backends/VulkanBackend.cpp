#if defined(SHINKOU_WITH_VULKAN)
#if defined(SHINKOU_PLATFORM_WINDOWS)
#include <windows.h>
#endif
#include <vulkan/vulkan.h>
#if defined(SHINKOU_WITH_IMGUI)
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#endif
#endif
#include "NativeTextureCache.h"
#include "BackendCommon.h"

#if defined(SHINKOU_WITH_VULKAN)
namespace shinkou::render {
namespace {
class VulkanBackend final : public IRenderBackend {
    using DebugUtilsMessengerCallback = VkBool32 (VKAPI_PTR*)(VkDebugUtilsMessageSeverityFlagBitsEXT,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT*, void*);
    static VkCullModeFlags cull_mode(std::string_view mode) {
        if (mode == "front") return VK_CULL_MODE_FRONT_BIT;
        if (mode == "none") return VK_CULL_MODE_NONE;
        return VK_CULL_MODE_BACK_BIT;
    }
    static VkPolygonMode fill_mode(std::string_view mode) {
        return mode == "wireframe" ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    }
    static VkPrimitiveTopology topology(std::string_view mode) {
        if (mode == "line") return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        if (mode == "point") return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
    struct ResourceRecord {
        ResourceKind kind{ResourceKind::Buffer};
        VkBuffer buffer{VK_NULL_HANDLE};
        VkImage image{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VkSampler sampler{VK_NULL_HANDLE};
        VkFramebuffer framebuffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        GpuAllocation gpuAllocation{};
        bool pooledMemory{false};
        bool hostVisible{false};
        bool depthStencil{false};
        std::size_t size{0};
        VkShaderModule shader{VK_NULL_HANDLE};
        PipelineDesc pipeline{};
        MaterialDesc material{};
        std::vector<ShaderBinding> shaderBindings;
        VkPipeline graphicsPipeline{VK_NULL_HANDLE};
        VkPipeline depthOnlyPipeline{VK_NULL_HANDLE};
        VkRenderPass pipelineRenderPass{VK_NULL_HANDLE};
        VkPipeline computePipeline{VK_NULL_HANDLE};
        VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
        VkDescriptorSetLayout descriptorLayout{VK_NULL_HANDLE};
        VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
        VkDescriptorSet imguiSet{VK_NULL_HANDLE};
        VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
        std::vector<VkDescriptorSetLayout> descriptorLayouts;
        std::vector<VkDescriptorSet> descriptorSets;
        bool alias{false};
        VkFormat format{VK_FORMAT_UNDEFINED};
        VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
        std::uint32_t width{1};
        std::uint32_t height{1};
        std::uint32_t mipLevels{1};
        std::uint32_t layers{1};
        ResourceUsage usage{ResourceUsage::Unknown};
        bool concurrentSharing{false};
        RenderQueue ownerQueue{RenderQueue::Graphics};
        std::uint32_t ownerBatch{UINT32_MAX};
        ~ResourceRecord() = default;
    };
    struct BufferState {
        ResourceUsage usage{ResourceUsage::Unknown};
    };
    struct BindlessTableRecord {
        VkDescriptorSet set{VK_NULL_HANDLE};
        VkDescriptorSetLayout layout{VK_NULL_HANDLE};
        VkDescriptorPool pool{VK_NULL_HANDLE};
        VkDescriptorType descriptorType{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
        std::uint32_t capacity{0};
    };
    VkInstance instance_{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT debugMessenger_{VK_NULL_HANDLE};
    PFN_vkSetDebugUtilsObjectNameEXT setDebugUtilsObjectName_{nullptr};
    PFN_vkCmdBeginDebugUtilsLabelEXT cmdBeginDebugUtilsLabel_{nullptr};
    PFN_vkCmdEndDebugUtilsLabelEXT cmdEndDebugUtilsLabel_{nullptr};
    bool validationEnabled_{false};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue graphicsQueue_{VK_NULL_HANDLE};
    VkQueue presentQueue_{VK_NULL_HANDLE};
    VkQueue computeQueue_{VK_NULL_HANDLE};
    VkQueue copyQueue_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    VkRenderPass renderPass_{VK_NULL_HANDLE};
    VkRenderPass overlayLoadRenderPass_{VK_NULL_HANDLE};
    VkRenderPass depthOnlyRenderPass_{VK_NULL_HANDLE};
    VkRenderPass depthOnlyLoadRenderPass_{VK_NULL_HANDLE};
    std::vector<VkFramebuffer> framebuffers_;
    std::unordered_map<std::uint64_t, VkFramebuffer> dynamicFramebuffers_;
    std::unordered_map<std::uint64_t, VkRenderPass> dynamicRenderPasses_;
    VkImage depthImage_{VK_NULL_HANDLE};
    VkDeviceMemory depthMemory_{VK_NULL_HANDLE};
    VkImageView depthView_{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    std::vector<VkDescriptorPool> descriptorPools_;
    VkPipelineCache pipelineCache_{VK_NULL_HANDLE};
    VkSemaphore uploadTimeline_{VK_NULL_HANDLE};
    std::uint64_t uploadTimelineValue_{0};
    bool timelineSemaphoreSupported_{false};
    VkDescriptorSetLayout bindlessLayout_{VK_NULL_HANDLE};
    VkSampler defaultSampler_{VK_NULL_HANDLE};
    std::vector<ResourceHandle> imguiTextures_;
    VkFormat swapchainFormat_{VK_FORMAT_UNDEFINED};
    VkExtent2D swapchainExtent_{};
    std::vector<bool> swapchainInitialized_;
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainViews_;
    VkCommandPool commandPool_{VK_NULL_HANDLE};
    VkCommandPool computeCommandPool_{VK_NULL_HANDLE};
    VkCommandPool copyCommandPool_{VK_NULL_HANDLE};
    std::vector<VkCommandBuffer> commandBuffers_;
    VkCommandBuffer activeCommandBuffer_{VK_NULL_HANDLE};
    std::vector<VkSemaphore> batchSignals_;
    std::vector<VkFence> batchFences_;
    std::vector<std::uint32_t> currentWaitBatches_;
    std::uint32_t currentBatchIndex_{0};
    bool swapchainPrepared_{false};
    bool currentBatchWaitsForAcquire_{false};
    std::vector<VkCommandBuffer> transientBatchCommands_;
    VkCommandBuffer pendingCommandBuffer_{VK_NULL_HANDLE};
    VkQueue pendingQueue_{VK_NULL_HANDLE};
    VkCommandPool pendingCommandPool_{VK_NULL_HANDLE};
    RenderQueue pendingQueueKind_{RenderQueue::Graphics};
    std::uint32_t pendingBatchIndex_{UINT32_MAX};
    std::vector<std::uint32_t> pendingWaitBatches_;
    std::uint64_t pendingBatchUploadWaitValue_{0};
    bool pendingBatchWaitsForAcquire_{false};
    VkQueue activeQueue_{VK_NULL_HANDLE};
    RenderQueue activeQueueKind_{RenderQueue::Graphics};
    VkSemaphore imageAvailable_{VK_NULL_HANDLE};
    VkSemaphore renderFinished_{VK_NULL_HANDLE};
    VkFence frameFence_{VK_NULL_HANDLE};
    VkPhysicalDeviceFeatures fixedFunctionFeatures_{};
    VkQueryPool timestampPool_{VK_NULL_HANDLE};
    std::uint32_t timestampCursor_{0};
    std::uint32_t submittedTimestampCount_{0};
    bool timestampPoolResetThisFrame_{false};
    float timestampPeriod_{0.0f};
    std::uint32_t imageIndex_{0};
    bool frameActive_{false};
    bool vsync_{true};
    bool renderPassActive_{false};
    VkRenderPass activeRenderPass_{VK_NULL_HANDLE};
    bool computeQueueActive_{false};
    bool frameSetupPending_{false};
    std::uint32_t currentRenderTarget_{0};
    std::uint32_t graphicsFamily_{0};
    std::uint32_t presentFamily_{0};
    std::uint32_t computeFamily_{UINT32_MAX};
    std::uint32_t copyFamily_{UINT32_MAX};
    VkPhysicalDeviceMemoryProperties memoryProperties_{};
    GpuMemoryAllocator gpuMemoryAllocator_{};
    std::unordered_map<std::uint64_t, VkDeviceMemory> gpuMemoryBlocks_;
    std::unordered_map<std::uint32_t, ResourceRecord> resources_;
    std::unordered_map<VkImage, ResourceUsage> imageUsages_;
    std::unordered_map<VkBuffer, ResourceUsage> bufferUsages_;
    std::vector<ResourceRecord> retiredResources_;
    struct RetiredDescriptorSets {
        VkDescriptorPool pool{VK_NULL_HANDLE};
        std::vector<VkDescriptorSet> sets;
    };
    std::vector<RetiredDescriptorSets> retiredDescriptorSets_;
    struct PendingUpload {
        VkFence fence{VK_NULL_HANDLE};
        VkCommandBuffer command{VK_NULL_HANDLE};
        VkBuffer staging{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        std::uint64_t timelineValue{0};
    };
    std::vector<PendingUpload> pendingUploads_;
    bool uploadBatchActive_{false};
    std::uint64_t pendingUploadWaitValue_{0};
    std::unique_ptr<IShaderCompiler> shaderCompiler_{create_shader_compiler()};
    ResourceRecord* boundPipeline_{nullptr};
    ResourceRecord* boundMaterial_{nullptr};
    RenderCapabilities capabilities_{};
    RenderStats stats_{};
    std::string lastError_;
    bool descriptorIndexingEnabled_{false};
    std::unordered_map<std::uint32_t, std::string> debugNames_;
    std::unordered_map<std::uint32_t, BindlessTableRecord> bindlessTables_;
    std::vector<BindlessTableRecord> retiredBindlessTables_;
    std::uint32_t nextBindlessTable_{1};
    std::uint32_t boundBindlessTable_{0};
    std::vector<std::string> debugLabelStorage_;
    std::filesystem::path pipeline_cache_path() const {
        VkPhysicalDeviceProperties properties{};
        if (physicalDevice_ != VK_NULL_HANDLE) vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
        const auto key = std::to_string(properties.vendorID) + "_" + std::to_string(properties.deviceID) + "_" +
            std::to_string(properties.driverVersion) + "_" + std::to_string(VK_VERSION_MAJOR(properties.apiVersion)) +
            "_" + std::to_string(VK_VERSION_MINOR(properties.apiVersion));
        if (const auto* directory = std::getenv("SHINKOU_PIPELINE_CACHE_DIR")) return std::filesystem::path(directory) / (key + ".vpc");
        return std::filesystem::temp_directory_path() / "shinkou_pipeline_cache" / (key + ".vpc");
    }
    std::vector<std::uint8_t> load_pipeline_cache() const {
        std::ifstream stream(pipeline_cache_path(), std::ios::binary | std::ios::ate);
        if (!stream) return {};
        const auto size = stream.tellg();
        if (size <= 0 || size > static_cast<std::streamoff>(1ull << 30)) return {};
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
        stream.seekg(0);
        if (!stream.read(reinterpret_cast<char*>(data.data()), size)) return {};
        if (data.size() < 32) return {};
        VkPhysicalDeviceProperties properties{};
        if (physicalDevice_ != VK_NULL_HANDLE) vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
        const auto header = reinterpret_cast<const std::uint32_t*>(data.data());
        const auto headerVersion = header[0];
        const auto headerVendor = header[2];
        const auto headerDevice = header[3];
        if (headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
            headerVendor != properties.vendorID || headerDevice != properties.deviceID) return {};
        return data;
    }
    void save_pipeline_cache() const {
        if (device_ == VK_NULL_HANDLE || pipelineCache_ == VK_NULL_HANDLE) return;
        std::size_t size = 0;
        if (vkGetPipelineCacheData(device_, pipelineCache_, &size, nullptr) != VK_SUCCESS || size == 0) return;
        std::vector<std::uint8_t> data(size);
        if (vkGetPipelineCacheData(device_, pipelineCache_, &size, data.data()) != VK_SUCCESS) return;
        std::error_code error;
        const auto path = pipeline_cache_path();
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return;
        const auto temporary = path.string() + ".tmp";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return;
        stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(size));
        stream.close();
        if (stream) {
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporary, path, error);
        }
        if (error) std::filesystem::remove(temporary, error);
    }
    VkCommandBuffer command_buffer() const noexcept {
        return activeCommandBuffer_ != VK_NULL_HANDLE ? activeCommandBuffer_ :
            (commandBuffers_.empty() ? VK_NULL_HANDLE : commandBuffers_[imageIndex_]);
    }
    VkCommandBuffer recording_command() const noexcept { return command_buffer(); }
    VkQueue queue_for(RenderQueue queue) const noexcept {
        if (queue == RenderQueue::Compute && computeQueue_ != VK_NULL_HANDLE) return computeQueue_;
        if (queue == RenderQueue::Copy && copyQueue_ != VK_NULL_HANDLE) return copyQueue_;
        return graphicsQueue_;
    }
    VkCommandPool pool_for(RenderQueue queue) const noexcept {
        if (queue == RenderQueue::Compute && computeCommandPool_ != VK_NULL_HANDLE) return computeCommandPool_;
        if (queue == RenderQueue::Copy && copyCommandPool_ != VK_NULL_HANDLE) return copyCommandPool_;
        return commandPool_;
    }
    QueueFamilyMap queue_family_map() const noexcept {
        return {graphicsFamily_, computeFamily_, copyFamily_};
    }
    static VkImageAspectFlags image_aspect_mask(const ResourceRecord& resource) noexcept {
        if (!resource.depthStencil) return VK_IMAGE_ASPECT_COLOR_BIT;
        return resource.format == VK_FORMAT_D24_UNORM_S8_UINT
            ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
            : VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    bool record_ownership_barrier(VkCommandBuffer command,
                                  const VulkanQueueFamilyOwnershipTransfer& transfer,
                                  const ResourceRecord& resource) {
        if (command == VK_NULL_HANDLE || transfer.srcQueueFamilyIndex == kIgnoredQueueFamily ||
            transfer.dstQueueFamilyIndex == kIgnoredQueueFamily ||
            transfer.srcQueueFamilyIndex == transfer.dstQueueFamilyIndex) {
            lastError_ = "Vulkan ownership barrier has invalid queue families";
            return false;
        }
        const auto srcStage = static_cast<VkPipelineStageFlags>(transfer.srcStageMask);
        const auto dstStage = static_cast<VkPipelineStageFlags>(transfer.dstStageMask);
        if (transfer.resourceType == VulkanBarrierResource::Buffer) {
            if (resource.buffer == VK_NULL_HANDLE) {
                lastError_ = "Vulkan ownership barrier has no buffer handle";
                return false;
            }
            VkBufferMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = static_cast<VkAccessFlags>(transfer.srcAccessMask);
            barrier.dstAccessMask = static_cast<VkAccessFlags>(transfer.dstAccessMask);
            barrier.srcQueueFamilyIndex = transfer.srcQueueFamilyIndex;
            barrier.dstQueueFamilyIndex = transfer.dstQueueFamilyIndex;
            barrier.buffer = resource.buffer;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(command, srcStage, dstStage, 0, 0, nullptr, 1, &barrier, 0, nullptr);
            ++stats_.barriers;
            return true;
        }
        if (resource.image == VK_NULL_HANDLE) {
            lastError_ = "Vulkan ownership barrier has no image handle";
            return false;
        }
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = static_cast<VkAccessFlags>(transfer.srcAccessMask);
        barrier.dstAccessMask = static_cast<VkAccessFlags>(transfer.dstAccessMask);
        barrier.oldLayout = static_cast<VkImageLayout>(static_cast<std::int32_t>(transfer.oldLayout));
        barrier.newLayout = static_cast<VkImageLayout>(static_cast<std::int32_t>(transfer.newLayout));
        barrier.srcQueueFamilyIndex = transfer.srcQueueFamilyIndex;
        barrier.dstQueueFamilyIndex = transfer.dstQueueFamilyIndex;
        barrier.image = resource.image;
        barrier.subresourceRange.aspectMask = image_aspect_mask(resource);
        barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        vkCmdPipelineBarrier(command, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        ++stats_.barriers;
        return true;
    }
    bool finalize_pending_queue_batch() {
        if (pendingCommandBuffer_ == VK_NULL_HANDLE) return true;
        const auto command = pendingCommandBuffer_;
        const auto queue = pendingQueue_;
        const auto batchIndex = pendingBatchIndex_;
        if (vkEndCommandBuffer(command) != VK_SUCCESS) {
            lastError_ = "Vulkan pending queue command buffer finalization failed";
            capabilities_.deviceState = RenderDeviceState::NeedsResize;
            pendingCommandBuffer_ = VK_NULL_HANDLE;
            pendingQueue_ = VK_NULL_HANDLE;
            return false;
        }
        std::vector<VkSemaphore> waits;
        std::vector<VkPipelineStageFlags> stages;
        if (pendingBatchWaitsForAcquire_ && imageAvailable_ != VK_NULL_HANDLE) {
            waits.push_back(imageAvailable_);
            stages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
        for (const auto waitBatch : pendingWaitBatches_) {
            if (waitBatch >= batchSignals_.size() || batchSignals_[waitBatch] == VK_NULL_HANDLE) {
                lastError_ = "Vulkan pending queue batch wait references an unsignaled batch";
                capabilities_.deviceState = RenderDeviceState::NeedsResize;
                pendingCommandBuffer_ = VK_NULL_HANDLE;
                pendingQueue_ = VK_NULL_HANDLE;
                return false;
            }
            waits.push_back(batchSignals_[waitBatch]);
            stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }
        const bool hasUploadWait = pendingBatchUploadWaitValue_ != 0 && uploadTimeline_ != VK_NULL_HANDLE;
        if (hasUploadWait) {
            waits.push_back(uploadTimeline_);
            stages.push_back(pendingQueueKind_ == RenderQueue::Copy ? VK_PIPELINE_STAGE_TRANSFER_BIT :
                (pendingQueueKind_ == RenderQueue::Compute ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));
        }
        VkSemaphore signal = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &signal) != VK_SUCCESS) {
            lastError_ = "Vulkan pending queue signal semaphore creation failed";
            pendingCommandBuffer_ = VK_NULL_HANDLE;
            pendingQueue_ = VK_NULL_HANDLE;
            return false;
        }
        if (batchSignals_.size() <= batchIndex) batchSignals_.resize(batchIndex + 1, VK_NULL_HANDLE);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = static_cast<std::uint32_t>(waits.size());
        submit.pWaitSemaphores = waits.data();
        submit.pWaitDstStageMask = stages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &signal;
        VkTimelineSemaphoreSubmitInfo timelineWait{};
        std::vector<std::uint64_t> waitValues;
        if (hasUploadWait) {
            waitValues.assign(waits.size(), 0);
            waitValues.back() = pendingBatchUploadWaitValue_;
            timelineWait.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timelineWait.waitSemaphoreValueCount = static_cast<std::uint32_t>(waitValues.size());
            timelineWait.pWaitSemaphoreValues = waitValues.data();
            submit.pNext = &timelineWait;
        }
        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device_, &fenceInfo, nullptr, &fence) != VK_SUCCESS ||
            vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS) {
            lastError_ = "Vulkan pending queue submission failed";
            capabilities_.deviceState = RenderDeviceState::Lost;
            if (fence != VK_NULL_HANDLE) vkDestroyFence(device_, fence, nullptr);
            vkDestroySemaphore(device_, signal, nullptr);
            pendingCommandBuffer_ = VK_NULL_HANDLE;
            pendingQueue_ = VK_NULL_HANDLE;
            return false;
        }
        batchSignals_[batchIndex] = signal;
        batchFences_.push_back(fence);
        ++stats_.queueSubmissions;
        if (hasUploadWait) pendingBatchUploadWaitValue_ = 0;
        pendingCommandBuffer_ = VK_NULL_HANDLE;
        pendingQueue_ = VK_NULL_HANDLE;
        pendingCommandPool_ = VK_NULL_HANDLE;
        pendingBatchIndex_ = UINT32_MAX;
        pendingWaitBatches_.clear();
        pendingBatchWaitsForAcquire_ = false;
        return true;
    }
    bool record_ownership_transfer(ResourceHandle handle, ResourceRecord& resource,
                                   ResourceUsage before, ResourceUsage after) {
        if (resource.concurrentSharing || resource.ownerBatch == UINT32_MAX) return true;
        const auto families = queue_family_map();
        if (families.same_family(resource.ownerQueue, activeQueueKind_)) return true;
        const QueueSyncRequest request{handle, resource.ownerQueue, activeQueueKind_, before, after,
            resource.ownerBatch, currentBatchIndex_};
        const auto plan = VulkanQueueSyncPlanner::build({request}, families);
        std::string planError;
        if (!VulkanQueueSyncPlanner::validate(plan, &planError) || !plan.requiresOwnershipTransfer() ||
            plan.releaseBarriers.empty() || plan.acquireBarriers.empty()) {
            lastError_ = "Vulkan ownership plan is invalid: " + planError;
            return false;
        }
        if (pendingCommandBuffer_ == VK_NULL_HANDLE || pendingBatchIndex_ != resource.ownerBatch ||
            pendingQueueKind_ != resource.ownerQueue) {
            lastError_ = "Vulkan ownership release batch is no longer pending";
            return false;
        }
        if (std::find(currentWaitBatches_.begin(), currentWaitBatches_.end(), pendingBatchIndex_) == currentWaitBatches_.end()) {
            currentWaitBatches_.push_back(pendingBatchIndex_);
        }
        if (!record_ownership_barrier(pendingCommandBuffer_, plan.releaseBarriers.front(), resource)) return false;
        if (!finalize_pending_queue_batch()) return false;
        if (resource.image != VK_NULL_HANDLE && renderPassActive_) {
            vkCmdEndRenderPass(activeCommandBuffer_);
            renderPassActive_ = false;
            activeRenderPass_ = VK_NULL_HANDLE;
        }
        if (!record_ownership_barrier(activeCommandBuffer_, plan.acquireBarriers.front(), resource)) return false;
        resource.ownerQueue = activeQueueKind_;
        resource.ownerBatch = currentBatchIndex_;
        return true;
    }
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData) {
        if (userData) {
            auto* backend = static_cast<VulkanBackend*>(userData);
            ++backend->stats_.validationMessages;
            if (data && data->pMessage) {
                const char* level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 ? "error" :
                    ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0 ? "warning" : "info");
                backend->stats_.validationDiagnostics.emplace_back(std::string(level) + ": " + data->pMessage);
            }
        }
        return VK_FALSE;
    }
    static VkResult create_debug_messenger(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT& info,
                                           VkDebugUtilsMessengerEXT* messenger) {
        const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        return create ? create(instance, &info, nullptr, messenger) : VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    static void destroy_debug_messenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(instance, messenger, nullptr);
    }
    void clear_dynamic_framebuffers() {
        for (const auto& [key, framebuffer] : dynamicFramebuffers_) {
            if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, framebuffer, nullptr);
        }
        dynamicFramebuffers_.clear();
    }
    void clear_dynamic_render_passes() {
        for (const auto& [key, renderPass] : dynamicRenderPasses_) {
            if (renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(device_, renderPass, nullptr);
        }
        dynamicRenderPasses_.clear();
    }
    VkDescriptorPool create_descriptor_pool(std::uint32_t multiplier) {
        const std::uint32_t safeMultiplier = std::min<std::uint32_t>(multiplier, 64u);
        VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,4096u*safeMultiplier},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096u * safeMultiplier},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 1024u * safeMultiplier},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096u * safeMultiplier},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096u * safeMultiplier},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048u * safeMultiplier}
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT |
            (descriptorIndexingEnabled_ ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0);
        poolInfo.maxSets = 512u * safeMultiplier;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(std::size(poolSizes));
        poolInfo.pPoolSizes = poolSizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
        descriptorPools_.push_back(pool);
        if (descriptorPool_ == VK_NULL_HANDLE) descriptorPool_ = pool;
        return pool;
    }
    bool allocate_descriptor_sets(std::uint32_t count, const VkDescriptorSetLayout* layouts,
                                  VkDescriptorSet* sets, VkDescriptorPool& allocatedPool, const void* pNext = nullptr) {
        if (count == 0 || !layouts || !sets) return false;
        VkDescriptorSetAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.pNext = pNext;
        allocation.descriptorSetCount = count;
        allocation.pSetLayouts = layouts;
        for (const auto pool : descriptorPools_) {
            allocation.descriptorPool = pool;
            const auto result = vkAllocateDescriptorSets(device_, &allocation, sets);
            if (result == VK_SUCCESS) { allocatedPool = pool; return true; }
            if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) break;
        }
        const auto multiplier = descriptorPools_.empty() ? 1u : (1u << std::min<std::size_t>(descriptorPools_.size(), 6u));
        const auto pool = create_descriptor_pool(multiplier);
        if (pool == VK_NULL_HANDLE) return false;
        allocation.descriptorPool = pool;
        if (vkAllocateDescriptorSets(device_, &allocation, sets) != VK_SUCCESS) return false;
        allocatedPool = pool;
        return true;
    }
    void release_completed_uploads(bool waitForAll = false) {
        for (auto it = pendingUploads_.begin(); it != pendingUploads_.end();) {
            if (waitForAll) vkWaitForFences(device_, 1, &it->fence, VK_TRUE, UINT64_MAX);
            else if (vkGetFenceStatus(device_, it->fence) != VK_SUCCESS) {
                ++it;
                continue;
            }
            if (it->command != VK_NULL_HANDLE) vkFreeCommandBuffers(device_, commandPool_, 1, &it->command);
            if (it->fence != VK_NULL_HANDLE) vkDestroyFence(device_, it->fence, nullptr);
            if (it->staging != VK_NULL_HANDLE) vkDestroyBuffer(device_, it->staging, nullptr);
            if (it->memory != VK_NULL_HANDLE) vkFreeMemory(device_, it->memory, nullptr);
            it = pendingUploads_.erase(it);
        }
    }
public:
    ~VulkanBackend() override {
        if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
        release_completed_uploads(true);
        save_pipeline_cache();
        while (!resources_.empty()) destroy_resource(ResourceHandle{resources_.begin()->first, resources_.begin()->second.kind});
        for (auto& resource : retiredResources_) release_resource(resource);
        retiredResources_.clear();
        transientTextures_.clear([&](auto& resource) { release_resource(resource); });
        for (auto& block : gpuMemoryBlocks_) if (block.second != VK_NULL_HANDLE) vkFreeMemory(device_, block.second, nullptr);
        gpuMemoryBlocks_.clear();
        for (auto& table : bindlessTables_) {
            if (table.second.set != VK_NULL_HANDLE && table.second.pool != VK_NULL_HANDLE) {
                vkFreeDescriptorSets(device_, table.second.pool, 1, &table.second.set);
            }
        }
        bindlessTables_.clear();
        for (auto& table : retiredBindlessTables_) {
            if (table.set != VK_NULL_HANDLE && table.pool != VK_NULL_HANDLE) {
                vkFreeDescriptorSets(device_, table.pool, 1, &table.set);
            }
        }
        retiredBindlessTables_.clear();
        for (auto& descriptorSets : retiredDescriptorSets_) {
            if (descriptorSets.pool != VK_NULL_HANDLE && !descriptorSets.sets.empty()) {
                vkFreeDescriptorSets(device_, descriptorSets.pool,
                    static_cast<std::uint32_t>(descriptorSets.sets.size()), descriptorSets.sets.data());
            }
        }
        retiredDescriptorSets_.clear();
        if (bindlessLayout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, bindlessLayout_, nullptr);
        if (defaultSampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, defaultSampler_, nullptr);
        for (const auto pool : descriptorPools_) {
            if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, pool, nullptr);
        }
        descriptorPools_.clear();
        descriptorPool_ = VK_NULL_HANDLE;
        if (pipelineCache_ != VK_NULL_HANDLE) vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
        if (uploadTimeline_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, uploadTimeline_, nullptr);
        for (auto framebuffer : framebuffers_) vkDestroyFramebuffer(device_, framebuffer, nullptr);
        if (renderPass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, renderPass_, nullptr);
        if (overlayLoadRenderPass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, overlayLoadRenderPass_, nullptr);
        if (depthOnlyRenderPass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, depthOnlyRenderPass_, nullptr);
        if (depthOnlyLoadRenderPass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, depthOnlyLoadRenderPass_, nullptr);
        clear_dynamic_framebuffers();
        clear_dynamic_render_passes();
        if (depthView_ != VK_NULL_HANDLE) vkDestroyImageView(device_, depthView_, nullptr);
        if (depthImage_ != VK_NULL_HANDLE) vkDestroyImage(device_, depthImage_, nullptr);
        if (depthMemory_ != VK_NULL_HANDLE) vkFreeMemory(device_, depthMemory_, nullptr);
        if (frameFence_ != VK_NULL_HANDLE) vkDestroyFence(device_, frameFence_, nullptr);
        if (timestampPool_ != VK_NULL_HANDLE) vkDestroyQueryPool(device_, timestampPool_, nullptr);
        if (imageAvailable_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, imageAvailable_, nullptr);
        if (renderFinished_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, renderFinished_, nullptr);
        if (commandPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, commandPool_, nullptr);
        if (computeCommandPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, computeCommandPool_, nullptr);
        if (copyCommandPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, copyCommandPool_, nullptr);
        for (auto semaphore : batchSignals_) if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore, nullptr);
        for (auto fence : batchFences_) if (fence != VK_NULL_HANDLE) vkDestroyFence(device_, fence, nullptr);
        for (auto view : swapchainViews_) vkDestroyImageView(device_, view, nullptr);
        if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        if (debugMessenger_ != VK_NULL_HANDLE) destroy_debug_messenger(instance_, debugMessenger_);
        if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
        if (surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    }
    RenderCapabilities capabilities() const noexcept override { return capabilities_; }
    std::string last_error() const override { return lastError_; }
    void clear_error() override { lastError_.clear(); }
#if defined(SHINKOU_WITH_IMGUI)
    bool initialize_imgui() override {
        if (device_ == VK_NULL_HANDLE || renderPass_ == VK_NULL_HANDLE || !ImGui::GetCurrentContext()) return false;
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_0;
        info.Instance = instance_;
        info.PhysicalDevice = physicalDevice_;
        info.Device = device_;
        info.QueueFamily = graphicsFamily_;
        info.Queue = graphicsQueue_;
        info.RenderPass = renderPass_;
        info.MinImageCount = std::max<std::uint32_t>(2, static_cast<std::uint32_t>(swapchainImages_.size()));
        info.ImageCount = info.MinImageCount;
        info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.PipelineCache = pipelineCache_;
        info.DescriptorPoolSize = 1000;
        info.MinAllocationSize = 1024 * 1024;
        return ImGui_ImplVulkan_Init(&info);
    }
    void shutdown_imgui() override {
        // Descriptors belong to ImGui's pool, images retain engine ownership.
        for (auto& [id, resource] : resources_) {
            (void)id;
            if (resource.imguiSet) ImGui_ImplVulkan_RemoveTexture(resource.imguiSet);
            resource.imguiSet=VK_NULL_HANDLE;
        }
        for (auto& resource : retiredResources_) {
            if (resource.imguiSet) ImGui_ImplVulkan_RemoveTexture(resource.imguiSet);
            resource.imguiSet=VK_NULL_HANDLE;
        }
        imguiTextures_.clear();
        if (ImGui::GetCurrentContext()) ImGui_ImplVulkan_Shutdown();
    }
    void new_imgui_frame() override {imguiTextures_.clear(); ImGui_ImplVulkan_NewFrame();}
    void invalidate_imgui_fonts() override { ImGui_ImplVulkan_DestroyFontsTexture(); }
    std::uint64_t imgui_texture(ResourceHandle handle) override {
        const auto found=resources_.find(handle.id);
        if (found==resources_.end() || !found->second.view || found->second.layers!=1 || found->second.samples!=VK_SAMPLE_COUNT_1_BIT) return 0;
        auto& resource=found->second;
        if (!resource.imguiSet) resource.imguiSet=ImGui_ImplVulkan_AddTexture(defaultSampler_,resource.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        imguiTextures_.push_back(handle);
        return reinterpret_cast<std::uint64_t>(resource.imguiSet);
    }
    void render_imgui(ImDrawData* drawData) override {
        if (!drawData || !frameActive_ || activeCommandBuffer_ == VK_NULL_HANDLE) return;
        if (renderPassActive_) { vkCmdEndRenderPass(activeCommandBuffer_); renderPassActive_=false; activeRenderPass_=VK_NULL_HANDLE; }
        for (const auto texture : imguiTextures_) transition_resource(texture,ResourceUsage::ShaderRead);
        imguiTextures_.clear();
        VkRenderPassBeginInfo begin{};
        begin.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass=overlayLoadRenderPass_;
        begin.framebuffer=framebuffers_[imageIndex_];
        begin.renderArea.extent=swapchainExtent_;
        vkCmdBeginRenderPass(activeCommandBuffer_,&begin,VK_SUBPASS_CONTENTS_INLINE);
        renderPassActive_=true; activeRenderPass_=overlayLoadRenderPass_; currentRenderTarget_=0;
        if (renderPassActive_) ImGui_ImplVulkan_RenderDrawData(drawData, activeCommandBuffer_);
    }
#endif
    bool initialize(const RenderBackendConfig& config) override {
        vsync_ = config.vsync;
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "ShinkouEngine";
        app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app.pEngineName = "ShinkouEngine";
        app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &app;
        std::vector<const char*> instanceExtensions;
#if defined(SHINKOU_PLATFORM_WINDOWS)
        if (config.nativeWindow) {
            instanceExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
            instanceExtensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
        }
#endif
        std::uint32_t instanceExtensionCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr);
        std::vector<VkExtensionProperties> availableInstanceExtensions(instanceExtensionCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, availableInstanceExtensions.data());
        const auto hasInstanceExtension = [&](const char* name) {
            return std::any_of(availableInstanceExtensions.begin(), availableInstanceExtensions.end(), [name](const auto& extension) {
                return std::strcmp(extension.extensionName, name) == 0;
            });
        };
        std::uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> layers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
        const bool validationLayerAvailable = std::any_of(layers.begin(), layers.end(), [](const auto& layer) {
            return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        validationEnabled_ = config.enableValidation && validationLayerAvailable &&
            hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (validationEnabled_) instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(instanceExtensions.size());
        createInfo.ppEnabledExtensionNames = instanceExtensions.data();
        const char* validationLayer = "VK_LAYER_KHRONOS_validation";
        if (validationEnabled_) {
            createInfo.enabledLayerCount = 1;
            createInfo.ppEnabledLayerNames = &validationLayer;
        }
        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        if (validationEnabled_) {
            debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debugCreateInfo.pfnUserCallback = &debug_callback;
            debugCreateInfo.pUserData = this;
            createInfo.pNext = &debugCreateInfo;
        }
        if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) return false;
        if (validationEnabled_ && create_debug_messenger(instance_, debugCreateInfo, &debugMessenger_) != VK_SUCCESS) {
            validationEnabled_ = false;
        }
#if defined(SHINKOU_PLATFORM_WINDOWS)
        if (config.nativeWindow) {
            VkWin32SurfaceCreateInfoKHR surfaceInfo{};
            surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
            surfaceInfo.hinstance = GetModuleHandleA(nullptr);
            surfaceInfo.hwnd = static_cast<HWND>(config.nativeWindow);
            if (vkCreateWin32SurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_) != VK_SUCCESS) return false;
        }
#endif

        std::uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (count == 0) return false;
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
        std::uint32_t bestGraphicsFamily = 0;
        std::uint32_t bestPresentFamily = 0;
        std::uint32_t bestComputeFamily = UINT32_MAX;
        std::uint32_t bestCopyFamily = UINT32_MAX;
        std::int64_t bestScore = std::numeric_limits<std::int64_t>::min();
        for (const auto device : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(device, &properties);
            std::uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
            std::uint32_t graphicsFamily = UINT32_MAX;
            std::uint32_t presentFamily = surface_ == VK_NULL_HANDLE ? 0u : UINT32_MAX;
            std::uint32_t dedicatedComputeFamily = UINT32_MAX;
            std::uint32_t dedicatedCopyFamily = UINT32_MAX;
            for (std::uint32_t i = 0; i < familyCount; ++i) {
                if (graphicsFamily == UINT32_MAX && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) graphicsFamily = i;
                if (dedicatedComputeFamily == UINT32_MAX &&
                    (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                    !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) dedicatedComputeFamily = i;
                if (dedicatedCopyFamily == UINT32_MAX &&
                    (families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
                    !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                    !(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) dedicatedCopyFamily = i;
                if (surface_) {
                    VkBool32 supported = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &supported);
                    if (presentFamily == UINT32_MAX && supported) presentFamily = i;
                }
            }
            if (graphicsFamily == UINT32_MAX || presentFamily == UINT32_MAX) continue;
            VkPhysicalDeviceMemoryProperties memoryProperties{};
            vkGetPhysicalDeviceMemoryProperties(device, &memoryProperties);
            std::uint64_t localMemory = 0;
            for (std::uint32_t i = 0; i < memoryProperties.memoryHeapCount; ++i) {
                if (memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                    localMemory += memoryProperties.memoryHeaps[i].size;
                }
            }
            std::int64_t score = static_cast<std::int64_t>(localMemory / (256ull * 1024ull * 1024ull));
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 100000;
            else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 10000;
            else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) score += 1000;
            if (graphicsFamily == presentFamily) score += 100;
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) score -= 1000000;
            if (score > bestScore) {
                bestScore = score;
                bestDevice = device;
                bestGraphicsFamily = graphicsFamily;
                bestPresentFamily = presentFamily;
                bestComputeFamily = dedicatedComputeFamily;
                bestCopyFamily = dedicatedCopyFamily;
            }
        }
        if (bestDevice == VK_NULL_HANDLE) return false;
        physicalDevice_ = bestDevice;
        graphicsFamily_ = bestGraphicsFamily;
        presentFamily_ = bestPresentFamily;
        computeFamily_ = bestComputeFamily;
        copyFamily_ = bestCopyFamily;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties_);
        const float priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queueInfos;
        const std::array<std::uint32_t, 4> requestedFamilies{
            graphicsFamily_, presentFamily_, computeFamily_, copyFamily_};
        for (const auto queueFamily : requestedFamilies) {
            if (queueFamily == UINT32_MAX) continue;
            bool duplicate = false;
            for (const auto& existing : queueInfos) duplicate |= existing.queueFamilyIndex == queueFamily;
            if (duplicate) continue;
            VkDeviceQueueCreateInfo queueInfo{};
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = queueFamily;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &priority;
            queueInfos.push_back(queueInfo);
        }
        VkPhysicalDeviceProperties physicalProperties{};
        vkGetPhysicalDeviceProperties(physicalDevice_, &physicalProperties);
        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
        deviceInfo.pQueueCreateInfos = queueInfos.data();
        std::uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extensionCount, extensions.data());
        const auto hasExtension = [&](const char* name) {
            return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& extension) {
                return std::strcmp(extension.extensionName, name) == 0;
            });
        };
        const bool descriptorExtension = hasExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        VkPhysicalDeviceFeatures2 supportedFeatures{};
        supportedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        VkPhysicalDeviceDescriptorIndexingFeatures supportedIndexing{};
        supportedIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        supportedFeatures.pNext = &supportedIndexing;
        vkGetPhysicalDeviceFeatures2(physicalDevice_, &supportedFeatures);
        VkPhysicalDeviceTimelineSemaphoreFeatures supportedTimeline{};
        supportedTimeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
        supportedFeatures.pNext = &supportedTimeline;
        vkGetPhysicalDeviceFeatures2(physicalDevice_, &supportedFeatures);
        const auto apiVersion = physicalProperties.apiVersion;
        const bool descriptorCore = VK_VERSION_MAJOR(apiVersion) > 1 ||
            (VK_VERSION_MAJOR(apiVersion) == 1 && VK_VERSION_MINOR(apiVersion) >= 2);
        const bool timelineCore = descriptorCore;
        const bool timelineExtension = hasExtension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        timelineSemaphoreSupported_ = (timelineCore || timelineExtension) && supportedTimeline.timelineSemaphore == VK_TRUE;
        descriptorIndexingEnabled_ = (descriptorExtension || descriptorCore) &&
            supportedIndexing.runtimeDescriptorArray && supportedIndexing.descriptorBindingPartiallyBound &&
            supportedIndexing.descriptorBindingVariableDescriptorCount &&
            supportedIndexing.descriptorBindingSampledImageUpdateAfterBind;
        std::vector<const char*> deviceExtensions;
        if (surface_) deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        if (descriptorIndexingEnabled_ && descriptorExtension) deviceExtensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        if (timelineSemaphoreSupported_ && timelineExtension) deviceExtensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size());
        deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
        VkPhysicalDeviceDescriptorIndexingFeatures enabledIndexing{};
        VkPhysicalDeviceTimelineSemaphoreFeatures enabledTimeline{};
        if (descriptorIndexingEnabled_) {
            enabledIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
            enabledIndexing.runtimeDescriptorArray = VK_TRUE;
            enabledIndexing.descriptorBindingPartiallyBound = VK_TRUE;
            enabledIndexing.descriptorBindingVariableDescriptorCount = VK_TRUE;
            enabledIndexing.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            deviceInfo.pNext = &enabledIndexing;
        }
        if (timelineSemaphoreSupported_) {
            enabledTimeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
            enabledTimeline.timelineSemaphore = VK_TRUE;
            enabledTimeline.pNext = descriptorIndexingEnabled_ ? static_cast<void*>(&enabledIndexing) : nullptr;
            deviceInfo.pNext = &enabledTimeline;
        }
        fixedFunctionFeatures_.independentBlend = supportedFeatures.features.independentBlend;
        fixedFunctionFeatures_.dualSrcBlend = supportedFeatures.features.dualSrcBlend;
        fixedFunctionFeatures_.depthClamp = supportedFeatures.features.depthClamp;
        fixedFunctionFeatures_.depthBiasClamp = supportedFeatures.features.depthBiasClamp;
        fixedFunctionFeatures_.fillModeNonSolid = supportedFeatures.features.fillModeNonSolid;
        fixedFunctionFeatures_.multiDrawIndirect = supportedFeatures.features.multiDrawIndirect;
        deviceInfo.pEnabledFeatures = &fixedFunctionFeatures_;
        if (vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_) != VK_SUCCESS) return false;
        setDebugUtilsObjectName_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(vkGetDeviceProcAddr(device_, "vkSetDebugUtilsObjectNameEXT"));
        cmdBeginDebugUtilsLabel_ = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(vkGetDeviceProcAddr(device_, "vkCmdBeginDebugUtilsLabelEXT"));
        cmdEndDebugUtilsLabel_ = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(vkGetDeviceProcAddr(device_, "vkCmdEndDebugUtilsLabelEXT"));
        vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
        vkGetDeviceQueue(device_, presentFamily_, 0, &presentQueue_);
        if (computeFamily_ != UINT32_MAX) vkGetDeviceQueue(device_, computeFamily_, 0, &computeQueue_);
        if (copyFamily_ != UINT32_MAX) vkGetDeviceQueue(device_, copyFamily_, 0, &copyQueue_);
        timestampPeriod_ = physicalProperties.limits.timestampPeriod;
        if (physicalProperties.limits.timestampComputeAndGraphics) {
            VkQueryPoolCreateInfo queryInfo{};
            queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = 512;
            vkCreateQueryPool(device_, &queryInfo, nullptr, &timestampPool_);
        }
        if (create_descriptor_pool(1) == VK_NULL_HANDLE) return false;
        VkPipelineCacheCreateInfo pipelineCacheInfo{};
        pipelineCacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        auto pipelineCacheData = load_pipeline_cache();
        pipelineCacheInfo.initialDataSize = pipelineCacheData.size();
        pipelineCacheInfo.pInitialData = pipelineCacheData.empty() ? nullptr : pipelineCacheData.data();
        auto pipelineCacheResult = vkCreatePipelineCache(device_, &pipelineCacheInfo, nullptr, &pipelineCache_);
        if (pipelineCacheResult != VK_SUCCESS && !pipelineCacheData.empty()) {
            pipelineCacheData.clear();
            pipelineCacheInfo.initialDataSize = 0;
            pipelineCacheInfo.pInitialData = nullptr;
            pipelineCacheResult = vkCreatePipelineCache(device_, &pipelineCacheInfo, nullptr, &pipelineCache_);
        }
        if (pipelineCacheResult != VK_SUCCESS) return false;
        if (timelineSemaphoreSupported_) {
            VkSemaphoreTypeCreateInfo timelineInfo{};
            timelineInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
            timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            timelineInfo.initialValue = 0;
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            semaphoreInfo.pNext = &timelineInfo;
            if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &uploadTimeline_) != VK_SUCCESS) {
                timelineSemaphoreSupported_ = false;
            }
        }
        if (descriptorIndexingEnabled_ && !create_bindless_layout()) {
            descriptorIndexingEnabled_ = false;
        }
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.maxAnisotropy = 1.0f;
        if (vkCreateSampler(device_, &samplerInfo, nullptr, &defaultSampler_) != VK_SUCCESS) return false;

        if (surface_ && !create_swapchain(config.width, config.height)) return false;
        if (!surface_) {
            VkCommandPoolCreateInfo uploadPoolInfo{};
            uploadPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            uploadPoolInfo.queueFamilyIndex = graphicsFamily_;
            uploadPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            if (vkCreateCommandPool(device_, &uploadPoolInfo, nullptr, &commandPool_) != VK_SUCCESS) return false;
            if (computeFamily_ != UINT32_MAX && computeFamily_ != graphicsFamily_) {
                uploadPoolInfo.queueFamilyIndex = computeFamily_;
                if (vkCreateCommandPool(device_, &uploadPoolInfo, nullptr, &computeCommandPool_) != VK_SUCCESS) return false;
            }
            if (copyFamily_ != UINT32_MAX && copyFamily_ != graphicsFamily_ && copyFamily_ != computeFamily_) {
                uploadPoolInfo.queueFamilyIndex = copyFamily_;
                if (vkCreateCommandPool(device_, &uploadPoolInfo, nullptr, &copyCommandPool_) != VK_SUCCESS) return false;
            }
            commandBuffers_.resize(1);
            VkCommandBufferAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocation.commandPool = commandPool_;
            allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocation.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(device_, &allocation, commandBuffers_.data()) != VK_SUCCESS) return false;
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkCreateFence(device_, &fenceInfo, nullptr, &frameFence_) != VK_SUCCESS) return false;
        }
        capabilities_.api = BackendApi::Vulkan;
        capabilities_.deviceReady = true;
        capabilities_.deviceState = RenderDeviceState::Ready;
        capabilities_.supportsImGui = false;
#if defined(SHINKOU_WITH_IMGUI)
        capabilities_.supportsImGui = true;
#endif
        capabilities_.supportsNativeUi = false;
        capabilities_.supportsCompute = true;
        capabilities_.supportsBindless = descriptorIndexingEnabled_;
        capabilities_.supportsMultiDrawIndirect = true;
        capabilities_.supportsDedicatedComputeQueue = computeFamily_ != UINT32_MAX;
        capabilities_.supportsDedicatedCopyQueue = copyFamily_ != UINT32_MAX;
        capabilities_.supportsValidation = validationEnabled_;
        capabilities_.supportsGpuMemoryAllocator = true;
        capabilities_.supportsGpuMemoryAliasing = true;
        return true;
    }
    bool resize(std::uint32_t width, std::uint32_t height) override {
        if (width == 0 || height == 0) {
            lastError_ = "Vulkan swapchain dimensions must be non-zero";
            return false;
        }
        if (!device_ || !surface_) return true;
        const auto idleResult = vkDeviceWaitIdle(device_);
        if (idleResult == VK_ERROR_DEVICE_LOST) {
            lastError_ = "Vulkan device lost while resizing swapchain";
            capabilities_.deviceState = RenderDeviceState::Lost;
            return false;
        }
        release_completed_uploads(true);
        clear_dynamic_framebuffers();
        clear_dynamic_render_passes();
        for (auto& [id, resource] : resources_) {
            if (resource.graphicsPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, resource.graphicsPipeline, nullptr);
                resource.graphicsPipeline = VK_NULL_HANDLE;
            }
            if (resource.depthOnlyPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, resource.depthOnlyPipeline, nullptr);
                resource.depthOnlyPipeline = VK_NULL_HANDLE;
            }
            if (resource.pipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_, resource.pipelineLayout, nullptr);
                resource.pipelineLayout = VK_NULL_HANDLE;
            }
            if (resource.descriptorLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device_, resource.descriptorLayout, nullptr);
                resource.descriptorLayout = VK_NULL_HANDLE;
            }
            for (const auto layout : resource.descriptorLayouts) {
                if (layout != VK_NULL_HANDLE && layout != bindlessLayout_) vkDestroyDescriptorSetLayout(device_, layout, nullptr);
            }
            resource.descriptorLayouts.clear();
            release_descriptor_sets(resource, false);
            resource.pipelineRenderPass = VK_NULL_HANDLE;
        }
        for (auto framebuffer : framebuffers_) vkDestroyFramebuffer(device_, framebuffer, nullptr);
        framebuffers_.clear();
        if (renderPass_ != VK_NULL_HANDLE) { vkDestroyRenderPass(device_, renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
        if (overlayLoadRenderPass_ != VK_NULL_HANDLE) { vkDestroyRenderPass(device_, overlayLoadRenderPass_, nullptr); overlayLoadRenderPass_=VK_NULL_HANDLE; }
        if (depthOnlyRenderPass_ != VK_NULL_HANDLE) { vkDestroyRenderPass(device_, depthOnlyRenderPass_, nullptr); depthOnlyRenderPass_ = VK_NULL_HANDLE; }
        if (depthOnlyLoadRenderPass_ != VK_NULL_HANDLE) { vkDestroyRenderPass(device_, depthOnlyLoadRenderPass_, nullptr); depthOnlyLoadRenderPass_ = VK_NULL_HANDLE; }
        if (depthView_ != VK_NULL_HANDLE) { vkDestroyImageView(device_, depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
        if (depthImage_ != VK_NULL_HANDLE) { vkDestroyImage(device_, depthImage_, nullptr); depthImage_ = VK_NULL_HANDLE; }
        if (depthMemory_ != VK_NULL_HANDLE) { vkFreeMemory(device_, depthMemory_, nullptr); depthMemory_ = VK_NULL_HANDLE; }
        for (auto view : swapchainViews_) vkDestroyImageView(device_, view, nullptr);
        swapchainViews_.clear();
        if (swapchain_ != VK_NULL_HANDLE) { vkDestroySwapchainKHR(device_, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }
        if (frameFence_ != VK_NULL_HANDLE) { vkDestroyFence(device_, frameFence_, nullptr); frameFence_ = VK_NULL_HANDLE; }
        if (imageAvailable_ != VK_NULL_HANDLE) { vkDestroySemaphore(device_, imageAvailable_, nullptr); imageAvailable_ = VK_NULL_HANDLE; }
        if (renderFinished_ != VK_NULL_HANDLE) { vkDestroySemaphore(device_, renderFinished_, nullptr); renderFinished_ = VK_NULL_HANDLE; }
        if (commandPool_ != VK_NULL_HANDLE) { vkDestroyCommandPool(device_, commandPool_, nullptr); commandPool_ = VK_NULL_HANDLE; }
        commandBuffers_.clear();
        frameActive_ = false;
        const bool recreated = create_swapchain(width, height);
        if (!recreated) {
            lastError_ = "Vulkan swapchain recreation failed";
            capabilities_.deviceState = RenderDeviceState::NeedsResize;
        } else {
            capabilities_.deviceState = RenderDeviceState::Ready;
        }
        return recreated;
    }
    bool create_swapchain(std::uint32_t width, std::uint32_t height) {
        VkSurfaceCapabilitiesKHR capabilities{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities) != VK_SUCCESS) return false;
        std::uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
        if (formatCount == 0) return false;
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data());
        VkSurfaceFormatKHR format = formats.front();
        for (const auto candidate : formats) {
            if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM) { format = candidate; break; }
        }
        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == UINT32_MAX) {
            extent.width = std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent.height = std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }
        swapchainFormat_ = format.format;
        swapchainExtent_ = extent;
        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = surface_;
        info.minImageCount = std::max(2u, capabilities.minImageCount);
        if (capabilities.maxImageCount > 0) info.minImageCount = std::min(info.minImageCount, capabilities.maxImageCount);
        info.imageFormat = format.format;
        info.imageColorSpace = format.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const std::uint32_t families[] = {graphicsFamily_, presentFamily_};
        if (graphicsFamily_ != presentFamily_) {
            info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            info.queueFamilyIndexCount = 2;
            info.pQueueFamilyIndices = families;
        } else {
            info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        if (!vsync_) {
            std::uint32_t presentModeCount = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount, nullptr);
            std::vector<VkPresentModeKHR> presentModes(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount, presentModes.data());
            if (std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != presentModes.end()) {
                info.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
            } else if (std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != presentModes.end()) {
                info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            }
        }
        info.clipped = VK_TRUE;
        if (vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_) != VK_SUCCESS) return false;
        std::uint32_t imageCount = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
        swapchainImages_.resize(imageCount);
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());
        swapchainViews_.resize(imageCount);
        for (std::uint32_t i = 0; i < imageCount; ++i) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = swapchainImages_[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format.format;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            if (vkCreateImageView(device_, &viewInfo, nullptr, &swapchainViews_[i]) != VK_SUCCESS) return false;
        }
        VkImageCreateInfo depthInfo{};
        depthInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        depthInfo.imageType = VK_IMAGE_TYPE_2D;
        depthInfo.format = VK_FORMAT_D24_UNORM_S8_UINT;
        depthInfo.extent = {extent.width, extent.height, 1};
        depthInfo.mipLevels = 1;
        depthInfo.arrayLayers = 1;
        depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (vkCreateImage(device_, &depthInfo, nullptr, &depthImage_) != VK_SUCCESS) return false;
        VkMemoryRequirements depthRequirements{};
        vkGetImageMemoryRequirements(device_, depthImage_, &depthRequirements);
        const auto depthMemoryType = find_memory_type(depthRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (depthMemoryType == UINT32_MAX) return false;
        VkMemoryAllocateInfo depthAllocation{};
        depthAllocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        depthAllocation.allocationSize = depthRequirements.size;
        depthAllocation.memoryTypeIndex = depthMemoryType;
        if (vkAllocateMemory(device_, &depthAllocation, nullptr, &depthMemory_) != VK_SUCCESS) return false;
        vkBindImageMemory(device_, depthImage_, depthMemory_, 0);
        VkImageViewCreateInfo depthViewInfo{};
        depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        depthViewInfo.image = depthImage_;
        depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        depthViewInfo.format = depthInfo.format;
        depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        depthViewInfo.subresourceRange.levelCount = 1;
        depthViewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &depthViewInfo, nullptr, &depthView_) != VK_SUCCESS) return false;
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = swapchainFormat_;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = depthInfo.format;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthReference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        subpass.pDepthStencilAttachment = &depthReference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        const VkAttachmentDescription attachments[] = {colorAttachment, depthAttachment};
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 2;
        renderPassInfo.pAttachments = attachments;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;
        if (vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_) != VK_SUCCESS) return false;
        VkAttachmentDescription loadAttachments[] = {colorAttachment,depthAttachment};
        loadAttachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
        loadAttachments[1].loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        loadAttachments[1].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        loadAttachments[1].initialLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkRenderPassCreateInfo overlayInfo=renderPassInfo;
        overlayInfo.pAttachments=loadAttachments;
        if (vkCreateRenderPass(device_,&overlayInfo,nullptr,&overlayLoadRenderPass_)!=VK_SUCCESS) return false;
        VkAttachmentDescription depthOnlyAttachment = depthAttachment;
        VkAttachmentReference depthOnlyReference{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription depthOnlySubpass{};
        depthOnlySubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        depthOnlySubpass.pDepthStencilAttachment = &depthOnlyReference;
        VkRenderPassCreateInfo depthOnlyInfo{};
        depthOnlyInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        depthOnlyInfo.attachmentCount = 1;
        depthOnlyInfo.pAttachments = &depthOnlyAttachment;
        depthOnlyInfo.subpassCount = 1;
        depthOnlyInfo.pSubpasses = &depthOnlySubpass;
        if (vkCreateRenderPass(device_, &depthOnlyInfo, nullptr, &depthOnlyRenderPass_) != VK_SUCCESS) return false;
        VkAttachmentDescription depthLoadAttachment = depthOnlyAttachment;
        depthLoadAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthLoadAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthLoadAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkRenderPassCreateInfo depthLoadInfo = depthOnlyInfo;
        depthLoadInfo.pAttachments = &depthLoadAttachment;
        if (vkCreateRenderPass(device_, &depthLoadInfo, nullptr, &depthOnlyLoadRenderPass_) != VK_SUCCESS) return false;
        framebuffers_.resize(imageCount);
        swapchainInitialized_.assign(imageCount, false);
        for (std::uint32_t i = 0; i < imageCount; ++i) {
            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass_;
            const VkImageView framebufferAttachments[] = {swapchainViews_[i], depthView_};
            framebufferInfo.attachmentCount = 2;
            framebufferInfo.pAttachments = framebufferAttachments;
            framebufferInfo.width = swapchainExtent_.width;
            framebufferInfo.height = swapchainExtent_.height;
            framebufferInfo.layers = 1;
            if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &framebuffers_[i]) != VK_SUCCESS) return false;
        }
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = graphicsFamily_;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) return false;
        if (computeFamily_ != UINT32_MAX && computeFamily_ != graphicsFamily_) {
            poolInfo.queueFamilyIndex = computeFamily_;
            if (vkCreateCommandPool(device_, &poolInfo, nullptr, &computeCommandPool_) != VK_SUCCESS) return false;
        }
        if (copyFamily_ != UINT32_MAX && copyFamily_ != graphicsFamily_ && copyFamily_ != computeFamily_) {
            poolInfo.queueFamilyIndex = copyFamily_;
            if (vkCreateCommandPool(device_, &poolInfo, nullptr, &copyCommandPool_) != VK_SUCCESS) return false;
        }
        commandBuffers_.resize(imageCount);
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = commandPool_;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = imageCount;
        if (vkAllocateCommandBuffers(device_, &allocation, commandBuffers_.data()) != VK_SUCCESS) return false;
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailable_) != VK_SUCCESS) return false;
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinished_) != VK_SUCCESS) return false;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        return vkCreateFence(device_, &fenceInfo, nullptr, &frameFence_) == VK_SUCCESS;
    }
    std::uint32_t find_memory_type(std::uint32_t typeBits, VkMemoryPropertyFlags properties) const {
        for (std::uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) && (memoryProperties_.memoryTypes[i].propertyFlags & properties) == properties) return i;
        }
        return UINT32_MAX;
    }
    VkDeviceMemory ensure_gpu_memory_block(const GpuAllocation& allocation) {
        if (!allocation) return VK_NULL_HANDLE;
        const auto existing = gpuMemoryBlocks_.find(allocation.blockId);
        if (existing != gpuMemoryBlocks_.end()) return existing->second;
        const auto capacity = gpuMemoryAllocator_.block_capacity(allocation.blockId);
        if (capacity == 0 || allocation.memoryType >= memoryProperties_.memoryTypeCount) return VK_NULL_HANDLE;
        VkMemoryAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        info.allocationSize = capacity;
        info.memoryTypeIndex = allocation.memoryType;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        const auto result = vkAllocateMemory(device_, &info, nullptr, &memory);
        if (result != VK_SUCCESS || memory == VK_NULL_HANDLE) {
            lastError_ = "Vulkan pooled device memory allocation failed result=" +
                std::to_string(static_cast<int>(result));
            if (memory != VK_NULL_HANDLE) vkFreeMemory(device_, memory, nullptr);
            return VK_NULL_HANDLE;
        }
        gpuMemoryBlocks_.emplace(allocation.blockId, memory);
        return memory;
    }
    bool allocate_resource_memory(const VkMemoryRequirements& requirements,
                                  VkMemoryPropertyFlags properties, VkDeviceMemory& memory,
                                  GpuAllocation& allocation, bool& pooled) {
        const auto memoryType = find_memory_type(requirements.memoryTypeBits, properties);
        if (memoryType == UINT32_MAX) {
            lastError_ = "Vulkan resource has no compatible memory type";
            return false;
        }
        allocation = gpuMemoryAllocator_.allocate(static_cast<std::size_t>(requirements.size),
            static_cast<std::size_t>(requirements.alignment), memoryType, GpuMemoryLifetime::Persistent);
        if (allocation) {
            memory = ensure_gpu_memory_block(allocation);
            if (memory != VK_NULL_HANDLE) {
                pooled = true;
                return true;
            }
            gpuMemoryAllocator_.release(allocation);
            allocation = {};
        }
        // Keep the dedicated path as a portable fallback for small heaps,
        // unusual device limits, and drivers that reject the pooled block.
        lastError_.clear();
        VkMemoryAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        info.allocationSize = requirements.size;
        info.memoryTypeIndex = memoryType;
        const auto result = vkAllocateMemory(device_, &info, nullptr, &memory);
        if (result != VK_SUCCESS || memory == VK_NULL_HANDLE) {
            lastError_ = "Vulkan device memory allocation failed result=" +
                std::to_string(static_cast<int>(result));
            if (memory != VK_NULL_HANDLE) vkFreeMemory(device_, memory, nullptr);
            return false;
        }
        pooled = false;
        return true;
    }
    void release_gpu_allocation(ResourceRecord& resource) {
        if (resource.pooledMemory && resource.gpuAllocation) {
            gpuMemoryAllocator_.release(resource.gpuAllocation);
        }
        resource.gpuAllocation = {};
        resource.pooledMemory = false;
    }
    static VkFormat format_of(std::string_view format) {
        if (format == "bgra8") return VK_FORMAT_B8G8R8A8_UNORM;
        if (format == "rgba16f") return VK_FORMAT_R16G16B16A16_SFLOAT;
        if (format == "r32f") return VK_FORMAT_R32_SFLOAT;
        if (format == "d24s8") return VK_FORMAT_D24_UNORM_S8_UINT;
        if (format == "d32f") return VK_FORMAT_D32_SFLOAT;
        if (format == "bc1") return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        if (format == "bc3") return VK_FORMAT_BC3_UNORM_BLOCK;
        if (format == "bc5") return VK_FORMAT_BC5_UNORM_BLOCK;
        if (format == "bc6h") return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        if (format == "bc7") return VK_FORMAT_BC7_UNORM_BLOCK;
        return VK_FORMAT_R8G8B8A8_UNORM;
    }
    static bool block_compressed_format(VkFormat format) noexcept {
        return format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || format == VK_FORMAT_BC3_UNORM_BLOCK ||
            format == VK_FORMAT_BC5_UNORM_BLOCK || format == VK_FORMAT_BC6H_UFLOAT_BLOCK ||
            format == VK_FORMAT_BC7_UNORM_BLOCK;
    }
    VkRenderPass get_or_create_color_render_pass(const std::vector<VkFormat>& colorFormats, VkFormat depthFormat,
                                                 VkSampleCountFlagBits samples, bool clearAttachments) {
        std::uint64_t key = clearAttachments ? 1ull : 0ull;
        key = key * 257ull + static_cast<std::uint64_t>(samples);
        for (const auto format : colorFormats) key = key * 257ull + static_cast<std::uint64_t>(format);
        key = key * 257ull + static_cast<std::uint64_t>(depthFormat);
        const auto existing = dynamicRenderPasses_.find(key);
        if (existing != dynamicRenderPasses_.end()) return existing->second;
        std::vector<VkAttachmentDescription> attachments;
        std::vector<VkAttachmentReference> colorReferences;
        const bool depth = depthFormat != VK_FORMAT_UNDEFINED;
        attachments.reserve(colorFormats.size() + (depth ? 1 : 0));
        colorReferences.reserve(colorFormats.size());
        for (std::size_t index = 0; index < colorFormats.size(); ++index) {
            VkAttachmentDescription attachment{};
            attachment.format = colorFormats[index];
            attachment.samples = samples;
            attachment.loadOp = clearAttachments ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachments.push_back(attachment);
            colorReferences.push_back({static_cast<std::uint32_t>(index), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
        }
        VkAttachmentReference depthReference{};
        if (depth) {
            VkAttachmentDescription attachment{};
            attachment.format = depthFormat;
            attachment.samples = samples;
            attachment.loadOp = clearAttachments ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.stencilLoadOp = clearAttachments ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = clearAttachments ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachments.push_back(attachment);
            depthReference = {static_cast<std::uint32_t>(colorFormats.size()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        }
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = static_cast<std::uint32_t>(colorReferences.size());
        subpass.pColorAttachments = colorReferences.data();
        subpass.pDepthStencilAttachment = depth ? &depthReference : nullptr;
        VkRenderPassCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        if (vkCreateRenderPass(device_, &info, nullptr, &renderPass) != VK_SUCCESS) return VK_NULL_HANDLE;
        dynamicRenderPasses_.emplace(key, renderPass);
        return renderPass;
    }
    NativeTextureCache<ResourceRecord> transientTextures_;
    bool create_transient_resource(ResourceHandle handle,const ResourceDesc& description) override {
        if (auto reused=transientTextures_.take(handle,description)) {
            destroy_resource(handle);
            resources_.emplace(handle.id,std::move(*reused));
            ++stats_.transientTextureReuses;
            return true;
        }
        if (!create_resource(handle,description)) return false;
        transientTextures_.track(handle,description);
        return true;
    }
    void retire_transient_resource(ResourceHandle handle,bool reusable) override {
        const auto found=resources_.find(handle.id);
        if (reusable && found!=resources_.end() && !found->second.alias && transientTextures_.recycle(handle,found->second)) {
            resources_.erase(found);
            return;
        }
        transientTextures_.forget(handle);
        retire_resource(handle);
    }
    bool create_resource(ResourceHandle handle, const ResourceDesc& description) override {
        if (!handle || !resource_description_matches_kind(handle.kind, description)) {
            lastError_ = "Vulkan resource handle and description kind do not match";
            return false;
        }
        if (const auto* pipeline=std::get_if<PipelineDesc>(&description); pipeline &&
            !validate_pipeline_state(*pipeline,lastError_)) return false;
        destroy_resource(handle);
        ResourceRecord record;
        record.kind = handle.kind;
        std::visit([&](const auto& desc) {
            using T = std::decay_t<decltype(desc)>;
            if constexpr (std::is_same_v<T, TextureDesc>) {
                TexturePhysicalImagePlan plan;
                if (!prepare_backend_texture(desc, BackendApi::Vulkan, plan, lastError_)) return;
                const VkFormat format = format_of(texture_format_name(plan.format));
                const bool depth = texture_format_is_depth(plan.format);
                record.depthStencil = depth;
                record.format = format;
                record.width = plan.width;
                record.height = plan.height;
                record.mipLevels = plan.mipLevels;
                record.layers = plan.physicalArrayLayers;
                VkImageCreateInfo imageInfo{};
                imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
                imageInfo.imageType = VK_IMAGE_TYPE_2D;
                imageInfo.format = format;
                imageInfo.extent = {plan.width, plan.height, 1};
                imageInfo.mipLevels = plan.mipLevels;
                imageInfo.arrayLayers = plan.physicalArrayLayers;
                switch (plan.sampleCount) {
                case 1: imageInfo.samples = VK_SAMPLE_COUNT_1_BIT; break;
                case 2: imageInfo.samples = VK_SAMPLE_COUNT_2_BIT; break;
                case 4: imageInfo.samples = VK_SAMPLE_COUNT_4_BIT; break;
                case 8: imageInfo.samples = VK_SAMPLE_COUNT_8_BIT; break;
                default: lastError_ = "Vulkan texture sample count is not representable"; return;
                }
                record.samples = imageInfo.samples;
                imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
                imageInfo.usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT :
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                if (desc.renderTarget) imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                if (desc.storage) imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
                const std::array<std::uint32_t, 3> resourceFamilies{graphicsFamily_, computeFamily_, copyFamily_};
                std::vector<std::uint32_t> sharingFamilies;
                for (const auto family : resourceFamilies) {
                    if (family == UINT32_MAX || std::find(sharingFamilies.begin(), sharingFamilies.end(), family) != sharingFamilies.end()) continue;
                    sharingFamilies.push_back(family);
                }
                record.concurrentSharing = sharingFamilies.size() > 1;
                if (record.concurrentSharing) {
                    imageInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
                    imageInfo.queueFamilyIndexCount = static_cast<std::uint32_t>(sharingFamilies.size());
                    imageInfo.pQueueFamilyIndices = sharingFamilies.data();
                }
                if (vkCreateImage(device_, &imageInfo, nullptr, &record.image) != VK_SUCCESS) return;
                VkMemoryRequirements requirements{};
                vkGetImageMemoryRequirements(device_, record.image, &requirements);
                if (!allocate_resource_memory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        record.memory, record.gpuAllocation, record.pooledMemory)) {
                    vkDestroyImage(device_, record.image, nullptr);
                    record.image = VK_NULL_HANDLE;
                    return;
                }
                if (vkBindImageMemory(device_, record.image, record.memory,
                        record.pooledMemory ? record.gpuAllocation.offset : 0) != VK_SUCCESS) {
                    const auto wasPooled = record.pooledMemory;
                    if (wasPooled) release_gpu_allocation(record);
                    if (!wasPooled && record.memory != VK_NULL_HANDLE) vkFreeMemory(device_, record.memory, nullptr);
                    record.memory = VK_NULL_HANDLE;
                    vkDestroyImage(device_, record.image, nullptr);
                    record.image = VK_NULL_HANDLE;
                    lastError_ = "Vulkan image memory binding failed";
                    return;
                }
                VkImageViewCreateInfo viewInfo{};
                viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                viewInfo.image = record.image;
                viewInfo.viewType = plan.physicalArrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
                viewInfo.format = format;
                viewInfo.subresourceRange.aspectMask = depth
                    ? (plan.format == TextureFormat::D24UnormS8Uint
                        ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT)
                    : VK_IMAGE_ASPECT_COLOR_BIT;
                viewInfo.subresourceRange.levelCount = plan.mipLevels;
                viewInfo.subresourceRange.layerCount = plan.physicalArrayLayers;
                if (vkCreateImageView(device_, &viewInfo, nullptr, &record.view) != VK_SUCCESS) {
                    lastError_ = "Vulkan texture image view creation failed";
                    return;
                }
                // Off-screen color targets use the dynamic attachment path at bind time.
                // The static framebuffer is reserved for the swapchain's actual format.
            } else if constexpr (std::is_same_v<T, BufferDesc>) {
                record.size = desc.size;
                VkBufferCreateInfo bufferInfo{};
                bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bufferInfo.size = std::max<std::size_t>(desc.size, 1);
                bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                if (desc.vertexBuffer) bufferInfo.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
                if (desc.indexBuffer) bufferInfo.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
                if (desc.indirectBuffer) bufferInfo.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
                if (!desc.vertexBuffer && !desc.indexBuffer) bufferInfo.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                const std::array<std::uint32_t, 3> resourceFamilies{graphicsFamily_, computeFamily_, copyFamily_};
                std::vector<std::uint32_t> sharingFamilies;
                for (const auto family : resourceFamilies) {
                    if (family == UINT32_MAX || std::find(sharingFamilies.begin(), sharingFamilies.end(), family) != sharingFamilies.end()) continue;
                    sharingFamilies.push_back(family);
                }
                record.concurrentSharing = sharingFamilies.size() > 1;
                if (record.concurrentSharing) {
                    bufferInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
                    bufferInfo.queueFamilyIndexCount = static_cast<std::uint32_t>(sharingFamilies.size());
                    bufferInfo.pQueueFamilyIndices = sharingFamilies.data();
                }
                if (vkCreateBuffer(device_, &bufferInfo, nullptr, &record.buffer) != VK_SUCCESS) return;
                VkMemoryRequirements requirements{};
                vkGetBufferMemoryRequirements(device_, record.buffer, &requirements);
                record.hostVisible = false;
                if (!allocate_resource_memory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        record.memory, record.gpuAllocation, record.pooledMemory)) {
                    vkDestroyBuffer(device_, record.buffer, nullptr);
                    record.buffer = VK_NULL_HANDLE;
                    return;
                }
                if (vkBindBufferMemory(device_, record.buffer, record.memory,
                        record.pooledMemory ? record.gpuAllocation.offset : 0) != VK_SUCCESS) {
                    const auto wasPooled = record.pooledMemory;
                    if (wasPooled) release_gpu_allocation(record);
                    if (!wasPooled && record.memory != VK_NULL_HANDLE) vkFreeMemory(device_, record.memory, nullptr);
                    record.memory = VK_NULL_HANDLE;
                    vkDestroyBuffer(device_, record.buffer, nullptr);
                    record.buffer = VK_NULL_HANDLE;
                    lastError_ = "Vulkan buffer memory binding failed";
                    return;
                }
            } else if constexpr (std::is_same_v<T, SamplerDesc>) {
                VkSamplerCreateInfo samplerInfo{};
                samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                samplerInfo.magFilter = desc.filter == "nearest" ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
                samplerInfo.minFilter = desc.filter == "nearest" ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
                samplerInfo.mipmapMode = desc.filter == "nearest" ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
                samplerInfo.addressModeU = desc.addressU == "clamp" ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
                samplerInfo.addressModeV = desc.addressV == "clamp" ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
                samplerInfo.addressModeW = desc.addressW == "clamp" ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
                samplerInfo.anisotropyEnable = desc.maxAnisotropy > 1.0f ? VK_TRUE : VK_FALSE;
                samplerInfo.maxAnisotropy = std::max(1.0f, desc.maxAnisotropy);
                samplerInfo.compareEnable = desc.compareEnable ? VK_TRUE : VK_FALSE;
                samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
                samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
                vkCreateSampler(device_, &samplerInfo, nullptr, &record.sampler);
            } else if constexpr (std::is_same_v<T, ShaderDesc>) {
                const auto compiled = shaderCompiler_->compile(desc, BackendApi::Vulkan);
                if (!compiled.valid || compiled.bytecode.size() % sizeof(std::uint32_t) != 0) {
                    lastError_ = compiled.diagnostics.empty() ? "invalid Vulkan SPIR-V bytecode" : compiled.diagnostics;
                    return;
                }
                record.shaderBindings = compiled.bindings;
                if (!validate_backend_shader_layout(record.shaderBindings, lastError_)) return;
                VkShaderModuleCreateInfo shaderInfo{};
                shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                shaderInfo.codeSize = compiled.bytecode.size();
                shaderInfo.pCode = reinterpret_cast<const std::uint32_t*>(compiled.bytecode.data());
                vkCreateShaderModule(device_, &shaderInfo, nullptr, &record.shader);
            } else if constexpr (std::is_same_v<T, PipelineDesc>) {
                record.pipeline = desc;
                record.pipeline.vertexLayout=resolve_vertex_layout(desc);
            } else if constexpr (std::is_same_v<T, MaterialDesc>) {
                record.material = desc;
            }
        }, description);
        resources_.emplace(handle.id, std::move(record));
        if (const auto inserted = resources_.find(handle.id); inserted != resources_.end() && inserted->second.buffer != VK_NULL_HANDLE) {
            bufferUsages_[inserted->second.buffer] = ResourceUsage::Unknown;
        }
        ++stats_.resourceCreates;
        if (const auto* buffer = std::get_if<BufferDesc>(&description); buffer && !buffer->initialData.empty()) {
            if (!update_buffer({handle, 0, buffer->initialData})) {
                if (lastError_.empty()) lastError_ = "Vulkan initial buffer upload failed";
                else lastError_ = "Vulkan initial buffer upload failed: " + lastError_;
                retire_resource(handle);
                return false;
            }
        }
        if (const auto* texture = std::get_if<TextureDesc>(&description); texture && !texture->initialData.empty()) {
            const auto textureFormat = texture->formatKind == TextureFormat::Unknown
                ? texture_format_from_name(texture->format) : texture->formatKind;
            const auto bytesPerPixel = textureFormat == TextureFormat::RGBA16Float ? 8u : 4u;
            if (!update_texture({handle, 0, 0, texture->width, texture->height,
                static_cast<std::size_t>(texture->width) * bytesPerPixel, texture->initialData})) {
                if (lastError_.empty()) lastError_ = "Vulkan initial texture upload failed";
                else lastError_ = "Vulkan initial texture upload failed: " + lastError_;
                retire_resource(handle);
                return false;
            }
        }
        if (const auto* texture = std::get_if<TextureDesc>(&description); texture) {
            for (const auto& subresource : texture->initialSubresources) {
                if (!update_texture({handle, subresource.mipLevel, subresource.layer, subresource.width,
                    subresource.height, subresource.rowPitch, subresource.data})) {
                    if (lastError_.empty()) lastError_ = "Vulkan initial texture subresource upload failed";
                    else lastError_ = "Vulkan initial texture subresource upload failed: " + lastError_;
                    retire_resource(handle);
                    return false;
                }
            }
            const auto hasExplicitMip = std::any_of(texture->initialSubresources.begin(), texture->initialSubresources.end(),
                [](const TextureDesc::SubresourceData& subresource) { return subresource.mipLevel > 0; });
            if (texture->generateMips && texture->mipLevels > 1 && !hasExplicitMip &&
                (!texture->initialData.empty() || !texture->initialSubresources.empty()) && !generate_mips(handle)) {
                if (lastError_.empty()) lastError_ = "Vulkan initial texture mip generation failed";
                else lastError_ = "Vulkan initial texture mip generation failed: " + lastError_;
                retire_resource(handle);
                return false;
            }
        }
        const auto created = resources_.find(handle.id);
        if (created == resources_.end() ||
            ((handle.kind == ResourceKind::Texture2D || handle.kind == ResourceKind::DepthStencil) &&
                (!created->second.image || !created->second.memory || !created->second.view)) ||
            (handle.kind == ResourceKind::Buffer && (!created->second.buffer || !created->second.memory))) {
            if (lastError_.empty()) lastError_ = "Vulkan resource creation failed";
            if (created != resources_.end()) retire_resource(handle);
            return false;
        }
        if (handle.kind == ResourceKind::Sampler && !created->second.sampler) {
            if (lastError_.empty()) lastError_ = "Vulkan sampler creation failed";
            retire_resource(handle);
            return false;
        }
        if (handle.kind == ResourceKind::Shader && !created->second.shader) {
            if (lastError_.empty()) lastError_ = "Vulkan shader module creation failed";
            retire_resource(handle);
            return false;
        }
        return true;
    }
    void release_descriptor_sets(ResourceRecord& resource, bool defer = true) {
        if (resource.descriptorPool == VK_NULL_HANDLE || resource.descriptorSets.empty()) return;
        std::vector<VkDescriptorSet> sets;
        for (const auto set : resource.descriptorSets) if (set != VK_NULL_HANDLE) sets.push_back(set);
        if (!sets.empty()) {
            if (defer) retiredDescriptorSets_.push_back({resource.descriptorPool, std::move(sets)});
            else vkFreeDescriptorSets(device_, resource.descriptorPool,
                static_cast<std::uint32_t>(sets.size()), sets.data());
        }
        resource.descriptorSets.clear();
        resource.descriptorSet = VK_NULL_HANDLE;
        resource.descriptorPool = VK_NULL_HANDLE;
    }
    void release_resource(ResourceRecord& resource) {
        if (resource.alias) {
            resource = {};
            return;
        }
        release_descriptor_sets(resource, false);
#if defined(SHINKOU_WITH_IMGUI)
        if (resource.imguiSet && ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData)
            ImGui_ImplVulkan_RemoveTexture(resource.imguiSet);
        resource.imguiSet=VK_NULL_HANDLE;
#endif
        if (resource.shader != VK_NULL_HANDLE) vkDestroyShaderModule(device_, resource.shader, nullptr);
        if (resource.graphicsPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, resource.graphicsPipeline, nullptr);
        if (resource.depthOnlyPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, resource.depthOnlyPipeline, nullptr);
        if (resource.computePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, resource.computePipeline, nullptr);
        if (resource.pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, resource.pipelineLayout, nullptr);
        if (resource.descriptorLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, resource.descriptorLayout, nullptr);
        for (const auto layout : resource.descriptorLayouts) {
            if (layout != VK_NULL_HANDLE && layout != bindlessLayout_) vkDestroyDescriptorSetLayout(device_, layout, nullptr);
        }
        if (resource.framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, resource.framebuffer, nullptr);
        if (resource.view != VK_NULL_HANDLE) vkDestroyImageView(device_, resource.view, nullptr);
        if (resource.sampler != VK_NULL_HANDLE) vkDestroySampler(device_, resource.sampler, nullptr);
        if (resource.image != VK_NULL_HANDLE) {
            imageUsages_.erase(resource.image);
            vkDestroyImage(device_, resource.image, nullptr);
        }
        if (resource.buffer != VK_NULL_HANDLE) {
            bufferUsages_.erase(resource.buffer);
            vkDestroyBuffer(device_, resource.buffer, nullptr);
        }
        const auto wasPooled = resource.pooledMemory;
        if (wasPooled) release_gpu_allocation(resource);
        if (!wasPooled && resource.memory != VK_NULL_HANDLE) vkFreeMemory(device_, resource.memory, nullptr);
        resource.memory = VK_NULL_HANDLE;
        resource = {};
    }
    void destroy_resource(ResourceHandle handle) override {
        retire_resource(handle);
    }
    bool alias_resource(ResourceHandle logical, ResourceHandle physical) override {
        const auto source = resources_.find(physical.id);
        if (!logical || !physical || source == resources_.end() || source->second.kind != physical.kind ||
            source->second.kind != logical.kind ||
            (source->second.kind != ResourceKind::Texture2D && source->second.kind != ResourceKind::DepthStencil && source->second.kind != ResourceKind::Buffer) ||
            ((source->second.kind == ResourceKind::Texture2D && source->second.image == VK_NULL_HANDLE) ||
             (source->second.kind == ResourceKind::Buffer && source->second.buffer == VK_NULL_HANDLE))) return false;
        ResourceRecord alias;
        alias.kind = source->second.kind;
        alias.buffer = source->second.buffer;
        alias.image = source->second.image;
        alias.view = source->second.view;
        alias.framebuffer = source->second.framebuffer;
        alias.memory = source->second.memory;
        alias.gpuAllocation = source->second.gpuAllocation;
        alias.pooledMemory = source->second.pooledMemory;
        alias.depthStencil = source->second.depthStencil;
        alias.hostVisible = source->second.hostVisible;
        alias.size = source->second.size;
        alias.format = source->second.format;
        alias.width = source->second.width;
        alias.height = source->second.height;
        alias.mipLevels = source->second.mipLevels;
        alias.layers = source->second.layers;
        alias.samples = source->second.samples;
        alias.usage = source->second.usage;
        alias.concurrentSharing = source->second.concurrentSharing;
        alias.ownerQueue = source->second.ownerQueue;
        alias.ownerBatch = source->second.ownerBatch;
        alias.alias = true;
        resources_.emplace(logical.id, std::move(alias));
        return true;
    }
    bool update_buffer(const BufferUpdate& update) override {
        const auto it = resources_.find(update.buffer.id);
        if (update.buffer.kind != ResourceKind::Buffer || it == resources_.end() || it->second.kind != ResourceKind::Buffer ||
            !it->second.buffer || update.data.empty() || update.offset > it->second.size ||
            update.data.size() > it->second.size - update.offset ||
            frameActive_ || commandPool_ == VK_NULL_HANDLE) {
            lastError_ = "Vulkan buffer handle or update state is invalid";
            return false;
        }
        auto& resource = it->second;
        if (resource.hostVisible) {
            if (!uploadBatchActive_ && !wait_for_frame_writes()) return false;
            void* mapped = nullptr;
            if (vkMapMemory(device_, resource.memory, update.offset, update.data.size(), 0, &mapped) != VK_SUCCESS) {
                lastError_ = "Vulkan host-visible buffer map failed";
                return false;
            }
            std::memcpy(mapped, update.data.data(), update.data.size());
            vkUnmapMemory(device_, resource.memory);
            return true;
        }
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = update.data.size();
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(device_, &bufferInfo, nullptr, &staging) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, staging, &requirements);
        const auto memoryType = find_memory_type(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (memoryType == UINT32_MAX) {
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(device_, &allocation, nullptr, &stagingMemory) != VK_SUCCESS) {
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        if (vkBindBufferMemory(device_, staging, stagingMemory, 0) != VK_SUCCESS) {
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            lastError_ = "Vulkan staging buffer memory binding failed";
            return false;
        }
        void* mapped = nullptr;
        if (vkMapMemory(device_, stagingMemory, 0, update.data.size(), 0, &mapped) != VK_SUCCESS) {
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            lastError_ = "Vulkan staging buffer map failed";
            return false;
        }
        std::memcpy(mapped, update.data.data(), update.data.size());
        vkUnmapMemory(device_, stagingMemory);
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo commandAllocation{};
        commandAllocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandAllocation.commandPool = commandPool_;
        commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandAllocation.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &commandAllocation, &command) != VK_SUCCESS) {
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        const auto previous = bufferUsages_.count(resource.buffer) ? bufferUsages_[resource.buffer] : ResourceUsage::Unknown;
        VkBufferMemoryBarrier before{};
        before.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        before.srcAccessMask = buffer_access_of(previous);
        before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.buffer = resource.buffer;
        before.offset = update.offset;
        before.size = update.data.size();
        vkCmdPipelineBarrier(command, buffer_stage_of(previous), VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 1, &before, 0, nullptr);
        VkBufferCopy copy{0, 0, update.data.size()};
        vkCmdCopyBuffer(command, staging, resource.buffer, 1, &copy);
        VkBufferMemoryBarrier after = before;
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = buffer_access_of(previous);
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, buffer_stage_of(previous),
            0, 0, nullptr, 1, &after, 0, nullptr);
        if (vkEndCommandBuffer(command) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        VkFence uploadFence = VK_NULL_HANDLE;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device_, &fenceInfo, nullptr, &uploadFence) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        VkTimelineSemaphoreSubmitInfo timelineSubmit{};
        const std::uint64_t signalValue = uploadTimeline_ != VK_NULL_HANDLE ? ++uploadTimelineValue_ : 0;
        if (uploadTimeline_ != VK_NULL_HANDLE) {
            timelineSubmit.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timelineSubmit.signalSemaphoreValueCount = 1;
            timelineSubmit.pSignalSemaphoreValues = &signalValue;
            submit.pNext = &timelineSubmit;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &uploadTimeline_;
        }
        const bool submitted = vkQueueSubmit(graphicsQueue_, 1, &submit, uploadFence) == VK_SUCCESS;
        if (!submitted) {
            vkDestroyFence(device_, uploadFence, nullptr);
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        if (uploadBatchActive_) {
            pendingUploads_.push_back({uploadFence, command, staging, stagingMemory, signalValue});
            pendingUploadWaitValue_ = std::max(pendingUploadWaitValue_, signalValue);
            bufferUsages_[resource.buffer] = previous;
            return true;
        }
        const auto waitResult = vkWaitForFences(device_, 1, &uploadFence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(device_, uploadFence, nullptr);
        vkFreeCommandBuffers(device_, commandPool_, 1, &command);
        vkFreeMemory(device_, stagingMemory, nullptr);
        vkDestroyBuffer(device_, staging, nullptr);
        if (waitResult != VK_SUCCESS) {
            lastError_ = "Vulkan buffer upload fence wait failed result=" +
                std::to_string(static_cast<int>(waitResult));
            return false;
        }
        bufferUsages_[resource.buffer] = previous;
        return true;
    }
    bool update_texture(const TextureUpdate& update) override {
        const auto it = resources_.find(update.texture.id);
        if (update.texture.kind != ResourceKind::Texture2D || it == resources_.end() || it->second.kind != ResourceKind::Texture2D ||
            it->second.image == VK_NULL_HANDLE || update.data.empty() || update.width == 0 || update.height == 0 ||
            frameActive_ || commandPool_ == VK_NULL_HANDLE) {
            lastError_ = "Vulkan texture handle or update state is invalid";
            return false;
        }
        auto& resource = it->second;
        if (resource.depthStencil || update.mipLevel >= resource.mipLevels || update.layer >= resource.layers) {
            lastError_ = "Vulkan texture update targets an invalid subresource";
            return false;
        }
        if (block_compressed_format(resource.format)) {
            lastError_ = "Vulkan compressed texture updates require block-compressed upload metadata";
            return false;
        }
        const auto mipWidth = std::max(1u, (resource.width >> update.mipLevel));
        const auto mipHeight = std::max(1u, (resource.height >> update.mipLevel));
        const auto bytesPerPixel = resource.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : 4u;
        const auto sourcePitch = update.rowPitch == 0 ? static_cast<std::size_t>(update.width) * bytesPerPixel : update.rowPitch;
        if (update.x > mipWidth || update.y > mipHeight || update.width > mipWidth - update.x ||
            update.height > mipHeight - update.y || sourcePitch < static_cast<std::size_t>(update.width) * bytesPerPixel ||
            sourcePitch > std::numeric_limits<std::size_t>::max() / update.height ||
            sourcePitch * update.height > update.data.size() || sourcePitch % bytesPerPixel != 0 ||
            sourcePitch / bytesPerPixel > std::numeric_limits<std::uint32_t>::max()) {
            lastError_ = "Vulkan texture update exceeds resource limits";
            return false;
        }
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = update.data.size();
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(device_, &bufferInfo, nullptr, &staging) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, staging, &requirements);
        const auto memoryType = find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (memoryType == UINT32_MAX) { vkDestroyBuffer(device_, staging, nullptr); return false; }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(device_, &allocation, nullptr, &stagingMemory) != VK_SUCCESS) { vkDestroyBuffer(device_, staging, nullptr); return false; }
        if (vkBindBufferMemory(device_, staging, stagingMemory, 0) != VK_SUCCESS) {
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            lastError_ = "Vulkan texture staging buffer memory binding failed";
            return false;
        }
        void* mapped = nullptr;
        if (vkMapMemory(device_, stagingMemory, 0, update.data.size(), 0, &mapped) != VK_SUCCESS) {
            vkFreeMemory(device_, stagingMemory, nullptr); vkDestroyBuffer(device_, staging, nullptr);
            lastError_ = "Vulkan texture staging buffer map failed";
            return false;
        }
        std::memcpy(mapped, update.data.data(), update.data.size());
        vkUnmapMemory(device_, stagingMemory);
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo commandAllocation{};
        commandAllocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandAllocation.commandPool = commandPool_;
        commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandAllocation.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &commandAllocation, &command) != VK_SUCCESS) {
            vkFreeMemory(device_, stagingMemory, nullptr); vkDestroyBuffer(device_, staging, nullptr); return false;
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            lastError_ = "Vulkan texture upload command buffer begin failed";
            return false;
        }
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        const auto usageIt = imageUsages_.find(resource.image);
        const auto previousUsage = usageIt == imageUsages_.end() ? ResourceUsage::Unknown : usageIt->second;
        toTransfer.oldLayout = previousUsage == ResourceUsage::Unknown ? VK_IMAGE_LAYOUT_UNDEFINED : layout_of(previousUsage);
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcAccessMask = access_of(previousUsage);
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = resource.image;
        // The backend tracks one layout for the complete image. Transition
        // the complete image here as well; a single-mip barrier combined with
        // a whole-image state map makes later mip/layer uploads use an invalid
        // old layout.
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.baseMipLevel = 0;
        toTransfer.subresourceRange.levelCount = resource.mipLevels;
        toTransfer.subresourceRange.baseArrayLayer = 0;
        toTransfer.subresourceRange.layerCount = resource.layers;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);
        VkBufferImageCopy copy{};
        copy.bufferOffset = 0;
        copy.bufferRowLength = static_cast<std::uint32_t>(sourcePitch / bytesPerPixel);
        copy.bufferImageHeight = update.height;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = update.mipLevel;
        copy.imageSubresource.baseArrayLayer = update.layer;
        copy.imageSubresource.layerCount = 1;
        copy.imageOffset = {static_cast<std::int32_t>(update.x), static_cast<std::int32_t>(update.y), 0};
        copy.imageExtent = {update.width, update.height, 1};
        vkCmdCopyBufferToImage(command, staging, resource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        VkImageMemoryBarrier toShader = toTransfer;
        toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShader);
        if (vkEndCommandBuffer(command) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            lastError_ = "Vulkan texture upload command buffer end failed";
            return false;
        }
        VkFence uploadFence = VK_NULL_HANDLE;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device_, &fenceInfo, nullptr, &uploadFence) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        VkTimelineSemaphoreSubmitInfo timelineSubmit{};
        const std::uint64_t signalValue = uploadTimeline_ != VK_NULL_HANDLE ? ++uploadTimelineValue_ : 0;
        if (uploadTimeline_ != VK_NULL_HANDLE) {
            timelineSubmit.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timelineSubmit.signalSemaphoreValueCount = 1;
            timelineSubmit.pSignalSemaphoreValues = &signalValue;
            submit.pNext = &timelineSubmit;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &uploadTimeline_;
        }
        const bool submitted = vkQueueSubmit(graphicsQueue_, 1, &submit, uploadFence) == VK_SUCCESS;
        if (!submitted) {
            vkDestroyFence(device_, uploadFence, nullptr);
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            vkFreeMemory(device_, stagingMemory, nullptr);
            vkDestroyBuffer(device_, staging, nullptr);
            return false;
        }
        if (uploadBatchActive_) {
            pendingUploads_.push_back({uploadFence, command, staging, stagingMemory, signalValue});
            pendingUploadWaitValue_ = std::max(pendingUploadWaitValue_, signalValue);
            imageUsages_[resource.image] = ResourceUsage::ShaderRead;
            resource.usage = ResourceUsage::ShaderRead;
            return true;
        }
        const auto waitResult = vkWaitForFences(device_, 1, &uploadFence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(device_, uploadFence, nullptr);
        vkFreeCommandBuffers(device_, commandPool_, 1, &command);
        vkFreeMemory(device_, stagingMemory, nullptr);
        vkDestroyBuffer(device_, staging, nullptr);
        if (waitResult != VK_SUCCESS) {
            lastError_ = "Vulkan texture upload fence wait failed result=" +
                std::to_string(static_cast<int>(waitResult));
            return false;
        }
        imageUsages_[resource.image] = ResourceUsage::ShaderRead;
        resource.usage = ResourceUsage::ShaderRead;
        return true;
    }
    bool resolve_texture(ResourceHandle destination, ResourceHandle source) override {
        const auto destinationIt = resources_.find(destination.id);
        const auto sourceIt = resources_.find(source.id);
        if (!frameActive_ || activeCommandBuffer_ == VK_NULL_HANDLE ||
            destination.kind != ResourceKind::Texture2D || source.kind != ResourceKind::Texture2D ||
            destination.id == source.id || destinationIt == resources_.end() || sourceIt == resources_.end() ||
            destinationIt->second.kind != ResourceKind::Texture2D || sourceIt->second.kind != ResourceKind::Texture2D ||
            destinationIt->second.image == VK_NULL_HANDLE || sourceIt->second.image == VK_NULL_HANDLE) {
            lastError_ = "Vulkan resolve requires an active command buffer and two valid texture resources";
            return false;
        }
        const auto& destinationResource = destinationIt->second;
        const auto& sourceResource = sourceIt->second;
        if (sourceResource.samples == VK_SAMPLE_COUNT_1_BIT || destinationResource.samples != VK_SAMPLE_COUNT_1_BIT ||
            sourceResource.width != destinationResource.width || sourceResource.height != destinationResource.height ||
            sourceResource.layers != destinationResource.layers || sourceResource.mipLevels != destinationResource.mipLevels ||
            sourceResource.format != destinationResource.format || sourceResource.depthStencil || destinationResource.depthStencil) {
            lastError_ = "Vulkan resolve resources must match and use a color MSAA source";
            return false;
        }
        if (renderPassActive_) {
            vkCmdEndRenderPass(activeCommandBuffer_);
            renderPassActive_ = false;
        }
        const auto sourceUsageIt = imageUsages_.find(sourceResource.image);
        const auto destinationUsageIt = imageUsages_.find(destinationResource.image);
        const auto sourceUsage = sourceUsageIt == imageUsages_.end() ? ResourceUsage::Unknown : sourceUsageIt->second;
        const auto destinationUsage = destinationUsageIt == imageUsages_.end() ? ResourceUsage::Unknown : destinationUsageIt->second;
        if (sourceUsage == ResourceUsage::Unknown) {
            lastError_ = "Vulkan resolve source image has no tracked layout";
            return false;
        }
        const auto makeBarrier = [](VkImage image, ResourceUsage previous, VkImageLayout oldLayout,
                                    VkImageLayout newLayout, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
                                    std::uint32_t mipLevels, std::uint32_t layers) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcAccessMask = sourceAccess == 0 ? access_of(previous) : sourceAccess;
            barrier.dstAccessMask = destinationAccess;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, layers};
            return barrier;
        };
        const auto sourceToTransfer = makeBarrier(sourceResource.image, sourceUsage, layout_of(sourceUsage),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
            sourceResource.mipLevels, sourceResource.layers);
        const auto destinationOldLayout = destinationUsage == ResourceUsage::Unknown ? VK_IMAGE_LAYOUT_UNDEFINED : layout_of(destinationUsage);
        const auto destinationToTransfer = makeBarrier(destinationResource.image, destinationUsage, destinationOldLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, destinationUsage == ResourceUsage::Unknown ? VK_ACCESS_NONE : 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, destinationResource.mipLevels, destinationResource.layers);
        const std::array<VkImageMemoryBarrier, 2> before{sourceToTransfer, destinationToTransfer};
        vkCmdPipelineBarrier(activeCommandBuffer_, queue_stage_mask(sourceUsage), VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, static_cast<std::uint32_t>(before.size()), before.data());
        for (std::uint32_t mip = 0; mip < sourceResource.mipLevels; ++mip) {
            VkImageResolve region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, sourceResource.layers};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, destinationResource.layers};
            region.extent = {std::max(1u, sourceResource.width >> mip), std::max(1u, sourceResource.height >> mip), 1};
            vkCmdResolveImage(activeCommandBuffer_, sourceResource.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                destinationResource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        const auto sourceAfter = makeBarrier(sourceResource.image, ResourceUsage::ShaderRead,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, sourceResource.mipLevels, sourceResource.layers);
        const auto destinationAfter = makeBarrier(destinationResource.image, ResourceUsage::ShaderRead,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, destinationResource.mipLevels, destinationResource.layers);
        const std::array<VkImageMemoryBarrier, 2> after{sourceAfter, destinationAfter};
        vkCmdPipelineBarrier(activeCommandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
            0, nullptr, 0, nullptr, static_cast<std::uint32_t>(after.size()), after.data());
        imageUsages_[sourceResource.image] = ResourceUsage::ShaderRead;
        imageUsages_[destinationResource.image] = ResourceUsage::ShaderRead;
        sourceIt->second.usage = ResourceUsage::ShaderRead;
        destinationIt->second.usage = ResourceUsage::ShaderRead;
        return true;
    }
    bool generate_mips(ResourceHandle texture) override {
        const auto it = resources_.find(texture.id);
        if (texture.kind != ResourceKind::Texture2D || it == resources_.end() || it->second.kind != ResourceKind::Texture2D ||
            it->second.image == VK_NULL_HANDLE || it->second.depthStencil || it->second.mipLevels <= 1 ||
            frameActive_ || commandPool_ == VK_NULL_HANDLE) {
            lastError_ = "Vulkan mip generation handle or resource is invalid";
            return false;
        }
        auto& resource = it->second;
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = commandPool_;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &allocation, &command) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            return false;
        }
        VkImageMemoryBarrier firstSource{};
        firstSource.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        const auto usageIt = imageUsages_.find(resource.image);
        const auto previousUsage = usageIt == imageUsages_.end() ? ResourceUsage::Unknown : usageIt->second;
        firstSource.oldLayout = previousUsage == ResourceUsage::Unknown ? VK_IMAGE_LAYOUT_UNDEFINED : layout_of(previousUsage);
        firstSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        firstSource.srcAccessMask = access_of(previousUsage);
        firstSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        firstSource.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        firstSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        firstSource.image = resource.image;
        firstSource.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier firstDestination = firstSource;
        firstDestination.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        firstDestination.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        firstDestination.srcAccessMask = 0;
        firstDestination.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        firstDestination.subresourceRange.baseMipLevel = 1;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, 2, std::array<VkImageMemoryBarrier, 2>{firstSource, firstDestination}.data());
        std::int32_t sourceWidth = static_cast<std::int32_t>(resource.width);
        std::int32_t sourceHeight = static_cast<std::int32_t>(resource.height);
        for (std::uint32_t level = 1; level < resource.mipLevels; ++level) {
            const auto destinationWidth = std::max(1, sourceWidth / 2);
            const auto destinationHeight = std::max(1, sourceHeight / 2);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
            blit.srcOffsets[1] = {sourceWidth, sourceHeight, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            blit.dstOffsets[1] = {destinationWidth, destinationHeight, 1};
            vkCmdBlitImage(command, resource.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                resource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            VkImageMemoryBarrier sourceAfter{};
            sourceAfter.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            sourceAfter.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            sourceAfter.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            sourceAfter.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            sourceAfter.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sourceAfter.image = resource.image;
            sourceAfter.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1, 0, 1};
            VkImageMemoryBarrier destinationAfter = sourceAfter;
            destinationAfter.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            destinationAfter.newLayout = level + 1 < resource.mipLevels ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            destinationAfter.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            destinationAfter.dstAccessMask = level + 1 < resource.mipLevels ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_READ_BIT;
            destinationAfter.subresourceRange.baseMipLevel = level;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                level + 1 < resource.mipLevels ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 2, std::array<VkImageMemoryBarrier, 2>{sourceAfter, destinationAfter}.data());
            sourceWidth = destinationWidth;
            sourceHeight = destinationHeight;
        }
        if (vkEndCommandBuffer(command) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &command);
            return false;
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        const bool submitted = vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
        if (submitted) vkQueueWaitIdle(graphicsQueue_);
        vkFreeCommandBuffers(device_, commandPool_, 1, &command);
        if (!submitted) return false;
        imageUsages_[resource.image] = ResourceUsage::ShaderRead;
        resource.usage = ResourceUsage::ShaderRead;
        return true;
    }
    BindlessTableHandle create_bindless_table(const BindlessTableDesc& description) override {
        if (!descriptorIndexingEnabled_ || descriptorPool_ == VK_NULL_HANDLE || bindlessLayout_ == VK_NULL_HANDLE ||
            description.type != DescriptorType::Texture || description.capacity == 0 || description.capacity > 4096) {
            lastError_ = "Vulkan bindless tables currently require texture descriptors with capacity 1..4096";
            return {};
        }
        std::uint32_t variableCount = description.capacity;
        VkDescriptorSetVariableDescriptorCountAllocateInfo variableInfo{};
        variableInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
        variableInfo.descriptorSetCount = 1;
        variableInfo.pDescriptorCounts = &variableCount;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (!allocate_descriptor_sets(1, &bindlessLayout_, &set, pool, &variableInfo)) {
            lastError_ = "Vulkan bindless descriptor set allocation failed";
            return {};
        }
        const auto id = nextBindlessTable_++;
        bindlessTables_[id] = {set, bindlessLayout_, pool, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, description.capacity};
        return {id, 0, description.capacity};
    }
    bool update_bindless(BindlessTableHandle table, std::uint32_t slot, ResourceHandle resource, DescriptorType type) override {
        const auto tableIt = bindlessTables_.find(table.id);
        const auto resourceIt = resources_.find(resource.id);
        if (tableIt == bindlessTables_.end() || slot >= tableIt->second.capacity || resourceIt == resources_.end() ||
            type != DescriptorType::Texture || tableIt->second.descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            lastError_ = "Vulkan bindless descriptor update type or slot is invalid";
            return false;
        }
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = tableIt->second.set;
        write.dstBinding = 0;
        write.dstArrayElement = slot;
        write.descriptorCount = 1;
        VkDescriptorImageInfo image{};
        if (type == DescriptorType::Texture && resourceIt->second.view != VK_NULL_HANDLE) {
            image.sampler = defaultSampler_;
            image.imageView = resourceIt->second.view;
            image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image;
        } else {
            lastError_ = "Vulkan bindless resource has no compatible native view";
            return false;
        }
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        return true;
    }
    void destroy_bindless_table(BindlessTableHandle table) override {
        const auto it = bindlessTables_.find(table.id);
        if (it == bindlessTables_.end()) return;
        if (boundBindlessTable_ == table.id) boundBindlessTable_ = 0;
        retiredBindlessTables_.push_back(it->second);
        bindlessTables_.erase(it);
    }
    void bind_bindless_table(BindlessTableHandle table) override {
        if (bindlessTables_.find(table.id) == bindlessTables_.end()) return;
        boundBindlessTable_ = table.id;
        const auto pipeline = boundPipeline_;
        const auto it = bindlessTables_.find(table.id);
        if (pipeline && it != bindlessTables_.end() && pipeline->pipelineLayout != VK_NULL_HANDLE && frameActive_) {
            const auto bindPoint = pipeline->pipeline.computeShader ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
            vkCmdBindDescriptorSets(recording_command(), bindPoint,
                pipeline->pipelineLayout, 0, 1, &it->second.set, 0, nullptr);
        }
    }
    void retire_resource(ResourceHandle handle) override {
        const auto it = resources_.find(handle.id);
        if (it == resources_.end() || it->second.kind != handle.kind) return;
        if (boundPipeline_ == &it->second) boundPipeline_ = nullptr;
        if (boundMaterial_ == &it->second) boundMaterial_ = nullptr;
        retiredResources_.push_back(std::move(it->second));
        resources_.erase(it);
    }
    void collect_garbage() override {
        if (device_ == VK_NULL_HANDLE) return;
        if (frameFence_ != VK_NULL_HANDLE && vkGetFenceStatus(device_, frameFence_) != VK_SUCCESS) return;
        release_completed_uploads();
        clear_dynamic_framebuffers();
        for (auto& resource : retiredResources_) {
            release_resource(resource);
            ++stats_.resourceDestroys;
        }
        retiredResources_.clear();
        for (auto& table : retiredBindlessTables_) {
            if (table.set != VK_NULL_HANDLE && table.pool != VK_NULL_HANDLE) {
                vkFreeDescriptorSets(device_, table.pool, 1, &table.set);
            }
        }
        retiredBindlessTables_.clear();
        for (auto& descriptorSets : retiredDescriptorSets_) {
            if (descriptorSets.pool != VK_NULL_HANDLE && !descriptorSets.sets.empty()) {
                vkFreeDescriptorSets(device_, descriptorSets.pool,
                    static_cast<std::uint32_t>(descriptorSets.sets.size()), descriptorSets.sets.data());
            }
        }
        retiredDescriptorSets_.clear();
    }
    bool wait_for_frame_writes() {
        if (capabilities_.deviceState!=RenderDeviceState::Ready || frameActive_) return false;
        const auto wait=[&](VkFence fence) {
            if (fence==VK_NULL_HANDLE) return true;
            if (vkWaitForFences(device_,1,&fence,VK_TRUE,5'000'000'000ull)==VK_SUCCESS) return true;
            lastError_="Vulkan frame-resource fence wait failed before host upload";
            capabilities_.deviceState=RenderDeviceState::Lost;return false;
        };
        if (!wait(frameFence_)) return false;
        for (const auto fence:batchFences_) if (!wait(fence)) return false;
        return true;
    }
    void begin_upload_batch() override {
        uploadBatchActive_ = wait_for_frame_writes();
        release_completed_uploads();
    }
    bool flush_upload_batch() override {
        uploadBatchActive_ = false;
        if (!timelineSemaphoreSupported_) {
            for (auto& upload : pendingUploads_) {
                if (vkWaitForFences(device_, 1, &upload.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
                    lastError_ = "Vulkan upload fence wait failed on a device without timeline semaphores";
                    return false;
                }
            }
        }
        return true;
    }
    static VkImageLayout layout_of(ResourceUsage usage) {
        if (usage == ResourceUsage::ShaderRead) return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (usage == ResourceUsage::ShaderWrite || usage == ResourceUsage::StorageRead || usage == ResourceUsage::StorageWrite) return VK_IMAGE_LAYOUT_GENERAL;
        if (is_color_attachment_usage(usage)) return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (usage == ResourceUsage::DepthStencil) return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (usage == ResourceUsage::CopyDestination) return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        if (usage == ResourceUsage::CopySource) return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if (usage == ResourceUsage::Present) return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        return VK_IMAGE_LAYOUT_GENERAL;
    }
    static VkAccessFlags access_of(ResourceUsage usage) {
        if (usage == ResourceUsage::ShaderRead || usage == ResourceUsage::StorageRead) return VK_ACCESS_SHADER_READ_BIT;
        if (usage == ResourceUsage::ShaderWrite || usage == ResourceUsage::StorageWrite) return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        if (is_color_attachment_usage(usage)) return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        if (usage == ResourceUsage::DepthStencil) return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        if (usage == ResourceUsage::CopySource) return VK_ACCESS_TRANSFER_READ_BIT;
        if (usage == ResourceUsage::CopyDestination) return VK_ACCESS_TRANSFER_WRITE_BIT;
        if (usage == ResourceUsage::Present) return VK_ACCESS_MEMORY_READ_BIT;
        return 0;
    }
    static VkPipelineStageFlags stage_of(ResourceUsage usage) {
        if (usage == ResourceUsage::ShaderRead || usage == ResourceUsage::ShaderWrite ||
            usage == ResourceUsage::StorageRead || usage == ResourceUsage::StorageWrite) {
            return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
        if (is_color_attachment_usage(usage)) return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        if (usage == ResourceUsage::DepthStencil) return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        if (usage == ResourceUsage::CopySource || usage == ResourceUsage::CopyDestination) return VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (usage == ResourceUsage::Present) return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    VkPipelineStageFlags queue_stage_mask(ResourceUsage usage) const {
        const auto stages = stage_of(usage);
        if (activeQueueKind_ == RenderQueue::Copy) return VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (activeQueueKind_ == RenderQueue::Compute) {
            if (stages == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            if (stages == VK_PIPELINE_STAGE_TRANSFER_BIT) return VK_PIPELINE_STAGE_TRANSFER_BIT;
            return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
        return stages;
    }
    VkPipelineStageFlags queue_buffer_stage_mask(ResourceUsage usage) const {
        const auto stages = buffer_stage_of(usage);
        if (activeQueueKind_ == RenderQueue::Copy) return VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (activeQueueKind_ == RenderQueue::Compute) {
            if (stages == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT || stages == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) {
                return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            }
            return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
        return stages;
    }
    static VkAccessFlags buffer_access_of(ResourceUsage usage) {
        if (usage == ResourceUsage::VertexBuffer) return VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        if (usage == ResourceUsage::IndexBuffer) return VK_ACCESS_INDEX_READ_BIT;
        if (usage == ResourceUsage::UniformBuffer) return VK_ACCESS_UNIFORM_READ_BIT;
        if (usage == ResourceUsage::IndirectArguments) return VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        return access_of(usage);
    }
    static VkPipelineStageFlags buffer_stage_of(ResourceUsage usage) {
        if (usage == ResourceUsage::VertexBuffer) return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        if (usage == ResourceUsage::IndexBuffer) return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        if (usage == ResourceUsage::UniformBuffer) return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        if (usage == ResourceUsage::IndirectArguments) return VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
        return stage_of(usage);
    }
    static VkDescriptorType descriptor_type_of(DescriptorType type) {
        if (type == DescriptorType::UniformBuffer) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (type == DescriptorType::StorageBuffer || type == DescriptorType::StructuredBuffer) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (type == DescriptorType::Sampler) return VK_DESCRIPTOR_TYPE_SAMPLER;
        if (type == DescriptorType::StorageTexture) return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    bool create_bindless_layout() {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 4096;
        binding.stageFlags = VK_SHADER_STAGE_ALL;
        const VkDescriptorBindingFlags bindingFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
        flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagsInfo.bindingCount = 1;
        flagsInfo.pBindingFlags = &bindingFlags;
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        layoutInfo.pNext = &flagsInfo;
        return vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &bindlessLayout_) == VK_SUCCESS;
    }
    void read_timestamps() {
        if (!timestampPool_ || submittedTimestampCount_ < 2) return;
        std::vector<std::uint64_t> values(submittedTimestampCount_);
        if (vkGetQueryPoolResults(device_, timestampPool_, 0, submittedTimestampCount_,
            values.size() * sizeof(std::uint64_t), values.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
        stats_.gpuPassNanoseconds.clear();
        stats_.gpuPassNanoseconds.reserve(submittedTimestampCount_ / 2);
        for (std::uint32_t i = 0; i + 1 < submittedTimestampCount_; i += 2) {
            const auto ticks = values[i + 1] >= values[i] ? values[i + 1] - values[i] : 0;
            stats_.gpuPassNanoseconds.push_back(static_cast<std::uint64_t>(ticks * timestampPeriod_));
        }
    }
    bool transition_resource(ResourceHandle handle, ResourceUsage usage) override {
        if (!frameActive_ || activeCommandBuffer_ == VK_NULL_HANDLE) {
            lastError_ = "Vulkan resource transition command buffer is invalid";
            return false;
        }
        const auto it = resources_.find(handle.id);
        if (it == resources_.end() || it->second.kind != handle.kind) {
            lastError_ = "Vulkan resource transition handle is invalid";
            return false;
        }
        auto& resource = it->second;
        if (resource.buffer != VK_NULL_HANDLE) {
            const auto state = bufferUsages_.find(resource.buffer);
            const auto previous = state == bufferUsages_.end() ? ResourceUsage::Unknown : state->second;
            if (!record_ownership_transfer(handle, resource, previous, usage)) return false;
            if (previous != usage || usage==ResourceUsage::StorageWrite || usage==ResourceUsage::ShaderWrite) {
                VkBufferMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                barrier.srcAccessMask = buffer_access_of(previous);
                barrier.dstAccessMask = buffer_access_of(usage);
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.buffer = resource.buffer;
                barrier.offset = 0;
                barrier.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(activeCommandBuffer_, queue_buffer_stage_mask(previous), queue_buffer_stage_mask(usage),
                    0, 0, nullptr, 1, &barrier, 0, nullptr);
                ++stats_.barriers;
            }
            bufferUsages_[resource.buffer] = usage;
            resource.usage = usage;
            resource.ownerQueue = activeQueueKind_;
            resource.ownerBatch = currentBatchIndex_;
            return true;
        }
        if (resource.image != VK_NULL_HANDLE) {
            const auto state = imageUsages_.find(resource.image);
            const auto previous = state == imageUsages_.end() ? ResourceUsage::Unknown : state->second;
            if (!record_ownership_transfer(handle, resource, previous, usage)) return false;
            if (previous == usage && usage!=ResourceUsage::StorageWrite && usage!=ResourceUsage::ShaderWrite) {
                resource.usage = usage;
                resource.ownerQueue = activeQueueKind_;
                resource.ownerBatch = currentBatchIndex_;
                return true;
            }
            if (renderPassActive_) {
                vkCmdEndRenderPass(activeCommandBuffer_);
                renderPassActive_ = false;
                activeRenderPass_ = VK_NULL_HANDLE;
            }
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = previous == ResourceUsage::Unknown ? VK_IMAGE_LAYOUT_UNDEFINED : layout_of(previous);
            barrier.newLayout = layout_of(usage);
            barrier.srcAccessMask = access_of(previous);
            barrier.dstAccessMask = access_of(usage);
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = resource.image;
            barrier.subresourceRange.aspectMask = image_aspect_mask(resource);
            barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
            barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
            vkCmdPipelineBarrier(activeCommandBuffer_, queue_stage_mask(previous), queue_stage_mask(usage),
                0, 0, nullptr, 0, nullptr, 1, &barrier);
            ++stats_.barriers;
            imageUsages_[resource.image] = usage;
            resource.usage = usage;
            resource.ownerQueue = activeQueueKind_;
            resource.ownerBatch = currentBatchIndex_;
            return true;
        }
        lastError_ = "Vulkan resource transition has no native image or buffer";
        return false;
    }
    void set_render_target(ResourceHandle target) override {
        if (!frameActive_ || commandBuffers_.empty()) return;
        if (renderPassActive_ && currentRenderTarget_ == target.id) return;
        const auto command = activeCommandBuffer_;
        if (renderPassActive_) {
            vkCmdEndRenderPass(command);
            renderPassActive_ = false;
            activeRenderPass_ = VK_NULL_HANDLE;
        }
        VkClearValue clear[2]{};
        clear[0].color = {{0.035f, 0.045f, 0.065f, 1.0f}};
        clear[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo renderPassBegin{};
        renderPassBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassBegin.renderPass = renderPass_;
        renderPassBegin.clearValueCount = 2;
        renderPassBegin.pClearValues = clear;
        if (target) {
            const auto it = resources_.find(target.id);
            if (it == resources_.end() || it->second.framebuffer == VK_NULL_HANDLE) {
                lastError_ = "Vulkan render target has no compatible framebuffer";
                return;
            }
            renderPassBegin.framebuffer = it->second.framebuffer;
            renderPassBegin.renderArea.extent = {it->second.width, it->second.height};
        } else {
            renderPassBegin.framebuffer = framebuffers_[imageIndex_];
            renderPassBegin.renderArea.extent = swapchainExtent_;
        }
        vkCmdBeginRenderPass(command, &renderPassBegin, VK_SUBPASS_CONTENTS_INLINE);
        const auto extent = target ? VkExtent2D{resources_[target.id].width, resources_[target.id].height} : swapchainExtent_;
        VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        renderPassActive_ = true;
        activeRenderPass_ = renderPass_;
        currentRenderTarget_ = target.id;
    }
    void set_render_targets(ResourceHandle color, ResourceHandle depth) override {
        set_render_targets(color, depth, true);
    }
    void set_render_targets(ResourceHandle color, ResourceHandle depth, bool clearAttachments) override {
        if (!frameActive_ || commandBuffers_.empty()) return;
        const auto colorIt = resources_.find(color.id);
        const auto depthIt = resources_.find(depth.id);
        if (!color && depth && depthIt != resources_.end() && depthIt->second.view != VK_NULL_HANDLE) {
            const auto renderPass = get_or_create_color_render_pass({}, depthIt->second.format,
                depthIt->second.samples, clearAttachments);
            const auto loadRenderPass = get_or_create_color_render_pass({}, depthIt->second.format,
                depthIt->second.samples, false);
            if (renderPass == VK_NULL_HANDLE || loadRenderPass == VK_NULL_HANDLE) {
                lastError_ = "Vulkan depth-only render pass creation failed";
                return;
            }
            const auto renderPassKey = reinterpret_cast<std::uintptr_t>(renderPass);
            const auto key = static_cast<std::uint64_t>(renderPassKey) * 257ull +
                reinterpret_cast<std::uint64_t>(depthIt->second.view);
            auto framebufferIt = dynamicFramebuffers_.find(key);
            if (framebufferIt == dynamicFramebuffers_.end()) {
                const VkImageView attachments[] = {depthIt->second.view};
                VkFramebufferCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                info.renderPass = renderPass;
                info.attachmentCount = 1;
                info.pAttachments = attachments;
                info.width = depthIt->second.width;
                info.height = depthIt->second.height;
                info.layers = 1;
                VkFramebuffer framebuffer = VK_NULL_HANDLE;
                if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) == VK_SUCCESS) framebufferIt = dynamicFramebuffers_.emplace(key, framebuffer).first;
            }
            if (framebufferIt != dynamicFramebuffers_.end()) {
                if (renderPassActive_) vkCmdEndRenderPass(activeCommandBuffer_);
                renderPassActive_ = false;
                activeRenderPass_ = VK_NULL_HANDLE;
                VkClearValue clear{};
                clear.depthStencil = {1.0f, 0};
                VkRenderPassBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                begin.renderPass = clearAttachments ? renderPass : loadRenderPass;
                begin.framebuffer = framebufferIt->second;
                begin.renderArea.extent = {depthIt->second.width, depthIt->second.height};
                begin.clearValueCount = clearAttachments ? 1u : 0u;
                begin.pClearValues = clearAttachments ? &clear : nullptr;
                vkCmdBeginRenderPass(activeCommandBuffer_, &begin, VK_SUBPASS_CONTENTS_INLINE);
                VkViewport viewport{0.0f, 0.0f, static_cast<float>(depthIt->second.width), static_cast<float>(depthIt->second.height), 0.0f, 1.0f};
                VkRect2D scissor{{0, 0}, {depthIt->second.width, depthIt->second.height}};
                vkCmdSetViewport(activeCommandBuffer_, 0, 1, &viewport);
                vkCmdSetScissor(activeCommandBuffer_, 0, 1, &scissor);
                renderPassActive_ = true;
                activeRenderPass_ = begin.renderPass;
                currentRenderTarget_ = depth.id;
            }
            return;
        }
        if (color && depth && colorIt != resources_.end() && depthIt != resources_.end()) {
            if (colorIt->second.view == VK_NULL_HANDLE || depthIt->second.view == VK_NULL_HANDLE) {
                lastError_ = "Vulkan color/depth target has no image view";
                return;
            }
            if (colorIt->second.samples != depthIt->second.samples ||
                colorIt->second.width != depthIt->second.width || colorIt->second.height != depthIt->second.height) {
                lastError_ = "Vulkan color/depth targets must have matching extent and sample count";
                return;
            }
            const auto renderPass = get_or_create_color_render_pass({colorIt->second.format}, depthIt->second.format,
                colorIt->second.samples, clearAttachments);
            if (renderPass == VK_NULL_HANDLE) {
                lastError_ = "Vulkan color/depth render pass creation failed";
                return;
            }
            const auto renderPassKey = reinterpret_cast<std::uintptr_t>(renderPass);
            const auto key = static_cast<std::uint64_t>(renderPassKey) * 257ull +
                reinterpret_cast<std::uint64_t>(colorIt->second.view) * 257ull +
                reinterpret_cast<std::uint64_t>(depthIt->second.view);
            auto framebufferIt = dynamicFramebuffers_.find(key);
            if (framebufferIt == dynamicFramebuffers_.end()) {
                const VkImageView attachments[] = {colorIt->second.view, depthIt->second.view};
                VkFramebufferCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                info.renderPass = renderPass;
                info.attachmentCount = 2;
                info.pAttachments = attachments;
                info.width = colorIt->second.width;
                info.height = colorIt->second.height;
                info.layers = 1;
                VkFramebuffer framebuffer = VK_NULL_HANDLE;
                if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) == VK_SUCCESS) framebufferIt = dynamicFramebuffers_.emplace(key, framebuffer).first;
            }
            if (framebufferIt != dynamicFramebuffers_.end()) {
                if (renderPassActive_) vkCmdEndRenderPass(activeCommandBuffer_);
                renderPassActive_ = false;
                activeRenderPass_ = VK_NULL_HANDLE;
                VkClearValue clear[2]{};
                clear[0].color = {{0, 0, 0, 1}};
                clear[1].depthStencil = {1.0f, 0};
                VkRenderPassBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                begin.renderPass = renderPass;
                begin.framebuffer = framebufferIt->second;
                begin.renderArea.extent = {colorIt->second.width, colorIt->second.height};
                begin.clearValueCount = clearAttachments ? 2u : 0u;
                begin.pClearValues = clearAttachments ? clear : nullptr;
                vkCmdBeginRenderPass(activeCommandBuffer_, &begin, VK_SUBPASS_CONTENTS_INLINE);
                VkViewport viewport{0.0f, 0.0f, static_cast<float>(colorIt->second.width), static_cast<float>(colorIt->second.height), 0.0f, 1.0f};
                VkRect2D scissor{{0, 0}, {colorIt->second.width, colorIt->second.height}};
                vkCmdSetViewport(activeCommandBuffer_, 0, 1, &viewport);
                vkCmdSetScissor(activeCommandBuffer_, 0, 1, &scissor);
                renderPassActive_ = true;
                activeRenderPass_ = begin.renderPass;
                currentRenderTarget_ = color.id;
            }
            return;
        }
        set_render_target(color);
    }
    void set_render_targets(const std::vector<ResourceHandle>& colors, ResourceHandle depth, bool clearAttachments) override {
        if (!frameActive_ || activeCommandBuffer_ == VK_NULL_HANDLE) return;
        const auto singleColor = colors.size() == 1 ? resources_.find(colors.front().id) : resources_.end();
        const bool useDynamicSingleColor = singleColor != resources_.end() && singleColor->second.view != VK_NULL_HANDLE &&
            singleColor->second.framebuffer == VK_NULL_HANDLE;
        if (colors.empty() || (colors.size() == 1 && !useDynamicSingleColor)) {
            set_render_targets(colors.empty() ? ResourceHandle{} : colors.front(), depth, clearAttachments);
            return;
        }
        std::vector<ResourceRecord*> colorResources;
        std::vector<VkImageView> attachments;
        std::vector<VkFormat> formats;
        for (const auto handle : colors) {
            const auto it = resources_.find(handle.id);
            if (it == resources_.end() || it->second.view == VK_NULL_HANDLE) {
                lastError_ = "Vulkan color attachment handle or image view is invalid";
                return;
            }
            colorResources.push_back(&it->second);
            attachments.push_back(it->second.view);
            formats.push_back(it->second.format);
        }
        ResourceRecord* depthResource = nullptr;
        if (depth) {
            const auto it = resources_.find(depth.id);
            if (it == resources_.end() || it->second.view == VK_NULL_HANDLE) {
                lastError_ = "Vulkan depth attachment handle or image view is invalid";
                return;
            }
            depthResource = &it->second;
            attachments.push_back(it->second.view);
        }
        if (formats.empty() && !depthResource) {
            set_render_target({});
            return;
        }
        const auto samples = colorResources.empty() ? depthResource->samples : colorResources.front()->samples;
        const auto width = colorResources.empty() ? depthResource->width : colorResources.front()->width;
        const auto height = colorResources.empty() ? depthResource->height : colorResources.front()->height;
        for (const auto* resource : colorResources) {
            if (resource->samples != samples || resource->width != width || resource->height != height) {
                lastError_ = "Vulkan color attachments must have matching extent and sample count";
                return;
            }
        }
        if (depthResource && (depthResource->samples != samples || depthResource->width != width || depthResource->height != height)) {
            lastError_ = "Vulkan depth attachment must match color extent and sample count";
            return;
        }
        const auto renderPass = get_or_create_color_render_pass(formats,
            depthResource ? depthResource->format : VK_FORMAT_UNDEFINED, samples, clearAttachments);
        if (renderPass == VK_NULL_HANDLE) return;
        const auto renderPassKey = reinterpret_cast<std::uintptr_t>(renderPass);
        std::uint64_t key = static_cast<std::uint64_t>(renderPassKey);
        for (const auto view : attachments) key = key * 257ull + reinterpret_cast<std::uint64_t>(view);
        auto framebufferIt = dynamicFramebuffers_.find(key);
        if (framebufferIt == dynamicFramebuffers_.end()) {
            VkFramebufferCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            info.renderPass = renderPass;
            info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
            info.pAttachments = attachments.data();
            info.width = width;
            info.height = height;
            info.layers = 1;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) != VK_SUCCESS) return;
            framebufferIt = dynamicFramebuffers_.emplace(key, framebuffer).first;
        }
        if (renderPassActive_) vkCmdEndRenderPass(activeCommandBuffer_);
        renderPassActive_ = false;
        std::vector<VkClearValue> clears(attachments.size());
        for (std::size_t index = 0; index < colorResources.size(); ++index) clears[index].color = {{0, 0, 0, 1}};
        if (depthResource) clears.back().depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = renderPass;
        begin.framebuffer = framebufferIt->second;
        begin.renderArea.extent = {width, height};
        begin.clearValueCount = clearAttachments ? static_cast<std::uint32_t>(clears.size()) : 0u;
        begin.pClearValues = clearAttachments ? clears.data() : nullptr;
        vkCmdBeginRenderPass(activeCommandBuffer_, &begin, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {width, height}};
        vkCmdSetViewport(activeCommandBuffer_, 0, 1, &viewport);
        vkCmdSetScissor(activeCommandBuffer_, 0, 1, &scissor);
        renderPassActive_ = true;
        activeRenderPass_ = renderPass;
        currentRenderTarget_ = colors.empty() ? depth.id : colors.front().id;
    }
    bool clear_render_attachments(const std::vector<ResourceHandle>& colors,ResourceHandle depth,bool clearColors,bool clearDepth) override {
        if (!renderPassActive_) return false;
        std::vector<VkClearAttachment> attachments;
        if (clearColors) for (std::size_t index=0;index<colors.size();++index) {
            VkClearAttachment clear{}; clear.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
            clear.colorAttachment=static_cast<std::uint32_t>(index); clear.clearValue.color={{0,0,0,1}};
            attachments.push_back(clear);
        }
        if (clearDepth && depth) {
            VkClearAttachment clear{}; clear.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
            const auto found=resources_.find(depth.id);
            if (found==resources_.end()) return false;
            if (found->second.format==VK_FORMAT_D24_UNORM_S8_UINT) clear.aspectMask|=VK_IMAGE_ASPECT_STENCIL_BIT;
            clear.clearValue.depthStencil={1,0}; attachments.push_back(clear);
        }
        const auto target=resources_.find(colors.empty()?depth.id:colors.front().id);
        if (target==resources_.end()) return false;
        VkClearRect rectangle{{{0,0},{target->second.width,target->second.height}},0,1};
        if (!attachments.empty()) vkCmdClearAttachments(recording_command(),static_cast<std::uint32_t>(attachments.size()),attachments.data(),1,&rectangle);
        return true;
    }
    void set_viewport(float x, float y, float width, float height, float minDepth, float maxDepth) override {
        if (!frameActive_ || activeCommandBuffer_ == VK_NULL_HANDLE) return;
        VkViewport viewport{x, y, width, height, minDepth, maxDepth};
        VkRect2D scissor{
            {static_cast<int32_t>(std::floor(x)), static_cast<int32_t>(std::floor(y))},
            {static_cast<std::uint32_t>(std::max(0.0f, std::ceil(width))),
             static_cast<std::uint32_t>(std::max(0.0f, std::ceil(height)))}};
        vkCmdSetViewport(activeCommandBuffer_, 0, 1, &viewport);
        vkCmdSetScissor(activeCommandBuffer_, 0, 1, &scissor);
    }
    void begin_frame() override {
        ++stats_.frames;
        if (!swapchain_) {
            if (frameFence_ != VK_NULL_HANDLE) {
                const auto waitResult = vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, 5'000'000'000ull);
                if (waitResult != VK_SUCCESS) {
                lastError_ = "Vulkan off-screen frame fence wait failed";
                capabilities_.deviceState = RenderDeviceState::Lost;
                return;
                }
            }
            for (const auto fence : batchFences_) {
                if (fence == VK_NULL_HANDLE) continue;
                const auto waitResult = vkWaitForFences(device_, 1, &fence, VK_TRUE, 5'000'000'000ull);
                if (waitResult != VK_SUCCESS) {
                    lastError_ = "Vulkan off-screen batch fence wait failed result=" +
                        std::to_string(static_cast<int>(waitResult));
                    capabilities_.deviceState = waitResult == VK_ERROR_DEVICE_LOST
                        ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
                    return;
                }
            }
            read_timestamps();
            collect_garbage();
            for (const auto semaphore : batchSignals_) {
                if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore, nullptr);
            }
            batchSignals_.clear();
            for (const auto fence : batchFences_) {
                if (fence != VK_NULL_HANDLE) vkDestroyFence(device_, fence, nullptr);
            }
            batchFences_.clear();
            if (commandPool_ != VK_NULL_HANDLE && vkResetCommandPool(device_, commandPool_, 0) != VK_SUCCESS) {
                lastError_ = "Vulkan off-screen command pool reset failed";
                capabilities_.deviceState = RenderDeviceState::Lost;
                return;
            }
            if (computeCommandPool_ != VK_NULL_HANDLE) vkResetCommandPool(device_, computeCommandPool_, 0);
            if (copyCommandPool_ != VK_NULL_HANDLE) vkResetCommandPool(device_, copyCommandPool_, 0);
            if (frameFence_ != VK_NULL_HANDLE) vkResetFences(device_, 1, &frameFence_);
            timestampCursor_ = 0;
            timestampPoolResetThisFrame_ = false;
            frameActive_ = true;
            activeCommandBuffer_ = VK_NULL_HANDLE;
            transientBatchCommands_.clear();
            currentBatchIndex_ = 0;
            currentRenderTarget_ = 0;
            return;
        }
        const auto frameWaitResult = vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, 5'000'000'000ull);
        if (frameWaitResult != VK_SUCCESS) {
            lastError_ = "Vulkan frame fence wait failed result=" + std::to_string(static_cast<int>(frameWaitResult));
            capabilities_.deviceState = frameWaitResult == VK_ERROR_DEVICE_LOST ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
            return;
        }
        for (const auto fence : batchFences_) {
            if (fence != VK_NULL_HANDLE) vkWaitForFences(device_, 1, &fence, VK_TRUE, 5'000'000'000ull);
        }
        // Upload command buffers use the graphics command pool. Reclaim them
        // at the batch boundary before resetting that pool for this frame.
        release_completed_uploads(true);
        read_timestamps();
        collect_garbage();
        for (const auto semaphore : batchSignals_) {
            if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore, nullptr);
        }
        batchSignals_.clear();
        for (const auto fence : batchFences_) {
            if (fence != VK_NULL_HANDLE) vkDestroyFence(device_, fence, nullptr);
        }
        batchFences_.clear();
        if (commandPool_ != VK_NULL_HANDLE) vkResetCommandPool(device_, commandPool_, 0);
        if (computeCommandPool_ != VK_NULL_HANDLE) vkResetCommandPool(device_, computeCommandPool_, 0);
        if (copyCommandPool_ != VK_NULL_HANDLE) vkResetCommandPool(device_, copyCommandPool_, 0);
        vkResetFences(device_, 1, &frameFence_);
        const auto acquireResult = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable_, VK_NULL_HANDLE, &imageIndex_);
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
            lastError_ = "Vulkan acquire failed result=" + std::to_string(static_cast<int>(acquireResult));
            capabilities_.deviceState = acquireResult == VK_ERROR_DEVICE_LOST ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
            return;
        }
        timestampCursor_ = 0;
        timestampPoolResetThisFrame_ = false;
        frameActive_ = true;
        activeCommandBuffer_ = VK_NULL_HANDLE;
        transientBatchCommands_.clear();
        currentBatchIndex_ = 0;
        swapchainPrepared_ = false;
        currentBatchWaitsForAcquire_ = false;
        currentRenderTarget_ = 0;
    }
    bool begin_queue(RenderQueue queue) override {
        return begin_queue(queue, currentBatchIndex_, {});
    }
    bool begin_queue(RenderQueue queue, std::uint32_t batchIndex,
                     const std::vector<std::uint32_t>& waitBatches) override {
        if (!frameActive_ || device_ == VK_NULL_HANDLE) return false;
        if (activeCommandBuffer_ != VK_NULL_HANDLE) return false;
        currentBatchIndex_ = batchIndex;
        currentWaitBatches_ = waitBatches;
        for (const auto waitBatch : currentWaitBatches_) {
            const bool isPendingProducer = pendingCommandBuffer_ != VK_NULL_HANDLE && waitBatch == pendingBatchIndex_;
            if (!isPendingProducer && (waitBatch >= batchSignals_.size() || batchSignals_[waitBatch] == VK_NULL_HANDLE)) {
                lastError_ = "Vulkan queue batch wait references an unsignaled batch";
                return false;
            }
        }
        const auto pool = pool_for(queue);
        if (pool == VK_NULL_HANDLE) return false;
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device_, &allocation, &command) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, pool, 1, &command);
            return false;
        }
        activeCommandBuffer_ = command;
        activeQueue_ = queue_for(queue);
        activeQueueKind_ = queue;
        currentBatchWaitsForAcquire_ = queue == RenderQueue::Graphics && !swapchainPrepared_ &&
            swapchain_ != VK_NULL_HANDLE && imageAvailable_ != VK_NULL_HANDLE;
        transientBatchCommands_.push_back(command);
        if (timestampPool_ && queue != RenderQueue::Copy && !timestampPoolResetThisFrame_) {
            vkCmdResetQueryPool(command, timestampPool_, 0, 512);
            timestampPoolResetThisFrame_ = true;
        }
        if (queue == RenderQueue::Graphics && !swapchainPrepared_ && swapchain_ != VK_NULL_HANDLE) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.oldLayout = swapchainInitialized_[imageIndex_] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.image = swapchainImages_[imageIndex_];
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);
            swapchainPrepared_ = true;
            swapchainInitialized_[imageIndex_] = true;
        }
        return true;
    }
    bool end_queue(RenderQueue queue, std::uint32_t batchIndex, bool lastBatch, bool present) override {
        if (activeCommandBuffer_ == VK_NULL_HANDLE || activeQueue_ == VK_NULL_HANDLE) return false;
        if (renderPassActive_) {
            vkCmdEndRenderPass(activeCommandBuffer_);
            renderPassActive_ = false;
            activeRenderPass_ = VK_NULL_HANDLE;
            currentRenderTarget_ = 0;
        }
        if (present && swapchain_ != VK_NULL_HANDLE && activeQueueKind_ == RenderQueue::Graphics) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = 0;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.image = swapchainImages_[imageIndex_];
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(activeCommandBuffer_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
        if (!lastBatch && !present) {
            if (!finalize_pending_queue_batch()) return false;
            pendingCommandBuffer_ = activeCommandBuffer_;
            pendingQueue_ = activeQueue_;
            pendingCommandPool_ = pool_for(activeQueueKind_);
            pendingQueueKind_ = activeQueueKind_;
            pendingBatchIndex_ = batchIndex;
            pendingWaitBatches_ = currentWaitBatches_;
            pendingBatchUploadWaitValue_ = pendingUploadWaitValue_;
            pendingBatchWaitsForAcquire_ = currentBatchWaitsForAcquire_;
            pendingUploadWaitValue_ = 0;
            activeCommandBuffer_ = VK_NULL_HANDLE;
            activeQueue_ = VK_NULL_HANDLE;
            currentBatchWaitsForAcquire_ = false;
            (void)queue;
            return capabilities_.deviceState == RenderDeviceState::Ready;
        }
        if (!finalize_pending_queue_batch()) return false;
        const auto endResult = vkEndCommandBuffer(activeCommandBuffer_);
        if (endResult != VK_SUCCESS) {
            lastError_ = "Vulkan command buffer finalization failed result=" + std::to_string(static_cast<int>(endResult));
            capabilities_.deviceState = endResult == VK_ERROR_DEVICE_LOST ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
            activeCommandBuffer_ = VK_NULL_HANDLE;
            return false;
        }
        std::vector<VkSemaphore> waits;
        std::vector<VkPipelineStageFlags> stages;
        if (currentBatchWaitsForAcquire_) {
            waits.push_back(imageAvailable_);
            stages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
        for (const auto waitBatch : currentWaitBatches_) {
            if (waitBatch < batchSignals_.size() && batchSignals_[waitBatch] != VK_NULL_HANDLE) {
                waits.push_back(batchSignals_[waitBatch]);
                stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            }
        }
        if (pendingUploadWaitValue_ != 0 && uploadTimeline_ != VK_NULL_HANDLE) {
            waits.push_back(uploadTimeline_);
            stages.push_back(activeQueueKind_ == RenderQueue::Copy ? VK_PIPELINE_STAGE_TRANSFER_BIT :
                (activeQueueKind_ == RenderQueue::Compute ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));
        }
        VkSemaphore signal = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &signal) != VK_SUCCESS) {
            activeCommandBuffer_ = VK_NULL_HANDLE;
            return false;
        }
        if (batchSignals_.size() <= batchIndex) batchSignals_.resize(batchIndex + 1, VK_NULL_HANDLE);
        batchSignals_[batchIndex] = signal;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = static_cast<std::uint32_t>(waits.size());
        submit.pWaitSemaphores = waits.data();
        submit.pWaitDstStageMask = stages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &activeCommandBuffer_;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &signal;
        VkTimelineSemaphoreSubmitInfo timelineWait{};
        if (pendingUploadWaitValue_ != 0 && uploadTimeline_ != VK_NULL_HANDLE) {
            timelineWait.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timelineWait.waitSemaphoreValueCount = static_cast<std::uint32_t>(waits.size());
            std::vector<std::uint64_t> waitValues(waits.size(), 0);
            waitValues.back() = pendingUploadWaitValue_;
            timelineWait.pWaitSemaphoreValues = waitValues.data();
            submit.pNext = &timelineWait;
            if (lastBatch) pendingUploadWaitValue_ = 0;
        }
        VkFence batchFence = frameFence_;
        if (!lastBatch) {
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (vkCreateFence(device_, &fenceInfo, nullptr, &batchFence) != VK_SUCCESS) {
                vkDestroySemaphore(device_, signal, nullptr);
                activeCommandBuffer_ = VK_NULL_HANDLE;
                return false;
            }
        }
        const auto submitResult = vkQueueSubmit(activeQueue_, 1, &submit, batchFence);
        if (submitResult != VK_SUCCESS) {
            lastError_ = "Vulkan queue submission failed";
            capabilities_.deviceState = RenderDeviceState::Lost;
            if (!lastBatch) vkDestroyFence(device_, batchFence, nullptr);
            batchSignals_[batchIndex] = VK_NULL_HANDLE;
            vkDestroySemaphore(device_, signal, nullptr);
            activeCommandBuffer_ = VK_NULL_HANDLE;
            activeQueue_ = VK_NULL_HANDLE;
            currentBatchWaitsForAcquire_ = false;
            return false;
        } else {
            ++stats_.queueSubmissions;
            if (!lastBatch) batchFences_.push_back(batchFence);
            if (lastBatch || present) submittedTimestampCount_ = timestampCursor_;
            if (lastBatch) pendingUploadWaitValue_ = 0;
        }
        activeCommandBuffer_ = VK_NULL_HANDLE;
        activeQueue_ = VK_NULL_HANDLE;
        currentBatchWaitsForAcquire_ = false;
        if (present && swapchain_ != VK_NULL_HANDLE) {
            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &signal;
            present.swapchainCount = 1;
            present.pSwapchains = &swapchain_;
            present.pImageIndices = &imageIndex_;
            const auto result = vkQueuePresentKHR(presentQueue_, &present);
            if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
                lastError_ = "Vulkan present failed result=" + std::to_string(static_cast<int>(result));
                capabilities_.deviceState = result == VK_ERROR_DEVICE_LOST ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
            }
        }
        (void)queue;
        return capabilities_.deviceState == RenderDeviceState::Ready;
    }
    void execute(const RenderPassContext&) override { ++stats_.passes; }
    void begin_pass() override {
        if (timestampPool_ && activeQueueKind_ != RenderQueue::Copy && frameActive_ && timestampCursor_ < 511) {
            vkCmdWriteTimestamp(recording_command(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampPool_, timestampCursor_++);
        }
    }
    void end_pass() override {
        if (timestampPool_ && activeQueueKind_ != RenderQueue::Copy && frameActive_ && timestampCursor_ < 512) {
            vkCmdWriteTimestamp(recording_command(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampPool_, timestampCursor_++);
        }
    }
    void set_debug_name(ResourceHandle handle, std::string_view name) override {
        debugNames_[handle.id] = std::string(name);
        if (!setDebugUtilsObjectName_ || device_ == VK_NULL_HANDLE) return;
        const auto resource = resources_.find(handle.id);
        if (resource == resources_.end()) return;
        VkObjectType objectType = VK_OBJECT_TYPE_UNKNOWN;
        std::uint64_t objectHandle = 0;
        if (resource->second.buffer != VK_NULL_HANDLE) {
            objectType = VK_OBJECT_TYPE_BUFFER;
            objectHandle = reinterpret_cast<std::uint64_t>(resource->second.buffer);
        } else if (resource->second.image != VK_NULL_HANDLE) {
            objectType = VK_OBJECT_TYPE_IMAGE;
            objectHandle = reinterpret_cast<std::uint64_t>(resource->second.image);
        } else if (resource->second.shader != VK_NULL_HANDLE) {
            objectType = VK_OBJECT_TYPE_SHADER_MODULE;
            objectHandle = reinterpret_cast<std::uint64_t>(resource->second.shader);
        } else if (resource->second.graphicsPipeline != VK_NULL_HANDLE) {
            objectType = VK_OBJECT_TYPE_PIPELINE;
            objectHandle = reinterpret_cast<std::uint64_t>(resource->second.graphicsPipeline);
        }
        if (objectType == VK_OBJECT_TYPE_UNKNOWN) return;
        VkDebugUtilsObjectNameInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        info.objectType = objectType;
        info.objectHandle = objectHandle;
        info.pObjectName = debugNames_[handle.id].c_str();
        setDebugUtilsObjectName_(device_, &info);
    }
    void begin_debug_label(std::string_view name) override {
        ++stats_.debugMarkers;
        if (!cmdBeginDebugUtilsLabel_ || recording_command() == VK_NULL_HANDLE) return;
        VkDebugUtilsLabelEXT label{};
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        const auto& stored = debugLabelStorage_.emplace_back(name);
        label.pLabelName = stored.c_str();
        label.color[0] = 0.2f;
        label.color[1] = 0.6f;
        label.color[2] = 1.0f;
        label.color[3] = 1.0f;
        cmdBeginDebugUtilsLabel_(recording_command(), &label);
    }
    void end_debug_label() override {
        if (cmdEndDebugUtilsLabel_ && recording_command() != VK_NULL_HANDLE) cmdEndDebugUtilsLabel_(recording_command());
        if (!debugLabelStorage_.empty()) debugLabelStorage_.pop_back();
    }
    bool ensure_pipeline(ResourceRecord& record) {
        if (record.computePipeline != VK_NULL_HANDLE) return true;
        const auto activePass = activeRenderPass_ != VK_NULL_HANDLE ? activeRenderPass_ : renderPass_;
        const auto target = resources_.find(currentRenderTarget_);
        const bool dynamicDepthOnlyPass = activeRenderPass_ != VK_NULL_HANDLE &&
            activeRenderPass_ != renderPass_ && target != resources_.end() && target->second.depthStencil;
        const bool depthOnlyPass = activePass == depthOnlyRenderPass_ || activePass == depthOnlyLoadRenderPass_ ||
            dynamicDepthOnlyPass;
        if (record.graphicsPipeline != VK_NULL_HANDLE && record.pipelineRenderPass == activePass) return true;
        if (depthOnlyPass && record.depthOnlyPipeline != VK_NULL_HANDLE && record.pipelineRenderPass == activePass) return true;
        if (!record.pipeline.computeShader && activeRenderPass_ == VK_NULL_HANDLE && renderPass_ == VK_NULL_HANDLE) return false;
        if (record.graphicsPipeline != VK_NULL_HANDLE || record.depthOnlyPipeline != VK_NULL_HANDLE) {
            std::uint32_t pipelineId = 0;
            for (const auto& [id, resource] : resources_) {
                if (&resource == &record) {
                    pipelineId = id;
                    break;
                }
            }
            if (pipelineId != 0) {
                for (auto& [id, resource] : resources_) {
                    if (resource.kind == ResourceKind::Material && resource.material.pipeline.id == pipelineId) {
                        release_descriptor_sets(resource);
                    }
                }
            }
            vkDestroyPipeline(device_, record.graphicsPipeline, nullptr);
            record.graphicsPipeline = VK_NULL_HANDLE;
            if (record.depthOnlyPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, record.depthOnlyPipeline, nullptr);
                record.depthOnlyPipeline = VK_NULL_HANDLE;
            }
            if (record.pipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_, record.pipelineLayout, nullptr);
                record.pipelineLayout = VK_NULL_HANDLE;
            }
            if (record.descriptorLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device_, record.descriptorLayout, nullptr);
                record.descriptorLayout = VK_NULL_HANDLE;
            }
            for (const auto layout : record.descriptorLayouts) {
                if (layout != VK_NULL_HANDLE && layout != bindlessLayout_) {
                    vkDestroyDescriptorSetLayout(device_, layout, nullptr);
                }
            }
            record.descriptorLayouts.clear();
        }
        if (record.pipeline.computeShader != 0) {
            const auto compute = resources_.find(record.pipeline.computeShader);
            if (compute == resources_.end() || compute->second.shader == VK_NULL_HANDLE) return false;
            if (!validate_backend_shader_layout(compute->second.shaderBindings, lastError_)) return false;
            record.shaderBindings = compute->second.shaderBindings;
            const bool usesBindlessSet = descriptorIndexingEnabled_ && std::any_of(compute->second.shaderBindings.begin(),
                compute->second.shaderBindings.end(), [](const auto& binding) {
                    return binding.space == 0 && binding.unbounded;
                });
            std::vector<std::vector<VkDescriptorSetLayoutBinding>> bindingsBySpace;
            const auto addDescriptorBinding = [&](std::uint32_t space, std::uint32_t slot, VkDescriptorType type,
                                                  std::uint32_t count = 1, bool unbounded = false) {
                if (space >= 16) return false;
                if (bindingsBySpace.size() <= space) bindingsBySpace.resize(space + 1);
                auto& descriptorBindings = bindingsBySpace[space];
                const auto existing = std::find_if(descriptorBindings.begin(), descriptorBindings.end(), [slot](const auto& b) { return b.binding == slot; });
                if (existing != descriptorBindings.end()) return existing->descriptorType == type && existing->descriptorCount == (unbounded ? 1024u : count);
                VkDescriptorSetLayoutBinding binding{};
                binding.binding = slot;
                binding.descriptorCount = unbounded ? 1024u : count;
                binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                binding.descriptorType = type;
                descriptorBindings.push_back(binding);
                return true;
            };
            for (const auto& reflected : compute->second.shaderBindings) {
                const auto type = reflected.type == "texture" ? (reflected.separateImage ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) :
                    (reflected.type == "storage_texture" ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
                    (reflected.type == "sampler" ? VK_DESCRIPTOR_TYPE_SAMPLER :
                    (reflected.type == "uniform_buffer" ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)));
                if (!addDescriptorBinding(reflected.space, reflected.slot, type, reflected.count, reflected.unbounded)) return false;
            }
            record.descriptorLayouts.clear();
            const auto setCount = std::max<std::size_t>(bindingsBySpace.size(), usesBindlessSet ? 1u : 0u);
            record.descriptorLayouts.reserve(setCount);
            for (std::size_t set = 0; set < setCount; ++set) {
                if (usesBindlessSet && set == 0) {
                    record.descriptorLayouts.push_back(bindlessLayout_);
                    continue;
                }
                const auto& descriptorBindings = set < bindingsBySpace.size()
                    ? bindingsBySpace[set] : std::vector<VkDescriptorSetLayoutBinding>{};
                VkDescriptorSetLayoutCreateInfo descriptorInfo{};
                descriptorInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                descriptorInfo.bindingCount = static_cast<std::uint32_t>(descriptorBindings.size());
                descriptorInfo.pBindings = descriptorBindings.data();
                VkDescriptorSetLayout layout = VK_NULL_HANDLE;
                if (vkCreateDescriptorSetLayout(device_, &descriptorInfo, nullptr, &layout) != VK_SUCCESS) return false;
                record.descriptorLayouts.push_back(layout);
            }
            VkPipelineLayoutCreateInfo layoutInfo{};
            layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layoutInfo.setLayoutCount = static_cast<std::uint32_t>(record.descriptorLayouts.size());
            layoutInfo.pSetLayouts = record.descriptorLayouts.data();
            if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &record.pipelineLayout) != VK_SUCCESS) return false;
            VkPipelineShaderStageCreateInfo stage{};
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = compute->second.shader;
            stage.pName = "main";
            VkComputePipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipelineInfo.stage = stage;
            pipelineInfo.layout = record.pipelineLayout;
            const auto result = vkCreateComputePipelines(device_, pipelineCache_, 1, &pipelineInfo, nullptr, &record.computePipeline);
            if (result != VK_SUCCESS) {
                lastError_ = "Vulkan compute pipeline creation failed VkResult=" + std::to_string(static_cast<int>(result));
                return false;
            }
            return true;
        }
        if (!is_supported_sample_count(record.pipeline.sampleCount) ||
            (record.pipeline.fillMode != "solid" && record.pipeline.fillMode != "wireframe") ||
            (record.pipeline.cullMode != "back" && record.pipeline.cullMode != "front" && record.pipeline.cullMode != "none") ||
            (record.pipeline.topology != "triangle" && record.pipeline.topology != "line" && record.pipeline.topology != "point") ||
            record.pipeline.colorFormats.size() > 8 ||
            std::any_of(record.pipeline.colorFormats.begin(), record.pipeline.colorFormats.end(),
                [](const std::string& format) { return !is_supported_color_format(format); }) ||
            ((!record.pipeline.colorFormats.empty() &&
              std::any_of(record.pipeline.colorFormats.begin(), record.pipeline.colorFormats.end(),
                [](const std::string& format) { return format != "rgba8" && format != "bgra8" &&
                    format != "rgba16f" && format != "r32f"; })) ||
             !is_supported_color_format(record.pipeline.colorFormat)) ||
            !is_supported_pipeline_depth_format(record.pipeline.depthFormat)) {
            lastError_ = "Vulkan pipeline fixed-function state or attachment format is unsupported";
            return false;
        }
        VkSampleCountFlagBits pipelineSamples = VK_SAMPLE_COUNT_1_BIT;
        switch (record.pipeline.sampleCount) {
        case 1: pipelineSamples = VK_SAMPLE_COUNT_1_BIT; break;
        case 2: pipelineSamples = VK_SAMPLE_COUNT_2_BIT; break;
        case 4: pipelineSamples = VK_SAMPLE_COUNT_4_BIT; break;
        case 8: pipelineSamples = VK_SAMPLE_COUNT_8_BIT; break;
        default: return false;
        }
        const auto activeTargetSamples = target != resources_.end() && target->second.image != VK_NULL_HANDLE
            ? target->second.samples : VK_SAMPLE_COUNT_1_BIT;
        if (pipelineSamples != activeTargetSamples) {
            lastError_ = "Vulkan pipeline sample count does not match the active render target";
            return false;
        }
        const auto expectedColorFormat = format_of(record.pipeline.colorFormats.empty()
            ? std::string_view(record.pipeline.colorFormat) : std::string_view(record.pipeline.colorFormats.front()));
        if (depthOnlyPass) {
            if (target == resources_.end() || !target->second.depthStencil ||
                format_of(record.pipeline.depthFormat) != target->second.format) {
                lastError_ = "Vulkan pipeline depth format does not match the active depth target";
                return false;
            }
        } else if (target != resources_.end() && target->second.image != VK_NULL_HANDLE) {
            if (target->second.depthStencil || target->second.format != expectedColorFormat) {
                lastError_ = "Vulkan pipeline color format does not match the active render target";
                return false;
            }
        } else if (swapchainFormat_ != VK_FORMAT_UNDEFINED && swapchainFormat_ != expectedColorFormat) {
            lastError_ = "Vulkan pipeline color format does not match the swapchain";
            return false;
        }
        const auto vertex = resources_.find(record.pipeline.vertexShader);
        const auto fragment = resources_.find(record.pipeline.fragmentShader);
        if (vertex == resources_.end() || fragment == resources_.end() ||
            vertex->second.shader == VK_NULL_HANDLE || fragment->second.shader == VK_NULL_HANDLE) return false;
        std::vector<ShaderBinding> mergedShaderBindings;
        if (!merge_shader_bindings(vertex->second.shaderBindings, mergedShaderBindings, lastError_) ||
            !merge_shader_bindings(fragment->second.shaderBindings, mergedShaderBindings, lastError_)) return false;
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        std::vector<std::vector<VkDescriptorSetLayoutBinding>> bindingsBySpace;
        record.shaderBindings = mergedShaderBindings;
        const bool usesBindlessSet = descriptorIndexingEnabled_ && std::any_of(mergedShaderBindings.begin(),
            mergedShaderBindings.end(), [](const auto& binding) {
                return binding.space == 0 && binding.unbounded;
            });
        const auto ensureSpace = [&bindingsBySpace](std::uint32_t space) {
            if (space >= 16) return false;
            if (bindingsBySpace.size() <= space) bindingsBySpace.resize(space + 1);
            return true;
        };
        const auto addDescriptorBinding = [&](std::uint32_t space, std::uint32_t slot, VkDescriptorType type, std::uint32_t count = 1, bool unbounded = false) {
            if (!ensureSpace(space)) return;
            auto& descriptorBindings = bindingsBySpace[space];
            const auto existing = std::find_if(descriptorBindings.begin(), descriptorBindings.end(),
                [slot](const auto& binding) { return binding.binding == slot; });
            if (existing != descriptorBindings.end()) {
                if (existing->descriptorType != type || existing->descriptorCount != (unbounded ? 1024u : count)) lastError_ = "Vulkan descriptor layout conflict";
                return;
            }
            VkDescriptorSetLayoutBinding layoutBinding{};
            layoutBinding.binding = slot;
            layoutBinding.descriptorCount = unbounded ? 1024u : count;
            layoutBinding.stageFlags = VK_SHADER_STAGE_ALL;
            layoutBinding.descriptorType = type;
            descriptorBindings.push_back(layoutBinding);
        };
        for (const auto shaderId : {record.pipeline.vertexShader, record.pipeline.fragmentShader}) {
            const auto shader = resources_.find(shaderId);
            if (shader == resources_.end()) continue;
            for (const auto& reflected : shader->second.shaderBindings) {
                if (usesBindlessSet && reflected.space == 0) continue;
                const auto type = reflected.type == "texture" ? (reflected.separateImage ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) :
                    (reflected.type == "storage_texture" ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
                    (reflected.type == "sampler" ? VK_DESCRIPTOR_TYPE_SAMPLER :
                    (reflected.type == "uniform_buffer" ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)));
                addDescriptorBinding(reflected.space, reflected.slot, type, reflected.count, reflected.unbounded);
            }
        }
        record.descriptorLayouts.clear();
        const auto setCount = std::max<std::size_t>(bindingsBySpace.size(), usesBindlessSet ? 1u : 0u);
        record.descriptorLayouts.reserve(setCount);
        for (std::size_t set = 0; set < setCount; ++set) {
            if (usesBindlessSet && set == 0) {
                record.descriptorLayouts.push_back(bindlessLayout_);
                continue;
            }
            const auto& descriptorBindings = set < bindingsBySpace.size()
                ? bindingsBySpace[set] : std::vector<VkDescriptorSetLayoutBinding>{};
            VkDescriptorSetLayoutCreateInfo descriptorInfo{};
            descriptorInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            descriptorInfo.bindingCount = static_cast<std::uint32_t>(descriptorBindings.size());
            descriptorInfo.pBindings = descriptorBindings.data();
            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            if (vkCreateDescriptorSetLayout(device_, &descriptorInfo, nullptr, &layout) != VK_SUCCESS) return false;
            record.descriptorLayouts.push_back(layout);
        }
        layoutInfo.setLayoutCount = static_cast<std::uint32_t>(record.descriptorLayouts.size());
        layoutInfo.pSetLayouts = record.descriptorLayouts.data();
        if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &record.pipelineLayout) != VK_SUCCESS) return false;
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex->second.shader;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment->second.shader;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        const auto layout = resolve_vertex_layout(record.pipeline);
        std::vector<VkVertexInputBindingDescription> vertexBindings;
        std::vector<VkVertexInputAttributeDescription> vertexAttributes;
        constexpr VkFormat formats[]{
            VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32G32B32_SFLOAT,VK_FORMAT_R32G32B32A32_SFLOAT,
            VK_FORMAT_R32_UINT,VK_FORMAT_R32G32_UINT,VK_FORMAT_R32G32B32_UINT,VK_FORMAT_R32G32B32A32_UINT,
            VK_FORMAT_R32_SINT,VK_FORMAT_R32G32_SINT,VK_FORMAT_R32G32B32_SINT,VK_FORMAT_R32G32B32A32_SINT,
            VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SNORM,VK_FORMAT_R8G8B8A8_UINT,
            VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16G16B16A16_UNORM,VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT};
        for (const auto& binding : layout.bindings)
            vertexBindings.push_back({binding.binding,binding.stride,
                binding.perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
        for (const auto& attribute : layout.attributes)
            vertexAttributes.push_back({attribute.location,attribute.binding,formats[static_cast<unsigned>(attribute.format)],attribute.offset});
        vertexInput.vertexBindingDescriptionCount = static_cast<std::uint32_t>(vertexBindings.size());
        vertexInput.pVertexBindingDescriptions = vertexBindings.data();
        vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(vertexAttributes.size());
        vertexInput.pVertexAttributeDescriptions = vertexAttributes.data();
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = topology(record.pipeline.topology);
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = nullptr;
        viewportState.scissorCount = 1;
        viewportState.pScissors = nullptr;
        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = fill_mode(record.pipeline.fillMode);
        raster.cullMode = cull_mode(record.pipeline.cullMode);
        // The engine's positive Vulkan viewport reverses framebuffer winding.
        raster.frontFace = record.pipeline.rasterizerState.frontCounterClockwise
            ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.depthClampEnable = !record.pipeline.rasterizerState.depthClip;
        raster.depthBiasEnable = record.pipeline.rasterizerState.depthBias != 0 || record.pipeline.rasterizerState.slopeScaledDepthBias != 0.0f;
        raster.depthBiasConstantFactor = static_cast<float>(record.pipeline.rasterizerState.depthBias);
        raster.depthBiasClamp = record.pipeline.rasterizerState.depthBiasClamp;
        raster.depthBiasSlopeFactor = record.pipeline.rasterizerState.slopeScaledDepthBias;
        if ((raster.depthClampEnable && !fixedFunctionFeatures_.depthClamp) ||
            (raster.depthBiasClamp != 0.0f && !fixedFunctionFeatures_.depthBiasClamp) ||
            (raster.polygonMode != VK_POLYGON_MODE_FILL && !fixedFunctionFeatures_.fillModeNonSolid)) {
            lastError_ = "Vulkan device does not support the requested rasterizer state";
            return false;
        }
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = pipelineSamples;
        VkPipelineDepthStencilStateCreateInfo depthState{};
        depthState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        const auto depth = resolve_depth_stencil(record.pipeline);
        depthState.depthTestEnable = depth.depthTest;
        depthState.depthWriteEnable = depth.depthWrite;
        depthState.depthCompareOp = static_cast<VkCompareOp>(depth.depthCompare);
        depthState.stencilTestEnable = depth.stencilTest;
        const auto stencil = [&](const StencilFaceState& face) {
            return VkStencilOpState{static_cast<VkStencilOp>(face.fail),static_cast<VkStencilOp>(face.pass),
                static_cast<VkStencilOp>(face.depthFail),static_cast<VkCompareOp>(face.compare),
                depth.stencilReadMask,depth.stencilWriteMask,depth.stencilReference};
        };
        depthState.front = stencil(depth.front);
        depthState.back = stencil(depth.back);
        const auto colorFormats = depthOnlyPass ? std::vector<std::string>{} : (record.pipeline.colorFormats.empty()
            ? std::vector<std::string>{record.pipeline.colorFormat} : record.pipeline.colorFormats);
        const auto attachments = resolve_blend_attachments(record.pipeline);
        std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
        for (std::size_t index = 0; index < colorFormats.size(); ++index) {
            const auto& blend = attachments[index];
            blendAttachments.push_back({blend.enabled,static_cast<VkBlendFactor>(blend.srcColor),
                static_cast<VkBlendFactor>(blend.dstColor),static_cast<VkBlendOp>(blend.colorOp),
                static_cast<VkBlendFactor>(blend.srcAlpha),static_cast<VkBlendFactor>(blend.dstAlpha),
                static_cast<VkBlendOp>(blend.alphaOp),blend.writeMask});
        }
        const auto sameBlend=[](const auto& a,const auto& b) {
            return a.blendEnable==b.blendEnable && a.srcColorBlendFactor==b.srcColorBlendFactor &&
                a.dstColorBlendFactor==b.dstColorBlendFactor && a.colorBlendOp==b.colorBlendOp &&
                a.srcAlphaBlendFactor==b.srcAlphaBlendFactor && a.dstAlphaBlendFactor==b.dstAlphaBlendFactor &&
                a.alphaBlendOp==b.alphaBlendOp && a.colorWriteMask==b.colorWriteMask;
        };
        if (blendAttachments.size() > 1 && !fixedFunctionFeatures_.independentBlend &&
            !std::all_of(blendAttachments.begin()+1,blendAttachments.end(),[&](const auto& state){return sameBlend(state,blendAttachments.front());})) {
            lastError_ = "Vulkan independent attachment blending is unavailable";
            return false;
        }
        const auto dualSource=[](auto factor){return factor>=VK_BLEND_FACTOR_SRC1_COLOR;};
        if (!fixedFunctionFeatures_.dualSrcBlend && std::any_of(blendAttachments.begin(),blendAttachments.end(),[&](const auto& state) {
            return state.blendEnable && (dualSource(state.srcColorBlendFactor) || dualSource(state.dstColorBlendFactor) ||
                dualSource(state.srcAlphaBlendFactor) || dualSource(state.dstAlphaBlendFactor));
        })) { lastError_="Vulkan dual-source blending is unavailable";return false; }
        VkPipelineColorBlendStateCreateInfo blendState{};
        blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blendState.blendConstants[0] = record.pipeline.blendConstants.x;
        blendState.blendConstants[1] = record.pipeline.blendConstants.y;
        blendState.blendConstants[2] = record.pipeline.blendConstants.z;
        blendState.blendConstants[3] = record.pipeline.blendConstants.w;
        blendState.attachmentCount = static_cast<std::uint32_t>(blendAttachments.size());
        blendState.pAttachments = blendAttachments.data();
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = depthOnlyPass ? 1u : 2u;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthState;
        pipelineInfo.pColorBlendState = depthOnlyPass ? nullptr : &blendState;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = record.pipelineLayout;
        pipelineInfo.renderPass = activePass;
        pipelineInfo.subpass = 0;
        if (depthOnlyPass) {
            const auto result = vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pipelineInfo, nullptr,
                &record.depthOnlyPipeline);
            if (result != VK_SUCCESS) {
                lastError_ = "Vulkan depth-only pipeline creation failed VkResult=" + std::to_string(static_cast<int>(result));
                vkDestroyPipelineLayout(device_, record.pipelineLayout, nullptr);
                record.pipelineLayout = VK_NULL_HANDLE;
                return false;
            }
            record.pipelineRenderPass = pipelineInfo.renderPass;
            return true;
        }
        const auto graphicsResult = vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pipelineInfo, nullptr, &record.graphicsPipeline);
        if (graphicsResult != VK_SUCCESS) {
            lastError_ = "Vulkan graphics pipeline creation failed VkResult=" + std::to_string(static_cast<int>(graphicsResult));
            vkDestroyPipelineLayout(device_, record.pipelineLayout, nullptr);
            record.pipelineLayout = VK_NULL_HANDLE;
            return false;
        }
        record.pipelineRenderPass = pipelineInfo.renderPass;
        if (pipelineSamples == VK_SAMPLE_COUNT_1_BIT && depthOnlyRenderPass_ != VK_NULL_HANDLE) {
            pipelineInfo.stageCount = 1;
            pipelineInfo.pColorBlendState = nullptr;
            pipelineInfo.subpass = 0;
            pipelineInfo.renderPass = depthOnlyRenderPass_;
            const auto depthResult = vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pipelineInfo, nullptr, &record.depthOnlyPipeline);
            if (depthResult != VK_SUCCESS) {
                lastError_ = "Vulkan secondary depth-only pipeline creation failed VkResult=" + std::to_string(static_cast<int>(depthResult));
                record.depthOnlyPipeline = VK_NULL_HANDLE;
            }
        }
        return true;
    }
    void bind_pipeline(std::string_view name) override {
        for (auto& [id, resource] : resources_) {
            if (resource.kind == ResourceKind::Pipeline && resource.pipeline.name == name) {
                bind_pipeline(ResourceHandle{id, ResourceKind::Pipeline});
                return;
            }
        }
        boundPipeline_ = nullptr;
        lastError_ = "Vulkan pipeline name is invalid: " + std::string(name);
    }
    void bind_pipeline(ResourceHandle handle) override {
        const auto it = resources_.find(handle.id);
        boundPipeline_ = it != resources_.end() && it->second.kind == ResourceKind::Pipeline &&
            handle.kind == ResourceKind::Pipeline ? &it->second : nullptr;
        if (!boundPipeline_) {
            lastError_ = "Vulkan pipeline handle is invalid";
            return;
        }
        if (boundPipeline_ && ensure_pipeline(*boundPipeline_)) {
            if (boundPipeline_->pipeline.computeShader) {
                vkCmdBindPipeline(recording_command(), VK_PIPELINE_BIND_POINT_COMPUTE, boundPipeline_->computePipeline);
            } else {
                const auto target = resources_.find(currentRenderTarget_);
                const bool depthOnly = target != resources_.end() && target->second.kind == ResourceKind::DepthStencil;
                vkCmdBindPipeline(recording_command(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                    depthOnly && boundPipeline_->depthOnlyPipeline != VK_NULL_HANDLE ? boundPipeline_->depthOnlyPipeline : boundPipeline_->graphicsPipeline);
            }
        } else if (boundPipeline_) {
            if (lastError_.empty()) lastError_ = "Vulkan pipeline binding failed";
            boundPipeline_ = nullptr;
        }
    }
    void bind_material(ResourceHandle handle) override {
        const auto material = resources_.find(handle.id);
        if (material == resources_.end() || material->second.kind != ResourceKind::Material ||
            handle.kind != ResourceKind::Material) {
            lastError_ = "Vulkan material handle is invalid";
            return;
        }
        ++stats_.materialBinds;
        boundMaterial_ = &material->second;
        bind_pipeline(material->second.material.pipeline);
        if (!boundPipeline_) return;
        const auto pipeline = resources_.find(material->second.material.pipeline.id);
        if (pipeline == resources_.end() || pipeline->second.pipelineLayout == VK_NULL_HANDLE || descriptorPool_ == VK_NULL_HANDLE) {
            lastError_ = "Vulkan material binding has invalid pipeline layout or descriptor pool";
            return;
        }
        const auto bindlessTable = bindlessTables_.find(boundBindlessTable_);
        if (material->second.material.bindless) {
            if (bindlessTable != bindlessTables_.end()) {
                const auto bindPoint = pipeline->second.pipeline.computeShader ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
                vkCmdBindDescriptorSets(recording_command(), bindPoint,
                    pipeline->second.pipelineLayout, 0, 1, &bindlessTable->second.set, 0, nullptr);
            }
            return;
        }
        stats_.descriptorBinds += material->second.material.bindings.size();
        for (const auto& binding : material->second.material.bindings) {
            bool reflectionAvailable = false;
            bool found = false;
            for (const auto shaderId : {pipeline->second.pipeline.vertexShader, pipeline->second.pipeline.fragmentShader, pipeline->second.pipeline.computeShader}) {
                const auto shader = resources_.find(shaderId);
                if (shader == resources_.end() || shader->second.shaderBindings.empty()) continue;
                reflectionAvailable = true;
                found |= std::any_of(shader->second.shaderBindings.begin(), shader->second.shaderBindings.end(),
                    [&](const auto& reflected) { return shader_binding_matches(reflected, binding); });
            }
            if (reflectionAvailable && !found) {
                lastError_ = "Vulkan material '" + material->second.material.name + "' binding '" + binding.name +
                    "' does not match shader set=" + std::to_string(binding.space) +
                    " binding=" + std::to_string(binding.slot) + " type=" + descriptor_type_name(binding.type);
                return;
            }
        }
        if (pipeline->second.descriptorLayouts.empty()) return;
        if (material->second.descriptorSets.empty()) {
        VkDescriptorSetAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            const bool pipelineUsesBindlessSet = descriptorIndexingEnabled_ && !pipeline->second.descriptorLayouts.empty() &&
                pipeline->second.descriptorLayouts.front() == bindlessLayout_;
            const auto firstMaterialSet = pipeline->second.pipeline.computeShader ? 0u : (pipelineUsesBindlessSet ? 1u : 0u);
            if (pipeline->second.descriptorLayouts.size() <= firstMaterialSet) return;
            allocation.descriptorSetCount = static_cast<std::uint32_t>(pipeline->second.descriptorLayouts.size()) - firstMaterialSet;
            allocation.pSetLayouts = pipeline->second.descriptorLayouts.data() + firstMaterialSet;
            material->second.descriptorSets.resize(pipeline->second.descriptorLayouts.size(), VK_NULL_HANDLE);
            if (!allocate_descriptor_sets(allocation.descriptorSetCount, allocation.pSetLayouts,
                material->second.descriptorSets.data() + firstMaterialSet, material->second.descriptorPool)) {
                material->second.descriptorSets.clear();
                return;
            }
            material->second.descriptorSet = material->second.descriptorSets.empty()
                ? VK_NULL_HANDLE : material->second.descriptorSets.back();
            std::vector<VkWriteDescriptorSet> writes;
            std::vector<VkDescriptorImageInfo> images;
            std::vector<VkDescriptorBufferInfo> buffers;
            std::size_t imageReserve = 0;
            std::size_t bufferReserve = 0;
            for (const auto& binding : material->second.material.bindings) {
                const auto count = binding.resources.empty() ? (binding.resource ? 1u : 0u)
                    : static_cast<std::uint32_t>(binding.resources.size());
                if (binding.type == DescriptorType::Texture || binding.type == DescriptorType::StorageTexture || binding.type == DescriptorType::Sampler) {
                    imageReserve += count;
                } else {
                    bufferReserve += count;
                }
            }
            images.reserve(imageReserve);
            buffers.reserve(bufferReserve);
            for (const auto& binding : material->second.material.bindings) {
                if (binding.space >= material->second.descriptorSets.size() ||
                    (pipelineUsesBindlessSet && binding.space == 0)) continue;
                const std::vector<ResourceHandle> handles = binding.resources.empty()
                    ? std::vector<ResourceHandle>{binding.resource}
                    : binding.resources;
                if (handles.empty() || (handles.size() == 1 && !handles.front())) continue;
                VkWriteDescriptorSet write{};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = material->second.descriptorSets[binding.space];
                write.dstBinding = binding.slot;
                write.descriptorCount = static_cast<std::uint32_t>(handles.size());
                if (binding.type == DescriptorType::Texture) {
                    const bool separateImage = std::any_of(boundPipeline_->shaderBindings.begin(),boundPipeline_->shaderBindings.end(),
                        [&](const auto& value) { return value.slot == binding.slot && value.space == binding.space && value.separateImage; });
                    const auto first = images.size();
                    for (const auto handle : handles) {
                        const auto resource = resources_.find(handle.id);
                        if (resource == resources_.end() || resource->second.view == VK_NULL_HANDLE) {
                            lastError_ = "Vulkan texture array binding '" + binding.name + "' contains an invalid resource";
                            return;
                        }
                        images.push_back({separateImage ? VK_NULL_HANDLE : defaultSampler_, resource->second.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                    }
                    write.descriptorType = separateImage ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    write.pImageInfo = images.data() + first;
                } else if (binding.type == DescriptorType::StorageTexture) {
                    const auto first = images.size();
                    for (const auto handle : handles) {
                        const auto resource = resources_.find(handle.id);
                        if (resource == resources_.end() || resource->second.view == VK_NULL_HANDLE) {
                            lastError_ = "Vulkan storage texture array binding '" + binding.name + "' contains an invalid resource";
                            return;
                        }
                        images.push_back({VK_NULL_HANDLE, resource->second.view, VK_IMAGE_LAYOUT_GENERAL});
                    }
                    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    write.pImageInfo = images.data() + first;
                } else if (binding.type == DescriptorType::Sampler) {
                    const auto first = images.size();
                    for (const auto handle : handles) {
                        const auto resource = resources_.find(handle.id);
                        if (resource == resources_.end() || resource->second.sampler == VK_NULL_HANDLE) {
                            lastError_ = "Vulkan sampler array binding '" + binding.name + "' contains an invalid resource";
                            return;
                        }
                        images.push_back({resource->second.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
                    }
                    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                    write.pImageInfo = images.data() + first;
                } else {
                    const auto first = buffers.size();
                    for (const auto handle : handles) {
                        const auto resource = resources_.find(handle.id);
                        if (resource == resources_.end() || resource->second.buffer == VK_NULL_HANDLE) {
                            lastError_ = "Vulkan buffer array binding '" + binding.name + "' contains an invalid resource";
                            return;
                        }
                        buffers.push_back({resource->second.buffer, 0, VK_WHOLE_SIZE});
                    }
                    write.descriptorType = binding.type == DescriptorType::UniformBuffer ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    write.pBufferInfo = buffers.data() + first;
                }
                writes.push_back(write);
            }
            vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        const auto bindPoint = pipeline->second.pipeline.computeShader ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
        const bool pipelineUsesBindlessSet = descriptorIndexingEnabled_ && !pipeline->second.descriptorLayouts.empty() &&
            pipeline->second.descriptorLayouts.front() == bindlessLayout_;
        const auto firstMaterialSet = pipeline->second.pipeline.computeShader ? 0u : (pipelineUsesBindlessSet ? 1u : 0u);
        const auto materialSetCount = static_cast<std::uint32_t>(material->second.descriptorSets.size()) - firstMaterialSet;
        if (materialSetCount > 0) {
            vkCmdBindDescriptorSets(recording_command(), bindPoint,
                pipeline->second.pipelineLayout, firstMaterialSet, materialSetCount,
                material->second.descriptorSets.data() + firstMaterialSet, 0, nullptr);
        }
    }
    bool update_material(ResourceHandle handle, const MaterialDesc& description) override {
        const auto material = resources_.find(handle.id);
        if (handle.kind != ResourceKind::Material || material == resources_.end() || material->second.kind != ResourceKind::Material) {
            lastError_ = "Vulkan material handle not found";
            return false;
        }
        if (material->second.material.pipeline.id != description.pipeline.id) {
            lastError_ = "Vulkan material pipeline cannot be changed after creation";
            return false;
        }
        material->second.material = description;
        release_descriptor_sets(material->second);
        material->second.descriptorSet = VK_NULL_HANDLE;
        material->second.descriptorSets.clear();
        boundMaterial_ = nullptr;
        return true;
    }
    void bind_uniform_buffer(ResourceHandle handle, std::uint32_t slot, std::uint32_t space) override {
        if (!device_ || !boundMaterial_) {
            if (lastError_.empty()) lastError_ = "Vulkan uniform buffer binding has invalid command state";
            return;
        }
        const auto resource = resources_.find(handle.id);
        if (handle.kind != ResourceKind::Buffer || resource == resources_.end() || resource->second.kind != ResourceKind::Buffer ||
            resource->second.buffer == VK_NULL_HANDLE) {
            lastError_ = "Vulkan uniform buffer handle is invalid";
            return;
        }
        const auto pipeline = resources_.find(boundMaterial_->material.pipeline.id);
        if (pipeline == resources_.end() || pipeline->second.pipelineLayout == VK_NULL_HANDLE) {
            lastError_ = "Vulkan uniform buffer pipeline layout is invalid";
            return;
        }
        const auto pipelineUsesBindlessSet = descriptorIndexingEnabled_ && !pipeline->second.descriptorLayouts.empty() &&
            pipeline->second.descriptorLayouts.front() == bindlessLayout_;
        if (space >= boundMaterial_->descriptorSets.size() ||
            (pipelineUsesBindlessSet && space == 0)) {
            lastError_ = "Vulkan uniform buffer descriptor set or space is invalid";
            return;
        }
        VkDescriptorBufferInfo bufferInfo{resource->second.buffer, 0, resource->second.size};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = boundMaterial_->descriptorSets[space];
        write.dstBinding = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        const auto bindPoint = pipeline->second.pipeline.computeShader ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
        vkCmdBindDescriptorSets(recording_command(), bindPoint,
            pipeline->second.pipelineLayout, space, 1, &boundMaterial_->descriptorSets[space], 0, nullptr);
    }
    bool bind_mesh_vertices(ResourceHandle primary, const std::vector<VertexBufferBinding>& explicitBindings) {
        DrawVertexBindings bindings;
        if (!draw_vertex_bindings(boundPipeline_->pipeline,primary,explicitBindings,bindings,lastError_)) return false;
        const auto& layout = boundPipeline_->pipeline.vertexLayout;
        for (const auto& binding : bindings) {
            const auto found = resources_.find(binding.buffer.id);
            if (found == resources_.end() || found->second.kind != ResourceKind::Buffer ||
                binding.offset >= found->second.size || found->second.size-binding.offset > UINT_MAX) {
                lastError_ = "vertex stream buffer or offset is invalid"; return false;
            }
            const auto definition = std::find_if(layout.bindings.begin(),layout.bindings.end(),
                [&](const auto& value) { return value.binding == binding.binding; });
            (void)definition;
            if (!found->second.buffer) { lastError_ = "missing Vulkan vertex buffer"; return false; }
            const VkDeviceSize offset = binding.offset;
            vkCmdBindVertexBuffers(recording_command(),binding.binding,1,&found->second.buffer,&offset);
        }
        return true;
    }
    void draw_sprite(const SpriteDraw&) override { ++stats_.drawCalls; ++stats_.spriteCalls; if (frameActive_ && boundPipeline_ && boundPipeline_->graphicsPipeline) vkCmdDraw(recording_command(), 6, 1, 0, 0); }
    void draw_mesh(const MeshDraw& draw) override {
        ++stats_.drawCalls; ++stats_.meshCalls;
        if (!frameActive_ || !boundPipeline_ || (!boundPipeline_->graphicsPipeline && !boundPipeline_->depthOnlyPipeline) || !draw.indexCount) {
            if (lastError_.empty()) lastError_ = "Vulkan mesh draw has invalid command state";
            return;
        }
        const auto vertex = resources_.find(draw.vertexBuffer.id);
        const auto index = resources_.find(draw.indexBuffer.id);
        if (vertex == resources_.end() || index == resources_.end() ||
            draw.vertexBuffer.kind != ResourceKind::Buffer || draw.indexBuffer.kind != ResourceKind::Buffer ||
            vertex->second.kind != ResourceKind::Buffer || index->second.kind != ResourceKind::Buffer ||
            vertex->second.buffer == VK_NULL_HANDLE || index->second.buffer == VK_NULL_HANDLE ||
            !indexed_range_fits(index->second.size, draw.firstIndex, draw.indexCount)) {
            lastError_ = "Vulkan mesh draw buffers are invalid";
            return;
        }
        if (!bind_mesh_vertices(draw.vertexBuffer,draw.vertexBindings)) return;
        vkCmdBindIndexBuffer(recording_command(), index->second.buffer, 0, VK_INDEX_TYPE_UINT32);
        if (!draw.instanceCount) { lastError_ = "instance count must be non-zero"; return; }
        vkCmdDrawIndexed(recording_command(),draw.indexCount,draw.instanceCount,draw.firstIndex,draw.vertexOffset,draw.firstInstance);
    }
    bool dispatch(const DispatchDesc& dispatch) override {
        if (!frameActive_ || !boundPipeline_ || dispatch.groupCountX == 0 || dispatch.groupCountY == 0 || dispatch.groupCountZ == 0) {
            lastError_ = "Vulkan dispatch has invalid command state or zero group count";
            return false;
        }
        if (!boundPipeline_->pipeline.computeShader || !ensure_pipeline(*boundPipeline_)) {
            lastError_ = "Vulkan dispatch requires a compute shader pipeline";
            return false;
        }
        if (renderPassActive_) {
            vkCmdEndRenderPass(recording_command());
            renderPassActive_ = false;
            activeRenderPass_ = VK_NULL_HANDLE;
            currentRenderTarget_ = 0;
        }
        vkCmdBindPipeline(recording_command(), VK_PIPELINE_BIND_POINT_COMPUTE, boundPipeline_->computePipeline);
        vkCmdDispatch(recording_command(), dispatch.groupCountX, dispatch.groupCountY, dispatch.groupCountZ);
        ++stats_.dispatchCalls;
        return true;
    }
    void draw_mesh_indirect(const IndirectMeshDraw& draw) override {
        ++stats_.drawCalls;
        ++stats_.meshCalls;
        const auto vertex = resources_.find(draw.vertexBuffer.id);
        const auto index = resources_.find(draw.indexBuffer.id);
        const auto arguments = resources_.find(draw.argumentBuffer.id);
        if (!frameActive_ || !boundPipeline_ || (!boundPipeline_->graphicsPipeline && !boundPipeline_->depthOnlyPipeline) ||
            vertex == resources_.end() || index == resources_.end() || arguments == resources_.end() ||
            vertex->second.buffer == VK_NULL_HANDLE || index->second.buffer == VK_NULL_HANDLE ||
            arguments->second.buffer == VK_NULL_HANDLE || draw.maxDrawCount == 0 ||
            draw.vertexBuffer.kind != ResourceKind::Buffer || draw.indexBuffer.kind != ResourceKind::Buffer ||
            draw.argumentBuffer.kind != ResourceKind::Buffer || vertex->second.kind != ResourceKind::Buffer ||
            index->second.kind != ResourceKind::Buffer || arguments->second.kind != ResourceKind::Buffer ||
            !indirect_arguments_fit(arguments->second.size, draw)) {
            lastError_ = "Vulkan indirect draw has invalid command state or buffer";
            return;
        }
        if (!bind_mesh_vertices(draw.vertexBuffer,draw.vertexBindings)) return;
        vkCmdBindIndexBuffer(recording_command(), index->second.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(recording_command(), arguments->second.buffer,
            draw.argumentOffset, draw.maxDrawCount, draw.stride);
        ++stats_.indirectDrawCalls;
    }
    void end_frame() override {
        if (!frameActive_) return;
        if (activeCommandBuffer_ != VK_NULL_HANDLE) end_queue(activeQueueKind_, currentBatchIndex_, true, swapchain_ != VK_NULL_HANDLE);
        if (pendingCommandBuffer_ != VK_NULL_HANDLE) finalize_pending_queue_batch();
        if (batchSignals_.empty() && frameFence_ != VK_NULL_HANDLE) {
            vkQueueSubmit(graphicsQueue_, 0, nullptr, frameFence_);
        }
        frameActive_ = false;
    }
    void discard_frame() override {
        if (!frameActive_) return;
        ++stats_.discardedFrames;
        if (pendingCommandBuffer_ != VK_NULL_HANDLE && !finalize_pending_queue_batch()) return;
        const auto discardedQueue = activeQueue_ != VK_NULL_HANDLE ? activeQueue_ : graphicsQueue_;
        const bool discardPresent = swapchain_ != VK_NULL_HANDLE &&
            activeQueueKind_ == RenderQueue::Graphics && imageAvailable_ != VK_NULL_HANDLE;
        VkSemaphore discardSignal = VK_NULL_HANDLE;
        if (activeCommandBuffer_ != VK_NULL_HANDLE) {
            if (renderPassActive_) {
                vkCmdEndRenderPass(activeCommandBuffer_);
                renderPassActive_ = false;
            }
            if (discardPresent && swapchainPrepared_) {
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.dstAccessMask = 0;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                barrier.image = swapchainImages_[imageIndex_];
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.layerCount = 1;
                vkCmdPipelineBarrier(activeCommandBuffer_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            }
            vkEndCommandBuffer(activeCommandBuffer_);
            std::vector<VkSemaphore> waits;
            std::vector<VkPipelineStageFlags> stages;
            if (currentBatchWaitsForAcquire_ && imageAvailable_ != VK_NULL_HANDLE) {
                waits.push_back(imageAvailable_);
                stages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            }
            for (const auto waitBatch : currentWaitBatches_) {
                if (waitBatch < batchSignals_.size() && batchSignals_[waitBatch] != VK_NULL_HANDLE) {
                    waits.push_back(batchSignals_[waitBatch]);
                    stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                }
            }
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = static_cast<std::uint32_t>(waits.size());
            submit.pWaitSemaphores = waits.data();
            submit.pWaitDstStageMask = stages.data();
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &activeCommandBuffer_;
            if (discardPresent) {
                VkSemaphoreCreateInfo semaphoreInfo{};
                semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &discardSignal) == VK_SUCCESS) {
                    submit.signalSemaphoreCount = 1;
                    submit.pSignalSemaphores = &discardSignal;
                }
            }
            if (discardedQueue == VK_NULL_HANDLE || frameFence_ == VK_NULL_HANDLE ||
                vkQueueSubmit(discardedQueue, 1, &submit, frameFence_) != VK_SUCCESS) {
                lastError_ = "Vulkan discarded frame submission failed";
                capabilities_.deviceState = RenderDeviceState::Lost;
                if (discardSignal != VK_NULL_HANDLE) vkDestroySemaphore(device_, discardSignal, nullptr);
                discardSignal = VK_NULL_HANDLE;
            }
            activeCommandBuffer_ = VK_NULL_HANDLE;
        } else if (discardedQueue != VK_NULL_HANDLE && frameFence_ != VK_NULL_HANDLE) {
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (discardPresent) vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &discardSignal);
            VkSemaphore waits[] = {imageAvailable_};
            VkPipelineStageFlags stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = discardPresent ? 1u : 0u;
            submit.pWaitSemaphores = discardPresent ? waits : nullptr;
            submit.pWaitDstStageMask = discardPresent ? stages : nullptr;
            submit.signalSemaphoreCount = discardSignal != VK_NULL_HANDLE ? 1u : 0u;
            submit.pSignalSemaphores = discardSignal != VK_NULL_HANDLE ? &discardSignal : nullptr;
            if (vkQueueSubmit(discardedQueue, 1, &submit, frameFence_) != VK_SUCCESS) {
                lastError_ = "Vulkan discarded empty frame submission failed";
                capabilities_.deviceState = RenderDeviceState::Lost;
                if (discardSignal != VK_NULL_HANDLE) vkDestroySemaphore(device_, discardSignal, nullptr);
                discardSignal = VK_NULL_HANDLE;
            }
        }
        if (discardSignal != VK_NULL_HANDLE) {
            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &discardSignal;
            present.swapchainCount = 1;
            present.pSwapchains = &swapchain_;
            present.pImageIndices = &imageIndex_;
            const auto presentResult = vkQueuePresentKHR(presentQueue_, &present);
            if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR) {
                lastError_ = "Vulkan discarded frame present failed result=" + std::to_string(static_cast<int>(presentResult));
                capabilities_.deviceState = presentResult == VK_ERROR_DEVICE_LOST ? RenderDeviceState::Lost : RenderDeviceState::NeedsResize;
            }
            batchSignals_.push_back(discardSignal);
            discardSignal = VK_NULL_HANDLE;
        }
        activeQueue_ = VK_NULL_HANDLE;
        currentBatchWaitsForAcquire_ = false;
        currentWaitBatches_.clear();
        swapchainPrepared_ = false;
        renderPassActive_ = false;
        activeRenderPass_ = VK_NULL_HANDLE;
        frameActive_ = false;
        boundPipeline_ = nullptr;
        boundMaterial_ = nullptr;
    }
    void wait_idle() override {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            collect_garbage();
        }
    }
    RenderStats stats() const noexcept override { return stats_; }
};
}
std::unique_ptr<IRenderBackend> create_vulkan_backend() {
    return std::make_unique<VulkanBackend>();
}
}
#endif
