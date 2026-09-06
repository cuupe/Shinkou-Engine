#include "shinkou/editor/RenderView.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace shinkou::editor {
namespace {

constexpr float kMinClip = 0.001f;

bool finite(float value) noexcept { return math::IsFinite(value); }
bool finite(math::Vec3 value) noexcept { return finite(value.x) && finite(value.y) && finite(value.z); }
bool finite(math::Quat value) noexcept { return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w); }
bool finite(math::Vec4 value) noexcept { return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w); }
bool finite(math::Mat4 value) noexcept {
    for (const float item : value.m) if (!finite(item)) return false;
    return true;
}

math::Vec3 mode_direction(RenderViewMode mode) noexcept {
    switch (mode) {
    case RenderViewMode::Front: return {0.0f, 0.0f, 1.0f};
    case RenderViewMode::Side: return {1.0f, 0.0f, 0.0f};
    case RenderViewMode::Top: return {0.0f, -1.0f, 0.0f};
    case RenderViewMode::Perspective: break;
    }
    return {0.0f, 0.0f, 1.0f};
}

math::Vec3 mode_up(RenderViewMode mode) noexcept {
    return mode == RenderViewMode::Top ? math::Vec3{0.0f, 0.0f, 1.0f} : math::Vec3{0.0f, 1.0f, 0.0f};
}

GridPlane resolved_plane(const RenderViewDescription& description) noexcept {
    if (description.grid.plane != GridPlane::Auto) return description.grid.plane;
    switch (description.camera.mode) {
    // Shinkou uses Y-up world coordinates: the front elevation is XY,
    // while the top view looks down onto the XZ ground plane.
    case RenderViewMode::Top: return GridPlane::XZ;
    case RenderViewMode::Front: return GridPlane::XY;
    case RenderViewMode::Side: return GridPlane::YZ;
    case RenderViewMode::Perspective: return GridPlane::XZ;
    }
    return GridPlane::XZ;
}

math::Vec3 plane_normal(GridPlane plane) noexcept {
    switch (plane) {
    case GridPlane::XY: return {0.0f, 0.0f, 1.0f};
    case GridPlane::YZ: return {1.0f, 0.0f, 0.0f};
    case GridPlane::XZ:
    case GridPlane::Auto: return {0.0f, 1.0f, 0.0f};
    }
    return {0.0f, 1.0f, 0.0f};
}

math::Vec3 plane_point(GridPlane plane, math::Vec3 center, float offset) noexcept {
    switch (plane) {
    case GridPlane::XY: return {center.x, center.y, offset};
    case GridPlane::YZ: return {offset, center.y, center.z};
    case GridPlane::XZ:
    case GridPlane::Auto: return {center.x, offset, center.z};
    }
    return center;
}

float nice_step(float desired) noexcept {
    if (!finite(desired) || desired <= kMinClip) return 1.0f;
    const float exponent = std::floor(std::log10(desired));
    const float base = std::pow(10.0f, exponent);
    const float normalized = desired / base;
    const float multiplier = normalized <= 1.0f ? 1.0f : normalized <= 2.0f ? 2.0f : normalized <= 5.0f ? 5.0f : 10.0f;
    return std::max(kMinClip, multiplier * base);
}

void set_plane_coordinates(GridPlane plane, math::Vec3& value, float first, float second) noexcept {
    switch (plane) {
    case GridPlane::XY: value.x = first; value.y = second; break;
    case GridPlane::YZ: value.y = first; value.z = second; break;
    case GridPlane::XZ:
    case GridPlane::Auto: value.x = first; value.z = second; break;
    }
}

void append_grid_line(GridGeometry& geometry, GridPlane plane, float first, float second,
                      bool horizontal, GridLineKind kind, RenderAxis axis, math::Vec4 color,
                      float firstExtent, float secondExtent, math::Vec3 center) {
    GridLine line;
    line.orientation = horizontal ? GridLineOrientation::Horizontal : GridLineOrientation::Vertical;
    line.kind = kind;
    line.axis = axis;
    line.color = color;
    line.start = center;
    line.end = center;
    if (horizontal) {
        set_plane_coordinates(plane, line.start, -firstExtent, second);
        set_plane_coordinates(plane, line.end, firstExtent, second);
    } else {
        set_plane_coordinates(plane, line.start, first, -secondExtent);
        set_plane_coordinates(plane, line.end, first, secondExtent);
    }
    geometry.lines.push_back(line);
}

