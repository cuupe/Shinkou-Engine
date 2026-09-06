#pragma once

#include "shinkou/MathGeometry.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace shinkou::math {

struct Segment {
    Vec3 a{};
    Vec3 b{};
};

struct Triangle {
    Vec3 a{};
    Vec3 b{};
    Vec3 c{};
};

struct Capsule {
    Segment segment{};
    float radius{0.0f};
};

inline bool IsValidCapsule(const Capsule& capsule) noexcept;

struct SegmentClosestPoints {
    Vec3 first{};
    Vec3 second{};
    float firstParameter{0.0f};
    float secondParameter{0.0f};
};

inline Vec3 ClosestPoint(Segment segment, Vec3 point) noexcept {
    const Vec3 edge = segment.b - segment.a;
    const float lengthSquared = LengthSquared(edge);
    if (!IsFinite(lengthSquared) || lengthSquared <= Epsilon) return segment.a;
    const float t = std::clamp(Dot(point - segment.a, edge) / lengthSquared, 0.0f, 1.0f);
    return segment.a + edge * t;
}

inline float DistanceSquared(Segment segment, Vec3 point) noexcept {
    return LengthSquared(ClosestPoint(segment, point) - point);
}

inline SegmentClosestPoints ClosestPoints(Segment first, Segment second) noexcept {
    const Vec3 directionA = first.b - first.a;
    const Vec3 directionB = second.b - second.a;
    const Vec3 offset = first.a - second.a;
    const float aa = Dot(directionA, directionA);
    const float ab = Dot(directionA, directionB);
    const float bb = Dot(directionB, directionB);
    const float ae = Dot(directionA, offset);
    const float be = Dot(directionB, offset);
    SegmentClosestPoints result{};
    const float denominator = aa * bb - ab * ab;
    if (aa <= Epsilon && bb <= Epsilon) return {first.a, second.a, 0.0f, 0.0f};
    if (aa <= Epsilon) {
        result.secondParameter = bb > Epsilon ? std::clamp(be / bb, 0.0f, 1.0f) : 0.0f;
    } else if (bb <= Epsilon) {
        result.firstParameter = std::clamp(-ae / aa, 0.0f, 1.0f);
    } else if (std::abs(denominator) > Epsilon) {
        result.firstParameter = std::clamp((ab * be - ae * bb) / denominator, 0.0f, 1.0f);
        result.secondParameter = (ab * result.firstParameter + be) / bb;
        if (result.secondParameter < 0.0f) {
            result.secondParameter = 0.0f;
            result.firstParameter = std::clamp(-ae / aa, 0.0f, 1.0f);
        } else if (result.secondParameter > 1.0f) {
            result.secondParameter = 1.0f;
            result.firstParameter = std::clamp((ab - ae) / aa, 0.0f, 1.0f);
        }
    } else {
        result.firstParameter = 0.0f;
        result.secondParameter = std::clamp(be / bb, 0.0f, 1.0f);
    }
    result.first = first.a + directionA * result.firstParameter;
    result.second = second.a + directionB * result.secondParameter;
    return result;
}

inline float DistanceSquared(Segment first, Segment second) noexcept {
    const SegmentClosestPoints points = ClosestPoints(first, second);
    return LengthSquared(points.first - points.second);
}

inline Vec3 ClosestPoint(Triangle triangle, Vec3 point) noexcept {
    const Vec3 ab = triangle.b - triangle.a;
    const Vec3 ac = triangle.c - triangle.a;
    if (LengthSquared(Cross(ab, ac)) <= Epsilon * Epsilon) {
        const Vec3 abPoint = ClosestPoint(Segment{triangle.a, triangle.b}, point);
        const Vec3 acPoint = ClosestPoint(Segment{triangle.a, triangle.c}, point);
        const Vec3 bcPoint = ClosestPoint(Segment{triangle.b, triangle.c}, point);
        const float abDistance = LengthSquared(abPoint - point);
        const float acDistance = LengthSquared(acPoint - point);
        const float bcDistance = LengthSquared(bcPoint - point);
        return abDistance <= acDistance && abDistance <= bcDistance ? abPoint : (acDistance <= bcDistance ? acPoint : bcPoint);
    }
    const Vec3 ap = point - triangle.a;
    const float d1 = Dot(ab, ap);
    const float d2 = Dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return triangle.a;

    const Vec3 bp = point - triangle.b;
    const float d3 = Dot(ab, bp);
    const float d4 = Dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return triangle.b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        return triangle.a + ab * v;
    }

    const Vec3 cp = point - triangle.c;
    const float d5 = Dot(ab, cp);
    const float d6 = Dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return triangle.c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        return triangle.a + ac * w;
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const Vec3 edge = triangle.c - triangle.b;
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return triangle.b + edge * w;
    }

    const float denominator = 1.0f / (va + vb + vc);
    const float v = vb * denominator;
    const float w = vc * denominator;
    return triangle.a + ab * v + ac * w;
}

