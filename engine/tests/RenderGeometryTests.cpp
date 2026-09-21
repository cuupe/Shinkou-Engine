#include "shinkou/render/RenderGeometry.h"
#include <cmath>
#include <iostream>
#include <vector>

int main() {
    using shinkou::math::Vec3;
    using shinkou::render::LineGeometry;
    using shinkou::render::LineSegment;

    LineGeometry geometry;
    if (!shinkou::render::append_line_segment(geometry, {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}) ||
        !geometry.valid() || geometry.positions.size() != 2 || geometry.indices.size() != 2 ||
        geometry.indices[0] != 0 || geometry.indices[1] != 1) {
        std::cerr << "basic line geometry generation failed\n";
        return 1;
    }

    const auto previous = geometry;
    if (shinkou::render::append_line_segment(geometry, {2.0f, 2.0f, 2.0f}, {2.0f, 2.0f, 2.0f}) ||
        geometry.positions.size() != previous.positions.size() || geometry.indices.size() != previous.indices.size()) {
        std::cerr << "degenerate line was accepted or mutated geometry\n";
        return 2;
    }

    const std::vector<LineSegment> segments{{{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
                                             {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}}};
    if (!shinkou::render::build_line_geometry(segments, geometry) || !geometry.valid() ||
        geometry.positions.size() != 4 || geometry.indices.size() != 4 || geometry.indices[2] != 2 ||
        geometry.indices[3] != 3) {
        std::cerr << "batched line geometry generation failed\n";
        return 3;
    }

    const auto beforeInvalidBuild = geometry;
    const std::vector<LineSegment> invalid{{{0.0f, 0.0f, 0.0f}, {NAN, 0.0f, 0.0f}}};
    if (shinkou::render::build_line_geometry(invalid, geometry) ||
        geometry.positions.size() != beforeInvalidBuild.positions.size() ||
        geometry.indices.size() != beforeInvalidBuild.indices.size()) {
        std::cerr << "invalid batch was accepted or mutated geometry\n";
        return 4;
    }
    return 0;
}