void build_axis(GridGeometry& geometry, GridPlane plane, RenderAxis axis, math::Vec4 color,
                float firstExtent, float secondExtent, math::Vec3 center, bool horizontal) {
    GridAxisLine line;
    line.visible = true;
    line.axis = axis;
    line.color = color;
    line.start = center;
    line.end = center;
    if (horizontal) {
        set_plane_coordinates(plane, line.start, -firstExtent, 0.0f);
        set_plane_coordinates(plane, line.end, firstExtent, 0.0f);
    } else {
        set_plane_coordinates(plane, line.start, 0.0f, -secondExtent);
        set_plane_coordinates(plane, line.end, 0.0f, secondExtent);
    }
    geometry.axes[static_cast<std::size_t>(axis == RenderAxis::X ? 0 : axis == RenderAxis::Y ? 1 : 2)] = line;
}

} // namespace

bool WindowClientRectPx::valid() const noexcept {
    return finite(x) && finite(y) && finite(width) && finite(height) && x >= 0.0f && y >= 0.0f && width > 0.0f && height > 0.0f;
}

bool UiLogicalRect::valid() const noexcept {
    return finite(x) && finite(y) && finite(width) && finite(height) && x >= 0.0f && y >= 0.0f && width > 0.0f && height > 0.0f;
}

bool ViewportRectPx::valid() const noexcept { return x >= 0 && y >= 0 && width > 0 && height > 0; }

bool ViewportRectPx::contains(ViewportLocalPx point) const noexcept {
    return valid() && finite(point.x) && finite(point.y) && point.x >= 0.0f && point.y >= 0.0f &&
        point.x < static_cast<float>(width) && point.y < static_cast<float>(height);
}

std::optional<WindowClientPx> ui_logical_to_window_client_px(UiLogicalPx point, float dpiScale) noexcept {
    if (!finite(point.x) || !finite(point.y) || !finite(dpiScale) || dpiScale <= 0.0f) return std::nullopt;
    return WindowClientPx{point.x * dpiScale, point.y * dpiScale};
}

std::optional<UiLogicalPx> window_client_to_ui_logical_px(WindowClientPx point, float dpiScale) noexcept {
    if (!finite(point.x) || !finite(point.y) || !finite(dpiScale) || dpiScale <= 0.0f) return std::nullopt;
    return UiLogicalPx{point.x / dpiScale, point.y / dpiScale};
}

std::optional<WindowClientRectPx> ui_logical_to_window_client_rect_px(UiLogicalRect rect, float dpiScale) noexcept {
    if (!rect.valid() || !finite(dpiScale) || dpiScale <= 0.0f) return std::nullopt;
    return WindowClientRectPx{rect.x * dpiScale, rect.y * dpiScale, rect.width * dpiScale, rect.height * dpiScale};
}

std::optional<UiLogicalRect> window_client_to_ui_logical_rect_px(WindowClientRectPx rect, float dpiScale) noexcept {
    if (!rect.valid() || !finite(dpiScale) || dpiScale <= 0.0f) return std::nullopt;
    return UiLogicalRect{rect.x / dpiScale, rect.y / dpiScale, rect.width / dpiScale, rect.height / dpiScale};
}

std::optional<ViewportRectPx> ui_logical_to_viewport_rect_px(UiLogicalRect rect, float dpiScale) noexcept {
    const auto physical = ui_logical_to_window_client_rect_px(rect, dpiScale);
    if (!physical) return std::nullopt;
    const auto round_to_int = [](float value) -> std::int32_t {
        if (!finite(value)) return 0;
        const auto rounded = std::llround(value);
        return rounded >= 0 && rounded <= std::numeric_limits<std::int32_t>::max() ? static_cast<std::int32_t>(rounded) : 0;
    };
    const ViewportRectPx result{round_to_int(physical->x), round_to_int(physical->y), round_to_int(physical->width), round_to_int(physical->height)};
    return result.valid() ? std::optional<ViewportRectPx>(result) : std::nullopt;
}

