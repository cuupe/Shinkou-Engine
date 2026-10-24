#pragma once

#include "shinkou/editor/EditorModelSceneRenderer.h"
#include "shinkou/editor/EditorUi.h"
#include "shinkou/editor/EditorViewportGrid.h"
#include "shinkou/render/RenderScene.h"
#include "shinkou/render/SkyRenderer.h"
#include <optional>

namespace shinkou::editor {

struct ViewportRay { math::Vec3 origin{}, direction{}; float maxDistance{1000000.0f}; };
std::optional<ViewportRay> make_viewport_ray(const math::Mat4& viewProjection,
    math::Vec2 localPixels, math::Vec2 physicalSize) noexcept;
std::optional<float> intersect_viewport_triangle(const ViewportRay& ray,
    math::Vec3 a, math::Vec3 b, math::Vec3 c) noexcept;
void navigate_editor_camera(render::CameraComponent& camera, math::Transform& transform,
    ViewportNavigation action, math::Vec2 physicalDelta, float physicalHeight,
    float dpiScale, float& focusDistance) noexcept;

// Each docked view owns its framebuffer and frame uniforms. The world image
// is sampled by ImGui at native pixel density, independently of the swapchain.
class EditorWorldViewport final {
    render::ResourceHandle color_{}, depth_{};
    render::TextureDesc colorDesc_{}, depthDesc_{};
    render::ForwardRenderer forward_;
    render::SkyRenderer sky_;
    bool skyReady_{false};
    EditorViewportGrid grid_;
    EditorModelSceneRenderer models_;
    render::RenderScene scene_;
    EditorModelSceneRenderState modelState_;
public:
    bool render(render::Renderer&, const shinkou::World&, std::uint32_t width,
        std::uint32_t height, const std::vector<EditorModelSceneInstance>&,bool showGrid=false);
    ObjectId pick(const shinkou::World&, const render::Renderer&,
        const std::vector<EditorModelSceneInstance>&, math::Vec2 localPixels) const;
    void release(render::Renderer&);
    render::ResourceHandle texture() const noexcept { return color_; }
    const render::RenderView& view() const noexcept { return scene_.view(); }
    const auto& model_state() const noexcept { return modelState_; }
    std::uint32_t width() const noexcept { return colorDesc_.width; }
    std::uint32_t height() const noexcept { return colorDesc_.height; }
};
}
