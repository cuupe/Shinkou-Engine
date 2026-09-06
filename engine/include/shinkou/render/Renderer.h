#pragma once

#include "shinkou/render/RenderGraph.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace shinkou::ui { class UiRenderList; }

namespace shinkou::render {
class Renderer {
    std::unique_ptr<IRenderBackend> backend_;
    RenderGraph graph_;
public:
    struct PersistentResource {
        ResourceHandle handle;
        ResourceDesc description;
    };
private:
    std::vector<PersistentResource> persistentResources_;
    std::unordered_map<std::string, ResourceHandle> pipelineCache_;
    std::unordered_map<std::string, ResourceHandle> shaderCache_;
    struct PersistentBindlessTable {
        BindlessTableDesc description{};
        BindlessTableHandle handle{};
        BindlessTableHandle backendHandle{};
        std::vector<std::pair<std::uint32_t, ResourceHandle>> entries;
    };
    std::vector<PersistentBindlessTable> persistentBindlessTables_;
    // Persistent renderer resources use a separate namespace from graph-owned
    // transient resources (0x80000000..0xffffffff).
    std::uint32_t nextPersistentResource_{0x40000000u};
    std::uint32_t nextPersistentBindlessTable_{0x40000000u};
    ImDrawData* imguiDrawData_{nullptr};
    const ::shinkou::ui::UiRenderList* editorUiRenderList_{nullptr};
    bool imguiReady_{false};
    bool initialized_{false};
    bool editorFrameActive_{false};
    EditorViewportSeam editorViewport_{};
    std::string lastError_;
    BackendApi api_{BackendApi::Null};
    RenderBackendConfig config_{};
    ResourceHandle allocate_persistent_resource(ResourceKind kind) noexcept;
    PersistentResource* find_persistent_resource(ResourceHandle handle) noexcept;
    const PersistentResource* find_persistent_resource(ResourceHandle handle) const noexcept;
    bool restore_persistent_state(IRenderBackend& backend);
public:
    explicit Renderer(BackendApi api = BackendApi::Null, RenderBackendConfig config = {});
    ~Renderer();
    bool initialize();
    bool initialize_imgui();
    void shutdown_imgui();
    bool imgui_ready() const noexcept { return imguiReady_; }
    void set_imgui_draw_data(ImDrawData* drawData) noexcept { imguiDrawData_ = drawData; }
    void set_editor_ui_render_list(const ::shinkou::ui::UiRenderList* renderList) noexcept {
        editorUiRenderList_ = renderList;
    }
    // Starts a per-frame editor contract. A RenderView integration must call
    // set_editor_viewport() after this and before adding scene passes. The
    // contract is cleared at the next editor frame so stale geometry cannot
    // accidentally render after a resize or dock-layout change.
    void begin_editor_frame() noexcept;
    bool set_editor_viewport(EditorViewportSeam seam);
    void clear_editor_viewport() noexcept;
    const EditorViewportSeam& editor_viewport() const noexcept { return editorViewport_; }
    bool editor_frame_active() const noexcept { return editorFrameActive_; }
    bool recover();
    bool resize(std::uint32_t width, std::uint32_t height);
    void set_config(RenderBackendConfig config);
    void begin_graph();
    RenderGraph& graph() noexcept { return graph_; }
    const RenderGraph& graph() const noexcept { return graph_; }
    ResourceHandle create_texture(const TextureDesc& description);
    ResourceHandle create_sampler(const SamplerDesc& description);
    ResourceHandle create_depth_stencil(const TextureDesc& description);
    ResourceHandle create_buffer(const BufferDesc& description);
    ResourceHandle create_shader(const ShaderDesc& description);
    bool reload_shader(ResourceHandle shader, const ShaderDesc& description);
    ResourceHandle create_pipeline(const PipelineDesc& description);
    ResourceHandle create_material(const MaterialDesc& description);
    bool update_material(ResourceHandle material, const MaterialDesc& description);
    bool update_material_binding(ResourceHandle material, const DescriptorBinding& binding);
    const ResourceDesc* resource_description(ResourceHandle handle) const noexcept;
    void destroy_resource(ResourceHandle handle);
    bool update_buffer(const BufferUpdate& update);
    bool update_texture(const TextureUpdate& update);
    bool generate_mips(ResourceHandle texture);
    BindlessTableHandle create_bindless_table(const BindlessTableDesc& description);
    bool update_bindless(BindlessTableHandle table, std::uint32_t slot, ResourceHandle resource,
                         DescriptorType type = DescriptorType::Texture);
    void destroy_bindless_table(BindlessTableHandle table);
    void bind_bindless_table(BindlessTableHandle table);
    const std::string& last_error() const noexcept { return lastError_; }
    const RenderCapabilities capabilities() const noexcept;
    const RenderStats stats() const noexcept;
    IRenderBackend* backend() noexcept { return backend_.get(); }
    const IRenderBackend* backend() const noexcept { return backend_.get(); }
    void submit();
};
}