std::optional<ViewportLocalPx> window_client_to_viewport_local_px(WindowClientPx point, const ViewportRectPx& viewport) noexcept {
    if (!viewport.valid() || !finite(point.x) || !finite(point.y)) return std::nullopt;
    return ViewportLocalPx{point.x - static_cast<float>(viewport.x), point.y - static_cast<float>(viewport.y)};
}

std::optional<WindowClientPx> viewport_local_to_window_client_px(ViewportLocalPx point, const ViewportRectPx& viewport) noexcept {
    if (!viewport.valid() || !finite(point.x) || !finite(point.y)) return std::nullopt;
    return WindowClientPx{point.x + static_cast<float>(viewport.x), point.y + static_cast<float>(viewport.y)};
}

std::optional<Ndc> viewport_local_to_ndc(ViewportLocalPx point, const ViewportRectPx& viewport) noexcept {
    if (!viewport.contains(point)) return std::nullopt;
    return Ndc{point.x / static_cast<float>(viewport.width) * 2.0f - 1.0f,
        1.0f - point.y / static_cast<float>(viewport.height) * 2.0f, 0.0f};
}

std::optional<ViewportLocalPx> ndc_to_viewport_local(Ndc ndc, const ViewportRectPx& viewport) noexcept {
    if (!viewport.valid() || !finite(ndc.x) || !finite(ndc.y) || !finite(ndc.z)) return std::nullopt;
    return ViewportLocalPx{(ndc.x + 1.0f) * 0.5f * static_cast<float>(viewport.width),
        (1.0f - ndc.y) * 0.5f * static_cast<float>(viewport.height)};
}

bool RenderView::configure(const RenderViewDescription& description) noexcept {
    const auto scissor = description.scissor.valid() ? description.scissor : description.viewport;
    const auto& camera = description.camera;
    const bool validDescription = description.viewport.valid() && scissor.valid() &&
        scissor.x >= description.viewport.x && scissor.y >= description.viewport.y &&
        scissor.x + scissor.width <= description.viewport.x + description.viewport.width &&
        scissor.y + scissor.height <= description.viewport.y + description.viewport.height &&
        finite(description.dpiScale) && description.dpiScale > 0.0f && finite(camera.position) && finite(camera.rotation) && finite(camera.focalPoint) &&
        finite(camera.orbitDistance) && camera.orbitDistance > 0.0f && finite(camera.verticalFieldOfView) && camera.verticalFieldOfView > kMinClip && camera.verticalFieldOfView < math::Pi - kMinClip &&
        finite(camera.orthographicSize) && camera.orthographicSize > kMinClip && finite(camera.nearPlane) && finite(camera.farPlane) && camera.nearPlane >= 0.0f && camera.farPlane > camera.nearPlane;
    if (!validDescription) {
        valid_ = false;
        description_ = {};
        viewMatrix_ = projectionMatrix_ = viewProjectionMatrix_ = inverseViewProjectionMatrix_ = math::Mat4::Identity();
        return false;
    }

    RenderViewDescription normalized = description;
    normalized.scissor = scissor;
    const float aspect = static_cast<float>(description.viewport.width) / static_cast<float>(description.viewport.height);
    math::Vec3 eye = camera.position;
    math::Vec3 target = camera.focalPoint;
    math::Vec3 up = mode_up(camera.mode);
    if (camera.mode != RenderViewMode::Perspective) {
        eye = target - mode_direction(camera.mode) * std::max(camera.orbitDistance, kMinClip);
        if (camera.mode == RenderViewMode::Top) eye = target + math::Vec3{0.0f, std::max(camera.orbitDistance, kMinClip), 0.0f};
    } else if (math::LengthSquared(target - eye) <= math::Epsilon) {
        const auto forward = math::Rotate(math::Normalize(camera.rotation), {0.0f, 0.0f, 1.0f});
        target = eye + (math::LengthSquared(forward) > math::Epsilon ? forward : math::Vec3{0.0f, 0.0f, 1.0f}) * std::max(camera.orbitDistance, kMinClip);
        up = math::Rotate(math::Normalize(camera.rotation), {0.0f, 1.0f, 0.0f});
    }
    viewMatrix_ = math::LookAt(eye, target, up);
    projectionMatrix_ = camera.mode == RenderViewMode::Perspective
        ? math::Perspective(camera.verticalFieldOfView, aspect, camera.nearPlane, camera.farPlane)
        : math::Orthographic(-camera.orthographicSize * aspect * 0.5f, camera.orthographicSize * aspect * 0.5f,
            -camera.orthographicSize * 0.5f, camera.orthographicSize * 0.5f, camera.nearPlane, camera.farPlane);
    viewProjectionMatrix_ = math::Multiply(projectionMatrix_, viewMatrix_);
    if (!finite(viewMatrix_) || !finite(projectionMatrix_) || !finite(viewProjectionMatrix_) || !math::Inverse(viewProjectionMatrix_, inverseViewProjectionMatrix_)) {
        valid_ = false;
        description_ = {};
        viewMatrix_ = projectionMatrix_ = viewProjectionMatrix_ = inverseViewProjectionMatrix_ = math::Mat4::Identity();
        return false;
    }
    description_ = normalized;
    valid_ = true;
    return true;
}