inline float DistanceSquared(Triangle triangle, Vec3 point) noexcept {
    return LengthSquared(ClosestPoint(triangle, point) - point);
}

inline bool Intersects(Triangle triangle, Sphere sphere) noexcept {
    if (!IsFinite(sphere.radius) || sphere.radius < 0.0f) return false;
    return DistanceSquared(triangle, sphere.center) <= sphere.radius * sphere.radius;
}

inline bool Intersects(const Capsule& first, const Capsule& second) noexcept {
    if (!IsValidCapsule(first) || !IsValidCapsule(second)) return false;
    const float radius = first.radius + second.radius;
    return DistanceSquared(first.segment, second.segment) <= radius * radius;
}

inline bool Intersects(const Capsule& capsule, Sphere sphere) noexcept {
    if (!IsValidCapsule(capsule) || !IsFinite(sphere.radius) || sphere.radius < 0.0f) return false;
    const float radius = capsule.radius + sphere.radius;
    return DistanceSquared(capsule.segment, sphere.center) <= radius * radius;
}

inline bool Intersects(Sphere sphere, const Capsule& capsule) noexcept { return Intersects(capsule, sphere); }

inline bool RaycastTriangle(Ray ray, Triangle triangle, float& distance,
                            Vec3* point = nullptr, Vec3* normal = nullptr,
                            bool cullBackFace = false) noexcept {
    const Vec3 edge1 = triangle.b - triangle.a;
    const Vec3 edge2 = triangle.c - triangle.a;
    const Vec3 p = Cross(ray.direction, edge2);
    const float determinant = Dot(edge1, p);
    if (!IsFinite(determinant) || (cullBackFace ? determinant <= Epsilon : std::abs(determinant) <= Epsilon)) return false;
    const float inverseDeterminant = 1.0f / determinant;
    const Vec3 t = ray.origin - triangle.a;
    const float u = Dot(t, p) * inverseDeterminant;
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 q = Cross(t, edge1);
    const float v = Dot(ray.direction, q) * inverseDeterminant;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float hit = Dot(edge2, q) * inverseDeterminant;
    if (!IsFinite(hit) || hit < 0.0f) return false;
    distance = hit;
    if (point) *point = ray.origin + ray.direction * hit;
    if (normal) *normal = Normalize(Cross(edge1, edge2));
    return true;
}

inline bool IsValidCapsule(const Capsule& capsule) noexcept {
    return IsFinite(capsule.segment.a.x) && IsFinite(capsule.segment.a.y) && IsFinite(capsule.segment.a.z) &&
           IsFinite(capsule.segment.b.x) && IsFinite(capsule.segment.b.y) && IsFinite(capsule.segment.b.z) &&
           IsFinite(capsule.radius) && capsule.radius >= 0.0f;
}

inline Aabb Bounds(Segment segment, float radius = 0.0f) noexcept {
    const Vec3 padding{std::abs(radius), std::abs(radius), std::abs(radius)};
    const Vec3 minimum{std::min(segment.a.x, segment.b.x), std::min(segment.a.y, segment.b.y), std::min(segment.a.z, segment.b.z)};
    const Vec3 maximum{std::max(segment.a.x, segment.b.x), std::max(segment.a.y, segment.b.y), std::max(segment.a.z, segment.b.z)};
    return {minimum - padding, maximum + padding};
}

inline Aabb Bounds(Triangle triangle) noexcept {
    return {{std::min({triangle.a.x, triangle.b.x, triangle.c.x}), std::min({triangle.a.y, triangle.b.y, triangle.c.y}), std::min({triangle.a.z, triangle.b.z, triangle.c.z})},
            {std::max({triangle.a.x, triangle.b.x, triangle.c.x}), std::max({triangle.a.y, triangle.b.y, triangle.c.y}), std::max({triangle.a.z, triangle.b.z, triangle.c.z})}};
}

struct BvhPrimitive {
    Aabb bounds{};
    std::uint32_t id{0};
};

// A compact static BVH for culling, picking, and broad spatial queries. It
// stores user IDs only; narrow-phase intersection remains the caller's job.
class StaticBvh {
    struct Node {
        Aabb bounds{};
        std::uint32_t first{0};
        std::uint32_t count{0};
        std::int32_t left{-1};
        std::int32_t right{-1};

        bool leaf() const noexcept { return left < 0; }
    };

    std::vector<BvhPrimitive> primitives_;
    std::vector<Node> nodes_;
    std::int32_t root_{-1};
    std::uint32_t leafSize_{4};

