#pragma once

#include "shinkou/editor/EditorModelPreview.h"
#include "shinkou/editor/EditorModelPreviewScene.h"
#include "shinkou/render/RenderTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace shinkou::render { class Renderer; }
namespace shinkou::ui { struct UiImageSnapshot; }

namespace shinkou::editor {

enum class EditorModelPreviewTextureRole : std::uint8_t {
    BaseColor,
    Normal,
    MetallicRoughness,
};

// Presentation state for the renderer-owned model preview seam. It is kept
// separate from EditorModelPreviewSnapshot so parser/provider data remains
// immutable and usable by the retained Inspector fallback.
struct EditorModelPreviewRenderState {
    render::BackendApi backend{render::BackendApi::Null};
    bool rendererReady{false};
    bool geometryUploaded{false};
    bool materialApplied{false};
    bool materialFactorsApplied{false};
    bool textureCoordinatesApplied{false};
    bool normalsApplied{false};
    bool textureRoleApplied{false};
    bool baseColorTextureSampled{false};
    bool normalTextureSampled{false};
    bool metallicRoughnessTextureSampled{false};
    bool textureSampled{false};
    bool offscreenTargetReady{false};
    bool depthTargetReady{false};
    bool offscreenCompositeApplied{false};
    std::size_t vertexCount{0};
    std::size_t triangleCount{0};
    std::uint64_t geometryRevision{0};
    std::uint64_t projectionRevision{0};
    std::string status{"GPU model preview not attempted"};
};

// Adds one bounded model pass to the already-built editor scene graph. This
// first backend path deliberately targets DirectX 11 HLSL. Other backends
// retain the provider/WIC preview until their shader and texture upload path
// is implemented, so capability fallbacks stay explicit and auditable.
class EditorModelPreviewRenderer final {
    render::ResourceHandle vertexBuffer_{};
    render::ResourceHandle indexBuffer_{};
    render::ResourceHandle sceneBuffer_{};
    render::ResourceHandle materialBuffer_{};
    render::ResourceHandle vertexShader_{};
    render::ResourceHandle fragmentShader_{};
    render::ResourceHandle pipeline_{};
    render::ResourceHandle material_{};
    render::ResourceHandle compositeVertexShader_{};
    render::ResourceHandle compositeFragmentShader_{};
    render::ResourceHandle compositePipeline_{};
    render::ResourceHandle compositeMaterial_{};
    render::ResourceHandle baseColorTexture_{};
    render::ResourceHandle baseColorSampler_{};
    render::ResourceHandle normalTexture_{};
    render::ResourceHandle normalSampler_{};
    render::ResourceHandle metallicRoughnessTexture_{};
    render::ResourceHandle metallicRoughnessSampler_{};
    render::ResourceHandle offscreenColor_{};
    render::ResourceHandle offscreenDepth_{};
    std::size_t vertexBytes_{0};
    std::size_t indexBytes_{0};
    std::uint64_t geometryRevision_{0};
    std::uint64_t projectionKey_{0};
    std::uint64_t materialRevision_{0};
    std::int32_t materialIndex_{-2};
    EditorModelPreviewTextureRole materialTextureRole_{EditorModelPreviewTextureRole::BaseColor};
    std::uint64_t textureRevision_{0};
    std::uint64_t normalTextureRevision_{0};
    std::uint64_t metallicRoughnessTextureRevision_{0};
    std::uint32_t materialTextureId_{0};
    std::uint32_t materialSamplerId_{0};
    std::uint32_t materialNormalTextureId_{0};
    std::uint32_t normalSamplerId_{0};
    std::uint32_t materialMetallicRoughnessTextureId_{0};
    std::uint32_t metallicRoughnessSamplerId_{0};
    bool actualBaseColorTexture_{false};
    bool actualNormalTexture_{false};
    bool actualMetallicRoughnessTexture_{false};
    std::string pipelineColorFormat_{};
    std::uint32_t offscreenWidth_{0};
    std::uint32_t offscreenHeight_{0};
    std::string offscreenColorFormat_{};
    std::string offscreenDepthFormat_{};
    std::uint32_t compositeTextureId_{0};
    std::uint32_t compositeSamplerId_{0};

    void clear_pipeline(render::Renderer& renderer) noexcept;

public:
    EditorModelPreviewRenderState render(
        render::Renderer& renderer,
        const EditorModelPreviewSnapshot& snapshot,
        const EditorModelPreviewSceneState& scene,
        std::int32_t materialIndex,
        std::shared_ptr<const ui::UiImageSnapshot> textureSnapshot = {},
        EditorModelPreviewTextureRole textureRole = EditorModelPreviewTextureRole::BaseColor);

    void clear(render::Renderer& renderer) noexcept;
    const render::ResourceHandle& vertex_buffer() const noexcept { return vertexBuffer_; }
    const render::ResourceHandle& index_buffer() const noexcept { return indexBuffer_; }
    const render::ResourceHandle& offscreen_color() const noexcept { return offscreenColor_; }
    const render::ResourceHandle& offscreen_depth() const noexcept { return offscreenDepth_; }
};

} // namespace shinkou::editor