bool RenderView::is_orthographic() const noexcept { return valid_ && description_.camera.mode != RenderViewMode::Perspective; }
GridPlane RenderView::grid_plane() const noexcept { return resolved_plane(description_); }

std::optional<Ndc> RenderView::world_to_ndc(World world) const noexcept {
    if (!valid_ || !finite(world.value)) return std::nullopt;
    const auto clip = math::TransformVector4(viewProjectionMatrix_, {world.value.x, world.value.y, world.value.z, 1.0f});
    if (!finite(clip) || std::abs(clip.w) <= math::Epsilon) return std::nullopt;
    return Ndc{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
}

std::optional<ViewportLocalPx> RenderView::world_to_viewport_local(World world) const noexcept {
    const auto ndc = world_to_ndc(world);
    return ndc ? ndc_to_viewport_local(*ndc, description_.viewport) : std::nullopt;
}

std::optional<World> RenderView::ndc_to_world(Ndc ndc) const noexcept {
    if (!valid_ || !finite(ndc.x) || !finite(ndc.y) || !finite(ndc.z)) return std::nullopt;
    const auto world = math::TransformVector4(inverseViewProjectionMatrix_, {ndc.x, ndc.y, ndc.z, 1.0f});
    if (!finite(world) || std::abs(world.w) <= math::Epsilon) return std::nullopt;
    return World{{world.x / world.w, world.y / world.w, world.z / world.w}};
}

std::optional<WorldRay> RenderView::viewport_local_to_world_ray(ViewportLocalPx point) const noexcept {
    if (!valid_ || !description_.viewport.contains(point)) return std::nullopt;
    auto nearNdc = viewport_local_to_ndc(point, description_.viewport);
    if (!nearNdc) return std::nullopt;
    auto farNdc = *nearNdc;
    nearNdc->z = 0.0f;
    farNdc.z = 1.0f;
    const auto nearWorld = ndc_to_world(*nearNdc);
    const auto farWorld = ndc_to_world(farNdc);
    if (!nearWorld || !farWorld) return std::nullopt;
    const auto direction = math::Normalize(farWorld->value - nearWorld->value);
    if (math::LengthSquared(direction) <= math::Epsilon) return std::nullopt;
    return WorldRay{*nearWorld, direction};
}

std::optional<World> RenderView::viewport_local_to_world_on_grid(ViewportLocalPx point) const noexcept {
    const auto ray = viewport_local_to_world_ray(point);
    if (!ray) return std::nullopt;
    const auto plane = grid_plane();
    const auto normal = plane_normal(plane);
    const auto origin = plane_point(plane, description_.camera.focalPoint, description_.grid.planeOffset);
    const float denominator = math::Dot(ray->direction, normal);
    if (std::abs(denominator) <= math::Epsilon) return std::nullopt;
    const float distance = math::Dot(origin - ray->origin.value, normal) / denominator;
    if (!finite(distance) || distance < 0.0f) return std::nullopt;
    return World{ray->origin.value + ray->direction * distance};
}

GridGeometry RenderView::build_grid_geometry() const {
    GridGeometry result;
    if (!valid_ || !description_.grid.enabled) return result;
    result.valid = true;
    result.plane = grid_plane();
    result.center = description_.camera.focalPoint;
    const float unitsPerPixel = is_orthographic() ? description_.camera.orthographicSize / static_cast<float>(description_.viewport.height) : description_.grid.perspectiveExtent / static_cast<float>(description_.viewport.height);
    result.majorStep = nice_step(std::max(kMinClip, unitsPerPixel * std::max(1.0f, description_.grid.targetMajorPixels)));
    const auto subdivisions = std::max<std::uint32_t>(1, description_.grid.minorSubdivisions);
    result.minorStep = result.majorStep / static_cast<float>(subdivisions);
    result.horizontalExtent = is_orthographic() ? description_.camera.orthographicSize * static_cast<float>(description_.viewport.width) / static_cast<float>(description_.viewport.height) * 0.5f : std::max(result.majorStep, description_.grid.perspectiveExtent);
    result.verticalExtent = is_orthographic() ? description_.camera.orthographicSize * 0.5f : std::max(result.majorStep, description_.grid.perspectiveExtent);
    const std::size_t maxLines = std::max<std::size_t>(4, description_.grid.maxLineCount);
    const std::size_t maxEachAxis = std::max<std::size_t>(2, maxLines / 2);
    const auto append_axis_family = [&](bool horizontal) {
        const float extent = horizontal ? result.horizontalExtent : result.verticalExtent;
        const std::size_t count = std::min<std::size_t>(maxEachAxis, static_cast<std::size_t>(std::ceil(extent / result.minorStep)) * 2u + 1u);
        const auto half = static_cast<std::int64_t>(count / 2u);
        for (std::int64_t index = -half; index <= half && result.lines.size() < maxLines; ++index) {
            const float position = static_cast<float>(index) * result.minorStep;
            const bool major = std::abs(std::fmod(std::abs(position), result.majorStep)) <= result.minorStep * 0.1f;
            RenderAxis axis = std::abs(position) <= result.minorStep * 0.1f ? (horizontal ? RenderAxis::X : RenderAxis::Z) : RenderAxis::None;
            const auto color = axis == RenderAxis::X ? description_.grid.xAxisColor : axis == RenderAxis::Z ? description_.grid.zAxisColor : major ? description_.grid.majorColor : description_.grid.minorColor;
            append_grid_line(result, result.plane, horizontal ? 0.0f : position, horizontal ? position : 0.0f, horizontal,
                axis == RenderAxis::None ? (major ? GridLineKind::Major : GridLineKind::Minor) : GridLineKind::Axis, axis, color,
                result.horizontalExtent, result.verticalExtent, result.center);
        }
    };
    append_axis_family(true);
    append_axis_family(false);
    if (result.plane == GridPlane::XY) {
        build_axis(result, result.plane, RenderAxis::X, description_.grid.xAxisColor, result.horizontalExtent, result.verticalExtent, result.center, true);
        build_axis(result, result.plane, RenderAxis::Y, description_.grid.yAxisColor, result.horizontalExtent, result.verticalExtent, result.center, false);
    } else if (result.plane == GridPlane::YZ) {
        build_axis(result, result.plane, RenderAxis::Y, description_.grid.yAxisColor, result.horizontalExtent, result.verticalExtent, result.center, true);
        build_axis(result, result.plane, RenderAxis::Z, description_.grid.zAxisColor, result.horizontalExtent, result.verticalExtent, result.center, false);
    } else {
        build_axis(result, result.plane, RenderAxis::X, description_.grid.xAxisColor, result.horizontalExtent, result.verticalExtent, result.center, true);
        build_axis(result, result.plane, RenderAxis::Z, description_.grid.zAxisColor, result.horizontalExtent, result.verticalExtent, result.center, false);
    }
    return result;
}

} // namespace shinkou::editor
