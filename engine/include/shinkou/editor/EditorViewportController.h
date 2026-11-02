#pragma once
#include "shinkou/editor/EditorPreferences.h"
#include "shinkou/editor/EditorPresentation.h"
#include "shinkou/render/RenderScene.h"

namespace shinkou::editor {
enum class EditorTransformTool : std::int8_t { Select = -1, Translate = 0, Rotate = 1, Scale = 2 };
class EditorViewportController {
    EditorTransformTool tool_{EditorTransformTool::Translate};
    float focusDistance_{5};
    math::Transform pose_{};
    render::CameraComponent camera_{};
    bool initialized_{false};

  public:
    void initialize_from_world(const World &);
    bool initialized() const noexcept {
        return initialized_;
    }
    bool restore(const math::Transform &, float focus, float orthographicSize,
                 bool orthographic) noexcept;
    const math::Transform &pose() const noexcept {
        return pose_;
    }
    const render::CameraComponent &camera() const noexcept {
        return camera_;
    }
    render::RenderView view(float aspect) const {
        return render::make_render_view(camera_, pose_, aspect);
    }
    void apply_settings(const EditorCameraSettings &settings) noexcept {
        configure_camera(camera_, settings);
    }
    void navigate(ViewportNavigation action, math::Vec2 delta, float height, float dpi,
                  const EditorCameraSettings &settings) noexcept {
        navigate(camera_, pose_, action, delta, height, dpi, settings);
    }
    void fly(const EditorActionBatch &actions, float seconds,
             const EditorCameraSettings &settings) noexcept {
        fly(pose_, movement(actions), seconds, movement_speed(actions, settings),
            settings.planarFlight);
    }
    void set_view_mode(std::string_view);
    void frame_selection(math::Vec3 center, float radius, float aspect) noexcept;
    EditorTransformTool tool() const noexcept {
        return tool_;
    }
    void set_tool(EditorTransformTool tool) noexcept {
        tool_ = tool;
    }
    float &focus_distance() noexcept {
        return focusDistance_;
    }
    float focus_distance() const noexcept {
        return focusDistance_;
    }
    void navigate(render::CameraComponent &, math::Transform &, ViewportNavigation, math::Vec2,
                  float height, float dpi, const EditorCameraSettings &) noexcept;
    static void fly(math::Transform &, math::Vec3 localDirection, float seconds, float speed,
                    bool planar) noexcept;
    static void configure_camera(render::CameraComponent &, const EditorCameraSettings &) noexcept;
    static math::Vec3 movement(const EditorActionBatch &) noexcept;
    static float movement_speed(const EditorActionBatch &, const EditorCameraSettings &) noexcept;
};
} // namespace shinkou::editor
