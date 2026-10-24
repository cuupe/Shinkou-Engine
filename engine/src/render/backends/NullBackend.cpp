#include "BackendCommon.h"

namespace shinkou::render {
namespace {
class NullBackend final : public IRenderBackend {
    RenderCapabilities capabilities_{};
    RenderStats stats_{};
    std::unordered_map<std::uint32_t, MaterialDesc> materials_;
    std::unordered_map<std::uint32_t, ResourceUsage> resourceUsages_;
    std::unordered_map<std::uint32_t, ResourceKind> resourceKinds_;
    std::vector<bool> submittedQueueBatches_;
    std::string lastError_;
public:
    explicit NullBackend(BackendApi api) {
        capabilities_.api = api;
        capabilities_.deviceReady = api == BackendApi::Null;
        capabilities_.deviceState = api == BackendApi::Null ? RenderDeviceState::Ready : RenderDeviceState::Uninitialized;
        capabilities_.supportsImGui = false;
        capabilities_.supportsNativeUi = false;
        capabilities_.supportsCompute = api != BackendApi::Null;
        capabilities_.supportsMultiDrawIndirect = api == BackendApi::DirectX12 || api == BackendApi::Vulkan;
        capabilities_.supportsBindless = false;
        capabilities_.supportsDedicatedComputeQueue = false;
        capabilities_.supportsDedicatedCopyQueue = false;
    }
    RenderCapabilities capabilities() const noexcept override { return capabilities_; }
    bool initialize(const RenderBackendConfig&) override { return true; }
    bool create_resource(ResourceHandle handle, const ResourceDesc& description) override {
        if (!handle || !resource_description_matches_kind(handle.kind, description)) {
            lastError_ = "Null backend resource handle and description kind do not match";
            return false;
        }
        ++stats_.resourceCreates;
        resourceUsages_[handle.id] = ResourceUsage::Unknown;
        resourceKinds_[handle.id] = handle.kind;
        if (const auto* material = std::get_if<MaterialDesc>(&description)) materials_[handle.id] = *material;
        return true;
    }
    bool alias_resource(ResourceHandle logical, ResourceHandle physical) override {
        const auto source = resourceKinds_.find(physical.id);
        if (!logical || !physical || source == resourceKinds_.end() || source->second != physical.kind ||
            source->second != logical.kind) {
            lastError_ = "Null backend alias handles or resource kinds are invalid";
            return false;
        }
        resourceKinds_[logical.id] = logical.kind;
        resourceUsages_[logical.id] = resourceUsages_[physical.id];
        return true;
    }
    void destroy_resource(ResourceHandle handle) override {
        const auto kind = resourceKinds_.find(handle.id);
        if (kind == resourceKinds_.end() || kind->second != handle.kind) return;
        ++stats_.resourceDestroys;
        materials_.erase(handle.id);
        resourceUsages_.erase(handle.id);
        resourceKinds_.erase(handle.id);
    }
    bool update_material(ResourceHandle handle, const MaterialDesc& description) override {
        if (handle.kind != ResourceKind::Material) {
            lastError_ = "Null backend material handle has the wrong resource kind";
            return false;
        }
        materials_[handle.id] = description;
        return true;
    }
    bool update_buffer(const BufferUpdate& update) override {
        if (!update.buffer || update.buffer.kind != ResourceKind::Buffer || update.data.empty()) {
            lastError_ = "Null backend buffer update handle or data is invalid";
            return false;
        }
        return true;
    }
    bool update_texture(const TextureUpdate& update) override {
        if (!update.texture || update.texture.kind != ResourceKind::Texture2D || update.data.empty() ||
            update.width == 0 || update.height == 0) {
            lastError_ = "Null backend texture update handle or data is invalid";
            return false;
        }
        return true;
    }
    bool generate_mips(ResourceHandle texture) override {
        if (!texture || texture.kind != ResourceKind::Texture2D) {
            lastError_ = "Null backend mip generation handle has the wrong resource kind";
            return false;
        }
        return true;
    }
    void begin_frame() override { ++stats_.frames; submittedQueueBatches_.clear(); }
    bool begin_queue(RenderQueue, std::uint32_t batchIndex,
                     const std::vector<std::uint32_t>& waitBatches) override {
        for (const auto waitBatch : waitBatches) {
            if (waitBatch >= submittedQueueBatches_.size() || !submittedQueueBatches_[waitBatch]) {
                lastError_ = "Null backend queue batch wait references an unsignaled batch";
                return false;
            }
        }
        if (submittedQueueBatches_.size() <= batchIndex) submittedQueueBatches_.resize(batchIndex + 1, false);
        submittedQueueBatches_[batchIndex] = true;
        ++stats_.queueSubmissions;
        return true;
    }
    bool transition_resource(ResourceHandle handle, ResourceUsage usage) override {
        const auto it = resourceUsages_.find(handle.id);
        const auto kind = resourceKinds_.find(handle.id);
        if (it == resourceUsages_.end() || kind == resourceKinds_.end() || kind->second != handle.kind) {
            lastError_ = "Null backend resource handle or kind is invalid";
            return false;
        }
        if (it->second == usage) return true;
        it->second = usage;
        ++stats_.barriers;
        return true;
    }
    bool clear_render_attachments(const std::vector<ResourceHandle>& colors,ResourceHandle depth,bool clearColors,bool clearDepth) override {
        (void)colors; (void)depth; (void)clearColors; (void)clearDepth;
        return true;
    }
    void begin_debug_label(std::string_view) override { ++stats_.debugMarkers; }
    void set_debug_name(ResourceHandle, std::string_view) override {}
    void execute(const RenderPassContext&) override { ++stats_.passes; }
    void bind_pipeline(std::string_view) override {}
    void bind_pipeline(ResourceHandle handle) override {
        if (handle && handle.kind != ResourceKind::Pipeline) lastError_ = "Null backend pipeline handle has the wrong resource kind";
    }
    void bind_material(ResourceHandle handle) override {
        if (handle.kind != ResourceKind::Material) {
            lastError_ = "Null backend material handle has the wrong resource kind";
            return;
        }
        const auto material = materials_.find(handle.id);
        if (material == materials_.end()) {
            lastError_ = "Null backend material handle is invalid";
            return;
        }
        ++stats_.materialBinds;
        for (const auto& binding : material->second.bindings) {
            const auto count = binding.resources.empty() ? (binding.resource ? 1u : 0u)
                : static_cast<std::uint32_t>(binding.resources.size());
            if (count != 0) ++stats_.descriptorBinds;
        }
    }
    void draw_sprite(const SpriteDraw&) override { ++stats_.drawCalls; ++stats_.spriteCalls; }
    void draw_mesh(const MeshDraw&) override { ++stats_.drawCalls; ++stats_.meshCalls; }
    void draw_mesh_indirect(const IndirectMeshDraw& draw) override {
        if (!draw.argumentBuffer || !draw.vertexBuffer || !draw.indexBuffer || draw.maxDrawCount == 0) return;
        ++stats_.drawCalls;
        ++stats_.meshCalls;
        ++stats_.indirectDrawCalls;
    }
    bool dispatch(const DispatchDesc& dispatch) override {
        if (dispatch.groupCountX == 0 || dispatch.groupCountY == 0 || dispatch.groupCountZ == 0) return false;
        ++stats_.dispatchCalls;
        return true;
    }
    void end_frame() override {}
    void discard_frame() override { ++stats_.discardedFrames; }
    void wait_idle() override {}
    void clear_error() override { lastError_.clear(); }
    std::string last_error() const override { return lastError_; }
    RenderStats stats() const noexcept override { return stats_; }
};

}
std::unique_ptr<IRenderBackend> create_null_backend(BackendApi api) {
    return std::make_unique<NullBackend>(api);
}
}
