#include "shinkou/editor/RenderView.h"

#include <cassert>
#include <cmath>

using namespace shinkou::editor;

int main() {
    const ViewportRectPx viewport{100, 50, 800, 400};
    assert(viewport.valid());
    assert(viewport.contains({0.0f, 0.0f}));
    assert(!viewport.contains({800.0f, 0.0f}));
    const auto local = window_client_to_viewport_local_px({100.0f, 50.0f}, viewport);
    assert(local && local->x == 0.0f && local->y == 0.0f);
    const auto ndc = viewport_local_to_ndc({400.0f, 200.0f}, viewport);
    assert(ndc && std::abs(ndc->x) < 0.001f && std::abs(ndc->y) < 0.001f);
    const auto roundTrip = ndc_to_viewport_local(*ndc, viewport);
    assert(roundTrip && std::abs(roundTrip->x - 400.0f) < 0.001f && std::abs(roundTrip->y - 200.0f) < 0.001f);

    RenderView view;
    RenderViewDescription description;
    description.viewport = viewport;
    description.scissor = viewport;
    description.camera.position = {0.0f, 0.0f, -5.0f};
    description.camera.focalPoint = {0.0f, 0.0f, 0.0f};
    assert(view.configure(description));
    const auto center = view.world_to_viewport_local(World{{0.0f, 0.0f, 0.0f}});
    assert(center && std::abs(center->x - 400.0f) < 0.01f && std::abs(center->y - 200.0f) < 0.01f);
    const auto ray = view.viewport_local_to_world_ray({400.0f, 200.0f});
    assert(ray && ray->direction.z > 0.9f);
    const auto grid = view.build_grid_geometry();
    assert(grid.valid && !grid.lines.empty() && grid.lines.size() <= description.grid.maxLineCount);

    description.camera.mode = RenderViewMode::Top;
    description.camera.orthographicSize = 20.0f;
    assert(view.configure(description));
    assert(view.is_orthographic());
    assert(view.grid_plane() == GridPlane::XZ);
    assert(view.viewport_local_to_world_on_grid({400.0f, 200.0f}));

    description.viewport.width = 0;
    assert(!view.configure(description));
    return 0;
}
