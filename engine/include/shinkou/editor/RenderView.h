#pragma once

#include "shinkou/Math.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace shinkou::editor {

// Strongly named coordinate spaces prevent a physical window pixel from being
// accidentally passed to UIKit or to a viewport-local picking operation.
struct WindowClientPx {
    float x{0.0f};
    float y{0.0f};
};

struct UiLogicalPx {
    float x{0.0f};
    float y{0.0f};
};

struct ViewportLocalPx {
    float x{0.0f};
    float y{0.0f};
};

struct Ndc {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct WorldPoint {
    math::Vec3 value{};
};
using World = WorldPoint;

struct WindowClientRectPx {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};

    bool valid() const noexcept;
};

struct UiLogicalRect {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};

    bool valid() const noexcept;
};

// Render targets use integer physical pixels. x/y are client-area offsets and
// do not include the native Windows title bar or menu frame.
struct ViewportRectPx {
    std::int32_t x{0};
    std::int32_t y{0};
    std::int32_t width{0};
    std::int32_t height{0};

    bool valid() const noexcept;
    // The point is local to the viewport; its valid range is [0,width) x
    // [0,height), not the viewport's window-client origin.
    bool contains(ViewportLocalPx point) const noexcept;
};

using ScissorRectPx = ViewportRectPx;

std::optional<WindowClientPx> ui_logical_to_window_client_px(UiLogicalPx point, float dpiScale) noexcept;
std::optional<UiLogicalPx> window_client_to_ui_logical_px(WindowClientPx point, float dpiScale) noexcept;
std::optional<WindowClientRectPx> ui_logical_to_window_client_rect_px(UiLogicalRect rect, float dpiScale) noexcept;
std::optional<UiLogicalRect> window_client_to_ui_logical_rect_px(WindowClientRectPx rect, float dpiScale) noexcept;
std::optional<ViewportRectPx> ui_logical_to_viewport_rect_px(UiLogicalRect rect, float dpiScale) noexcept;

std::optional<ViewportLocalPx> window_client_to_viewport_local_px(
    WindowClientPx point, const ViewportRectPx& viewport) noexcept;
std::optional<WindowClientPx> viewport_local_to_window_client_px(
    ViewportLocalPx point, const ViewportRectPx& viewport) noexcept;
std::optional<Ndc> viewport_local_to_ndc(ViewportLocalPx point, const ViewportRectPx& viewport) noexcept;
std::optional<ViewportLocalPx> ndc_to_viewport_local(Ndc ndc, const ViewportRectPx& viewport) noexcept;

enum class RenderViewMode : std::uint8_t {
    Perspective,
    Front,
    Side,
    Top,
};

enum class RenderAxis : std::uint8_t {
    X,
    Y,
    Z,
    None,
};

enum class GridPlane : std::uint8_t {
    Auto,
    XY,
    YZ,
    XZ,
};

enum class GridLineKind : std::uint8_t {
    Minor,
    Major,
    Axis,
};

enum class GridLineOrientation : std::uint8_t {
    Horizontal,
    Vertical,
};

struct CameraState {
    RenderViewMode mode{RenderViewMode::Perspective};
    math::Vec3 position{0.0f, 0.0f, -5.0f};
    math::Quat rotation{};
    math::Vec3 focalPoint{};
    float orbitDistance{10.0f};
    float verticalFieldOfView{math::Pi / 3.0f};
    float orthographicSize{10.0f};
    float nearPlane{0.05f};
    float farPlane{1000.0f};
};

struct GridSettings {
    bool enabled{true};
    GridPlane plane{GridPlane::Auto};
    float planeOffset{0.0f};
    float targetMajorPixels{80.0f};
    std::uint32_t minorSubdivisions{5};
    float perspectiveExtent{100.0f};
    std::uint32_t maxLineCount{256};
    math::Vec4 minorColor{0.20f, 0.22f, 0.26f, 1.0f};
    math::Vec4 majorColor{0.32f, 0.35f, 0.40f, 1.0f};
    math::Vec4 xAxisColor{0.86f, 0.24f, 0.24f, 1.0f};
    math::Vec4 yAxisColor{0.30f, 0.78f, 0.40f, 1.0f};
    math::Vec4 zAxisColor{0.30f, 0.52f, 0.94f, 1.0f};
};

struct GridLine {
    GridLineOrientation orientation{GridLineOrientation::Horizontal};
    GridLineKind kind{GridLineKind::Minor};
    RenderAxis axis{RenderAxis::None};
    math::Vec3 start{};
    math::Vec3 end{};
    math::Vec4 color{};
};

struct GridAxisLine {
    bool visible{false};
    RenderAxis axis{RenderAxis::None};
    math::Vec3 start{};
    math::Vec3 end{};
    math::Vec4 color{};
};

struct GridGeometry {
    bool valid{false};
    GridPlane plane{GridPlane::XY};
    math::Vec3 center{};
    float minorStep{0.0f};
    float majorStep{0.0f};
    float horizontalExtent{0.0f};
    float verticalExtent{0.0f};
    std::vector<GridLine> lines;
    std::array<GridAxisLine, 3> axes{};
};

struct RenderViewDescription {
    ViewportRectPx viewport{};
    ScissorRectPx scissor{};
    float dpiScale{1.0f};
    CameraState camera{};
    GridSettings grid{};
};

struct WorldRay {
    World origin{};
    math::Vec3 direction{0.0f, 0.0f, 1.0f};
};

class RenderView {
public:
    RenderView() noexcept = default;

    // Transactional: an invalid description leaves finite identity matrices
    // and no renderable rectangle.
    bool configure(const RenderViewDescription& description) noexcept;

    bool valid() const noexcept { return valid_; }
    bool is_orthographic() const noexcept;
    const RenderViewDescription& description() const noexcept { return description_; }
    const ViewportRectPx& viewport() const noexcept { return description_.viewport; }
    const ScissorRectPx& scissor() const noexcept { return description_.scissor; }
    const CameraState& camera() const noexcept { return description_.camera; }
    GridPlane grid_plane() const noexcept;
    const math::Mat4& view_matrix() const noexcept { return viewMatrix_; }
    const math::Mat4& projection_matrix() const noexcept { return projectionMatrix_; }
    const math::Mat4& view_projection_matrix() const noexcept { return viewProjectionMatrix_; }

    std::optional<Ndc> world_to_ndc(World world) const noexcept;
    std::optional<ViewportLocalPx> world_to_viewport_local(World world) const noexcept;
    std::optional<World> ndc_to_world(Ndc ndc) const noexcept;
    std::optional<WorldRay> viewport_local_to_world_ray(ViewportLocalPx point) const noexcept;
    std::optional<World> viewport_local_to_world_on_grid(ViewportLocalPx point) const noexcept;

    // CPU-only geometry description for a renderer/debug overlay. It is
    // deterministic and bounded by GridSettings::maxLineCount.
    GridGeometry build_grid_geometry() const;

private:
    RenderViewDescription description_{};
    math::Mat4 viewMatrix_{math::Mat4::Identity()};
    math::Mat4 projectionMatrix_{math::Mat4::Identity()};
    math::Mat4 viewProjectionMatrix_{math::Mat4::Identity()};
    math::Mat4 inverseViewProjectionMatrix_{math::Mat4::Identity()};
    bool valid_{false};
};

} // namespace shinkou::editor
