#pragma once

#include "shinkou/render/RenderTypes.h"
#include "shinkou/render/TextureModel.h"
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ImDrawData;

namespace shinkou::render {
struct RenderBackendConfig {
    void* nativeWindow{nullptr};
    std::uint32_t width{1280};
    std::uint32_t height{720};
    bool vsync{true};
    bool enableValidation{false};
};

enum class RenderDeviceState { Uninitialized, Ready, NeedsResize, Lost };

struct RenderPassContext {
    std::string_view name;
    std::vector<ResourceHandle> reads;
    std::vector<ResourceHandle> writes;
    std::vector<ResourceAccess> accesses;
    RenderQueue queue{RenderQueue::Graphics};
};

struct BufferUpdate {
    ResourceHandle buffer{};
    std::size_t offset{0};
    std::vector<std::uint8_t> data;
};

struct TextureUpdate {
    ResourceHandle texture{};
    std::uint32_t mipLevel{0};
    std::uint32_t layer{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::size_t rowPitch{0};
    std::vector<std::uint8_t> data;
    std::uint32_t x{0};
    std::uint32_t y{0};
};

using TextureSubresourceUpdate = TextureUpdate;

struct RenderCapabilities {
    BackendApi api{BackendApi::Null};
    bool deviceReady{false};
    RenderDeviceState deviceState{RenderDeviceState::Uninitialized};
    bool supportsCompute{false};
    bool supportsBindless{false};
    bool supportsRayTracing{false};
    bool supportsMeshShaders{false};
    bool supportsVariableRateShading{false};
    bool supportsMultiDrawIndirect{false};
    bool supportsDedicatedComputeQueue{false};
    bool supportsDedicatedCopyQueue{false};
    bool supportsValidation{false};
    // Native backends may expose pooled GPU memory allocation while keeping
    // committed-resource fallback available when device limits reject it.
    bool supportsGpuMemoryAllocator{false};
    bool supportsGpuMemoryAliasing{false};
    TextureCapabilities textureCapabilities{};
};

struct RenderStats {
    std::uint64_t frames{0};
    std::uint64_t passes{0};
    std::uint64_t drawCalls{0};
    std::uint64_t spriteCalls{0};
    std::uint64_t meshCalls{0};
    std::uint64_t indirectDrawCalls{0};
    std::uint64_t dispatchCalls{0};
    std::uint64_t resourceCreates{0};
    std::uint64_t resourceDestroys{0};
    std::uint64_t barriers{0};
    std::vector<std::uint64_t> gpuPassNanoseconds;
    std::uint64_t debugMarkers{0};
    std::uint64_t validationMessages{0};
    std::vector<std::string> validationDiagnostics;
    std::uint64_t queueSubmissions{0};
    std::uint64_t pipelineCacheHits{0};
    std::uint64_t materialBinds{0};
    std::uint64_t descriptorBinds{0};
    std::uint64_t discardedFrames{0};
};

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;
    virtual RenderCapabilities capabilities() const noexcept = 0;
    // Optional compatibility surface. Existing backends inherit this safely.
    virtual TextureCapabilities texture_capabilities() const noexcept {
        const auto renderCapabilities = capabilities();
        return renderCapabilities.textureCapabilities.known
            ? renderCapabilities.textureCapabilities
            : TextureCapabilities::for_backend(renderCapabilities.api);
    }
    virtual TextureValidationResult validate_texture(const TextureDesc& description) const {
        return validate_texture_desc(description, texture_capabilities());
    }
    virtual TextureValidationResult validate_texture_view(const TextureDesc& texture,
                                                          const TextureViewDesc& view) const {
        return shinkou::render::validate_texture_view(texture, view, texture_capabilities());
    }
    virtual bool resolve_texture(ResourceHandle, ResourceHandle) { return false; }
    virtual bool initialize(const RenderBackendConfig& config) = 0;
    virtual bool resize(std::uint32_t, std::uint32_t) { return true; }
    virtual bool create_resource(ResourceHandle handle, const ResourceDesc& desc) = 0;
    virtual bool alias_resource(ResourceHandle logical, ResourceHandle physical) {
        (void)logical;
        (void)physical;
        return false;
    }
    virtual void destroy_resource(ResourceHandle handle) = 0;
    virtual bool update_buffer(const BufferUpdate&) { return false; }
    virtual bool update_texture(const TextureUpdate&) { return false; }
    // Uploads issued between begin_graph() and submit() are recorded into one
    // backend batch when supported, so the render graph can consume them in
    // submission order without forcing a CPU wait per update.
    virtual void begin_upload_batch() {}
    virtual bool flush_upload_batch() { return true; }
    virtual bool generate_mips(ResourceHandle) { return false; }
    virtual BindlessTableHandle create_bindless_table(const BindlessTableDesc&) { return {}; }
    virtual bool update_bindless(BindlessTableHandle, std::uint32_t, ResourceHandle, DescriptorType) { return false; }
    virtual void destroy_bindless_table(BindlessTableHandle) {}
    virtual void bind_bindless_table(BindlessTableHandle) {}
    virtual void retire_resource(ResourceHandle handle) { destroy_resource(handle); }
    virtual void collect_garbage() {}
    virtual bool transition_resource(ResourceHandle, ResourceUsage) { return true; }
    virtual void set_render_target(ResourceHandle) {}
    virtual void set_render_targets(ResourceHandle color, ResourceHandle depth) {
        (void)depth;
        set_render_target(color);
    }
    virtual void set_render_targets(const std::vector<ResourceHandle>& colors, ResourceHandle depth,
                                    bool clearAttachments = true) {
        (void)clearAttachments;
        set_render_targets(colors.empty() ? ResourceHandle{} : colors.front(), depth);
    }
    virtual void set_render_targets(ResourceHandle color, ResourceHandle depth, bool clearAttachments) {
        (void)clearAttachments;
        set_render_targets(color, depth);
    }
    virtual void set_viewport(float x, float y, float width, float height, float minDepth = 0.0f, float maxDepth = 1.0f) {
        (void)x; (void)y; (void)width; (void)height; (void)minDepth; (void)maxDepth;
    }
    virtual void set_debug_name(ResourceHandle handle, std::string_view name) {
        (void)handle; (void)name;
    }
    virtual void begin_debug_label(std::string_view name) { (void)name; }
    virtual void end_debug_label() {}
    virtual void begin_frame() = 0;
    virtual bool initialize_imgui() { return false; }
    virtual void shutdown_imgui() {}
    virtual void render_imgui(ImDrawData*) {}
    virtual bool begin_queue(RenderQueue) { return true; }
    virtual bool begin_queue(RenderQueue queue, std::uint32_t batchIndex,
                             const std::vector<std::uint32_t>& waitBatches) {
        (void)batchIndex;
        (void)waitBatches;
        return begin_queue(queue);
    }
    virtual bool end_queue(RenderQueue, std::uint32_t, bool) { return true; }
    virtual bool end_queue(RenderQueue queue, std::uint32_t batchIndex, bool lastBatch, bool present) {
        (void)present;
        return end_queue(queue, batchIndex, lastBatch);
    }
    virtual void begin_pass() {}
    virtual void execute(const RenderPassContext& pass) = 0;
    virtual void end_pass() {}
    virtual void bind_pipeline(std::string_view pipelineName) = 0;
    virtual void bind_pipeline(ResourceHandle pipeline) = 0;
    virtual void bind_material(ResourceHandle material) { (void)material; }
    virtual bool update_material(ResourceHandle material, const MaterialDesc& description) {
        (void)material;
        (void)description;
        return false;
    }
    virtual void bind_uniform_buffer(ResourceHandle buffer, std::uint32_t slot, std::uint32_t space = 0) {
        (void)buffer; (void)slot; (void)space;
    }
    virtual void draw_sprite(const SpriteDraw& draw) = 0;
    virtual void draw_mesh(const MeshDraw& draw) = 0;
    virtual void draw_mesh_indirect(const IndirectMeshDraw& draw) { (void)draw; }
    virtual bool dispatch(const DispatchDesc& dispatch) { (void)dispatch; return false; }
    virtual void end_frame() = 0;
    virtual void discard_frame() {}
    virtual void wait_idle() {}
    virtual void clear_error() {}
    virtual std::string last_error() const { return {}; }
    virtual RenderStats stats() const noexcept = 0;
};

std::unique_ptr<IRenderBackend> create_backend(BackendApi api);
}
