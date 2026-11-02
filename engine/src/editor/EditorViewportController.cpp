#include "shinkou/editor/EditorViewportController.h"
#include "shinkou/World.h"
#include "shinkou/editor/EditorWorldViewport.h"
#include <algorithm>
#include <cmath>

namespace shinkou::editor {
bool EditorViewportController::restore(const math::Transform &pose, float focus,
                                       float orthographicSize, bool orthographic) noexcept {
    for (float value : math::TransformMatrix(pose).m)
        if (!std::isfinite(value))
            return false;
    if (!std::isfinite(focus) || focus <= 0 || !std::isfinite(orthographicSize) ||
        orthographicSize <= 0 || math::Dot(pose.rotation, pose.rotation) < 1e-8f)
        return false;
    pose_ = pose;
    pose_.rotation = math::Normalize(pose.rotation);
    pose_.scale = {1, 1, 1};
    focusDistance_ = focus;
    camera_.orthographic = orthographic;
    camera_.orthographicSize = orthographicSize;
    initialized_ = true;
    return true;
}
void EditorViewportController::initialize_from_world(const World &world) {
    if (initialized_)
        return;
    const auto view = render::extract_render_view(world, 16.f / 9);
    if (view.has_camera() && restore(view.transform, 5, view.projection.orthographicSize, false)) {
        const auto forward = math::Rotate(pose_.rotation, {0, 0, 1});
        const float yaw = std::atan2(forward.x, forward.z),
                    pitch = -std::asin(std::clamp(forward.y, -1.f, 1.f));
        pose_.rotation =
            math::FromAxisAngle({0, 1, 0}, yaw) * math::FromAxisAngle({1, 0, 0}, pitch);
    } else {
        pose_.position = {3, 2.4f, -6};
        pose_.rotation =
            math::FromAxisAngle({0, 1, 0}, -.42f) * math::FromAxisAngle({1, 0, 0}, .25f);
    }
    initialized_ = true;
}
void EditorViewportController::set_view_mode(std::string_view mode) {
    initialized_ = true;
    focusDistance_ = 5;
    camera_.orthographic = mode != "Perspective";
    pose_.rotation = math::Quat::Identity();
    if (mode == "Front" || mode == "Perspective")
        pose_.position = {0, 0, -5};
    else if (mode == "Back") {
        pose_.position = {0, 0, 5};
        pose_.rotation = math::FromAxisAngle({0, 1, 0}, math::Pi);
    } else if (mode == "Left") {
        pose_.position = {-5, 0, 0};
        pose_.rotation = math::FromAxisAngle({0, 1, 0}, math::HalfPi);
    } else if (mode == "Right") {
        pose_.position = {5, 0, 0};
        pose_.rotation = math::FromAxisAngle({0, 1, 0}, -math::HalfPi);
    } else if (mode == "Top") {
        pose_.position = {0, 5, 0};
        pose_.rotation = math::FromAxisAngle({1, 0, 0}, math::HalfPi);
    } else if (mode == "Bottom") {
        pose_.position = {0, -5, 0};
        pose_.rotation = math::FromAxisAngle({1, 0, 0}, -math::HalfPi);
    }
}
void EditorViewportController::frame_selection(math::Vec3 center, float radius,
                                               float aspect) noexcept {
    if (!math::IsFinite(center.x) || !math::IsFinite(center.y) || !math::IsFinite(center.z) ||
        !math::IsFinite(radius) || !math::IsFinite(aspect))
        return;
    radius = std::max(radius, .25f);
    aspect = std::max(aspect, .1f);
    const float tangent = std::tan(camera_.verticalFieldOfView * .5f) * std::min(aspect, 1.f);
    focusDistance_ =
        std::clamp(radius * 1.2f / std::sin(std::atan(tangent)), .2f, camera_.farPlane * .8f);
    if (camera_.orthographic)
        camera_.orthographicSize = radius * 2.4f / std::min(aspect, 1.f);
    pose_.position = center - math::Rotate(pose_.rotation, {0, 0, 1}) * focusDistance_;
}
void EditorViewportController::navigate(render::CameraComponent &camera, math::Transform &pose,
                                        ViewportNavigation action, math::Vec2 delta, float height,
                                        float dpi, const EditorCameraSettings &settings) noexcept {
    const float sensitivity = action == ViewportNavigation::Zoom ? settings.zoomSensitivity
                              : action == ViewportNavigation::Pan || camera.orthographic
                                  ? settings.panSensitivity
                                  : settings.lookSensitivity;
    delta = delta * sensitivity;
    if (action == ViewportNavigation::Orbit && !camera.orthographic && settings.invertY)
        delta.y = -delta.y;
    navigate_editor_camera(camera, pose, action, delta, height, dpi, focusDistance_);
}
void EditorViewportController::configure_camera(render::CameraComponent &camera,
                                                const EditorCameraSettings &settings) noexcept {
    camera.verticalFieldOfView = settings.fieldOfView * math::Pi / 180;
    camera.nearPlane = settings.nearPlane;
    camera.farPlane = settings.farPlane;
}
void EditorViewportController::fly(math::Transform &pose, math::Vec3 direction, float seconds,
                                   float speed, bool planar) noexcept {
    if (!math::IsFinite(seconds) || !math::IsFinite(speed) ||
        !math::IsFinite(math::LengthSquared(direction)) || seconds <= 0 || speed <= 0 ||
        math::LengthSquared(direction) < 1e-8f)
        return;
    direction = math::Normalize(direction);
    auto forward = math::Rotate(pose.rotation, {0, 0, 1});
    auto right = math::Rotate(pose.rotation, {1, 0, 0});
    if (planar) {
        forward.y = right.y = 0;
        forward =
            math::LengthSquared(forward) > 1e-8f ? math::Normalize(forward) : math::Vec3{0, 0, 1};
        right = math::LengthSquared(right) > 1e-8f ? math::Normalize(right) : math::Vec3{1, 0, 0};
    }
    const auto worldDirection =
        right * direction.x + math::Vec3{0, 1, 0} * direction.y + forward * direction.z;
    if (math::LengthSquared(worldDirection) < 1e-8f)
        return;
    pose.position =
        pose.position + math::Normalize(worldDirection) * (std::clamp(seconds, 0.0f, .1f) * speed);
}
math::Vec3 EditorViewportController::movement(const EditorActionBatch &actions) noexcept {
    return {float(actions.contains("camera.right")) - float(actions.contains("camera.left")),
            float(actions.contains("camera.up")) - float(actions.contains("camera.down")),
            float(actions.contains("camera.forward")) - float(actions.contains("camera.back"))};
}
float EditorViewportController::movement_speed(const EditorActionBatch &actions,
                                               const EditorCameraSettings &settings) noexcept {
    return settings.flySpeed * (actions.contains("camera.boost") ? settings.fastMultiplier : 1) *
           (actions.contains("camera.precise") ? settings.preciseMultiplier : 1);
}
} // namespace shinkou::editor
