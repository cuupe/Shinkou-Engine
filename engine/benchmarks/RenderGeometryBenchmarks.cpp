#include "shinkou/render/RenderGeometry.h"
#include <chrono>
#include <cstddef>
#include <iostream>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
}

int main() {
    using shinkou::math::Vec3;
    using shinkou::render::LineGeometry;
    using shinkou::render::LineSegment;

    constexpr std::size_t count = 1u << 20;
    constexpr int repetitions = 8;
    std::vector<LineSegment> segments(count);
    for (std::size_t index = 0; index < count; ++index) {
        const float x = static_cast<float>(index & 4095u) * 0.25f;
        segments[index] = {{x, static_cast<float>(index >> 12), 0.0f},
                           {x + 1.0f, static_cast<float>(index >> 12), 0.5f}};
    }

    LineGeometry geometry;
    geometry.positions.reserve(count * 2u);
    geometry.indices.reserve(count * 2u);
    const auto start = Clock::now();
    for (int iteration = 0; iteration < repetitions; ++iteration) {
        geometry.clear();
        for (const auto& segment : segments) {
            if (!shinkou::render::append_line_segment(geometry, segment.start, segment.end)) return 1;
        }
    }
    const auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::uint64_t checksum = 0;
    for (const auto index : geometry.indices) checksum += index;
    const double generatedSegments = static_cast<double>(count) * repetitions;
    std::cout << "render geometry benchmark\n"
              << "  segments: " << count << "\n"
              << "  repetitions: " << repetitions << "\n"
              << "  append Msegments/s: " << generatedSegments / seconds / 1.0e6 << "\n"
              << "  checksum: " << checksum << "\n";
    return geometry.valid() && geometry.indices.size() == count * 2u ? 0 : 2;
}