    std::int32_t build_node(std::size_t begin, std::size_t end) {
        Node node{};
        node.first = static_cast<std::uint32_t>(begin);
        node.count = static_cast<std::uint32_t>(end - begin);
        node.bounds = primitives_[begin].bounds;
        Vec3 centroidMin = Center(primitives_[begin].bounds);
        Vec3 centroidMax = centroidMin;
        for (std::size_t index = begin + 1; index < end; ++index) {
            node.bounds = Merge(node.bounds, primitives_[index].bounds);
            const Vec3 center = Center(primitives_[index].bounds);
            centroidMin = {std::min(centroidMin.x, center.x), std::min(centroidMin.y, center.y), std::min(centroidMin.z, center.z)};
            centroidMax = {std::max(centroidMax.x, center.x), std::max(centroidMax.y, center.y), std::max(centroidMax.z, center.z)};
        }
        const std::int32_t nodeIndex = static_cast<std::int32_t>(nodes_.size());
        nodes_.push_back(node);
        if (end - begin <= leafSize_) return nodeIndex;

        const Vec3 centroidExtent = centroidMax - centroidMin;
        int axis = 0;
        if (centroidExtent.y > centroidExtent.x) axis = 1;
        if (centroidExtent.z > centroidExtent[axis]) axis = 2;
        const std::size_t middle = begin + (end - begin) / 2;
        std::nth_element(primitives_.begin() + static_cast<std::ptrdiff_t>(begin),
                         primitives_.begin() + static_cast<std::ptrdiff_t>(middle),
                         primitives_.begin() + static_cast<std::ptrdiff_t>(end),
                         [axis](const BvhPrimitive& lhs, const BvhPrimitive& rhs) {
                             return Center(lhs.bounds)[axis] < Center(rhs.bounds)[axis];
                         });
        const std::int32_t left = build_node(begin, middle);
        const std::int32_t right = build_node(middle, end);
        nodes_[static_cast<std::size_t>(nodeIndex)].left = left;
        nodes_[static_cast<std::size_t>(nodeIndex)].right = right;
        nodes_[static_cast<std::size_t>(nodeIndex)].count = 0;
        return nodeIndex;
    }

    template<class Callback>
    void query_node(std::int32_t nodeIndex, Aabb bounds, Callback& callback) const {
        const Node& node = nodes_[static_cast<std::size_t>(nodeIndex)];
        if (!Overlaps(node.bounds, bounds)) return;
        if (node.leaf()) {
            for (std::uint32_t offset = 0; offset < node.count; ++offset) callback(primitives_[node.first + offset].id);
            return;
        }
        query_node(node.left, bounds, callback);
        query_node(node.right, bounds, callback);
    }

    template<class Callback>
    void query_ray_node(std::int32_t nodeIndex, const Ray& ray, float maxDistance, Callback& callback) const {
        const Node& node = nodes_[static_cast<std::size_t>(nodeIndex)];
        float nodeDistance = 0.0f;
        if (!RaycastAabb(ray, node.bounds, nodeDistance) || nodeDistance > maxDistance) return;
        if (node.leaf()) {
            for (std::uint32_t offset = 0; offset < node.count; ++offset) {
                float primitiveDistance = 0.0f;
                if (RaycastAabb(ray, primitives_[node.first + offset].bounds, primitiveDistance) && primitiveDistance <= maxDistance)
                    callback(primitives_[node.first + offset].id, primitiveDistance);
            }
            return;
        }
        query_ray_node(node.left, ray, maxDistance, callback);
        query_ray_node(node.right, ray, maxDistance, callback);
    }

public:
    void clear() noexcept {
        primitives_.clear();
        nodes_.clear();
        root_ = -1;
    }

    void build(ConstArrayView<BvhPrimitive> primitives, std::uint32_t leafSize = 4) {
        clear();
        leafSize_ = std::clamp<std::uint32_t>(leafSize, 1, 32);
        primitives_.reserve(primitives.size());
        for (std::size_t index = 0; index < primitives.size(); ++index)
            if (IsValidAabb(primitives[index].bounds)) primitives_.push_back(primitives[index]);
        if (primitives_.empty()) return;
        nodes_.reserve(primitives_.size() * 2);
        root_ = build_node(0, primitives_.size());
    }

    bool empty() const noexcept { return root_ < 0; }
    std::size_t primitive_count() const noexcept { return primitives_.size(); }

    template<class Callback>
    void query(Aabb bounds, Callback&& callback) const {
        if (root_ < 0 || !IsValidAabb(bounds)) return;
        auto&& callable = callback;
        query_node(root_, bounds, callable);
    }

    template<class Callback>
    void query_ray(Ray ray, float maxDistance, Callback&& callback) const {
        if (root_ < 0 || !IsFinite(maxDistance) || maxDistance < 0.0f) return;
        auto&& callable = callback;
        query_ray_node(root_, ray, maxDistance, callable);
    }
};

}
