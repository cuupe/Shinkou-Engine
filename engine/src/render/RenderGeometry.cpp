#include "shinkou/render/RenderGeometry.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace shinkou::render {
namespace {
bool finite(math::Vec3 value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

bool non_degenerate(math::Vec3 start, math::Vec3 end) noexcept {
    // Use double precision for the predicate so large world coordinates do
    // not overflow the squared-length calculation. The tolerance is relative
    // to the endpoint magnitude, with a one-meter floor near the origin.
    const double dx = static_cast<double>(end.x) - start.x;
    const double dy = static_cast<double>(end.y) - start.y;
    const double dz = static_cast<double>(end.z) - start.z;
    const double lengthSquared = dx * dx + dy * dy + dz * dz;
    const double scale = std::max({1.0, std::abs(static_cast<double>(start.x)),
        std::abs(static_cast<double>(start.y)), std::abs(static_cast<double>(start.z)),
        std::abs(static_cast<double>(end.x)), std::abs(static_cast<double>(end.y)),
        std::abs(static_cast<double>(end.z))});
    const double tolerance = static_cast<double>(math::Epsilon) * scale;
    return std::isfinite(lengthSquared) && lengthSquared > tolerance * tolerance;
}

bool can_append(const LineGeometry& geometry) noexcept {
    constexpr auto maxIndex = std::numeric_limits<std::uint32_t>::max();
    return geometry.positions.size() <= static_cast<std::size_t>(maxIndex) - 2u;
}
}

bool LineGeometry::valid() const noexcept {
    if (positions.empty() && indices.empty()) return true;
    if (positions.empty() || indices.empty() || (indices.size() % 2u) != 0u) return false;
    for (const auto& position : positions) {
        if (!finite(position)) return false;
    }
    for (const auto index : indices) {
        if (static_cast<std::size_t>(index) >= positions.size()) return false;
    }
    return true;
}

bool append_line_segment(LineGeometry& geometry, math::Vec3 start, math::Vec3 end) noexcept {
    if (!finite(start) || !finite(end) || !can_append(geometry)) return false;
    if (!non_degenerate(start, end)) return false;
    const auto base = static_cast<std::uint32_t>(geometry.positions.size());
    geometry.positions.push_back(start);
    geometry.positions.push_back(end);
    geometry.indices.push_back(base);
    geometry.indices.push_back(base + 1u);
    return true;
}

bool build_line_geometry(const std::vector<LineSegment>& segments, LineGeometry& geometry) {
    constexpr auto maxIndex = std::numeric_limits<std::uint32_t>::max();
    if (segments.size() > (static_cast<std::size_t>(maxIndex) + 1u) / 2u) return false;

    LineGeometry rebuilt;
    rebuilt.positions.reserve(segments.size() * 2u);
    rebuilt.indices.reserve(segments.size() * 2u);
    for (const auto& segment : segments) {
        if (!append_line_segment(rebuilt, segment.start, segment.end)) return false;
    }
    geometry = std::move(rebuilt);
    return true;
}
}
