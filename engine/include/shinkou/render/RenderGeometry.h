#pragma once

#include "shinkou/Math.h"
#include <cstdint>
#include <vector>

namespace shinkou::render {

// Backend-neutral line primitive. It deliberately contains only geometric
// data; material, color and pipeline state belong to the render pass.
struct LineSegment {
    math::Vec3 start{};
    math::Vec3 end{};
};

// Indexed line-list geometry suitable for a MeshDraw with a line topology.
// The positions are tightly packed Vec3 values, so callers can upload
// positions.data() directly to a vertex buffer with stride sizeof(Vec3).
struct LineGeometry {
    std::vector<math::Vec3> positions;
    std::vector<std::uint32_t> indices;

    void clear() noexcept {
        positions.clear();
        indices.clear();
    }

    bool empty() const noexcept {
        return positions.empty() || indices.empty();
    }

    bool valid() const noexcept;
};

// Appends one non-degenerate, finite line segment without touching the
// destination when validation fails. Returns false for invalid input or when
// the 32-bit index space would overflow.
bool append_line_segment(LineGeometry& geometry, math::Vec3 start, math::Vec3 end) noexcept;

// Rebuilds geometry as an indexed line list. This is transactional: an
// invalid segment leaves the caller's existing geometry unchanged.
bool build_line_geometry(const std::vector<LineSegment>& segments, LineGeometry& geometry);

}
