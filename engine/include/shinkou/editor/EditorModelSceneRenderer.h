#pragma once

#include "shinkou/assets/AssetSystem.h"
#include "shinkou/editor/EditorModelPreview.h"
#include "shinkou/render/RenderTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace shinkou::render { class Renderer; }

namespace shinkou::editor {

// A renderer-neutral scene instance. The AssetId is the stable identity from
// the AssetSystem manifest; the immutable snapshot is the parser output and
// never owns a backend resource.
struct EditorModelSceneInstance {
    assets::AssetId assetId{0};
    std::uint64_t objectId{0};
    std::shared_ptr<const EditorModelPreviewSnapshot> snapshot{};
    math::Mat4 model{math::Mat4::Identity()};
};

struct EditorModelSceneRenderState {
    render::BackendApi backend{render::BackendApi::Null};
    bool rendererReady{false};
    bool pipelineReady{false};
    bool sceneBufferReady{false};
    std::size_t submittedInstances{0};
    std::size_t uploadedAssets{0};
    std::size_t rejectedInstances{0};
    std::uint64_t frameRevision{0};
    std::string status{"GPU model scene not attempted"};
};

// Uploads validated model snapshots once per AssetId and adds one bounded
// scene pass for the active references. This is intentionally separate from
// EditorModelPreviewRenderer: the Inspector preview has its own camera and
// offscreen target, while this class consumes the editor World camera and
// target seam.
class EditorModelSceneRenderer final {
    struct GeometryResource {
        std::uint64_t revision{0};
        render::ResourceHandle vertexBuffer{};
        render::ResourceHandle indexBuffer{};
        std::size_t vertexBytes{0};
        std::size_t indexBytes{0};
        std::uint32_t indexCount{0};
    };

    render::ResourceHandle sceneBuffer_{};
    render::ResourceHandle vertexShader_{};
    render::ResourceHandle fragmentShader_{};
    render::ResourceHandle pipeline_{};
    std::unordered_map<assets::AssetId, GeometryResource> geometry_;
    std::unordered_map<std::uint64_t, render::ResourceHandle> objectBuffers_;
    std::uint64_t sceneRevision_{0};
    std::string pipelineColorFormat_{};

    void clear_pipeline(render::Renderer& renderer) noexcept;

public:
    EditorModelSceneRenderState render(
        render::Renderer& renderer,
        const std::vector<EditorModelSceneInstance>& instances,
        const math::Mat4& viewProjection,
        math::Vec3 cameraPosition);

    // Called after a frame with the active identities. This makes deletion or
    // replacement of scene references reclaim persistent GPU buffers instead
    // of growing a cache for the lifetime of the editor process.
    void prune(render::Renderer& renderer,
               const std::vector<assets::AssetId>& activeAssets,
               const std::vector<std::uint64_t>& activeObjects) noexcept;
    void clear(render::Renderer& renderer) noexcept;

    std::size_t cached_asset_count() const noexcept { return geometry_.size(); }
    std::size_t cached_object_count() const noexcept { return objectBuffers_.size(); }
};

} // namespace shinkou::editor
