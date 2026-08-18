#pragma once

#include "shinkou/Math.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace shinkou::math {

struct Ray {
    Vec3 origin{};
    Vec3 direction{0.0f, 0.0f, 1.0f};
};

struct Plane {
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float distance{0.0f};

    static Plane FromPointNormal(Vec3 point, Vec3 normal) noexcept {
        const Vec3 unit = Normalize(normal);
        return {unit, -Dot(unit, point)};
    }
    float signed_distance(Vec3 point) const noexcept { return Dot(normal, point) + distance; }
    Plane normalized() const noexcept {
        const float length = Length(normal);
        return length > Epsilon ? Plane{normal / length, distance / length} : Plane{};
    }
};

struct Sphere {
    Vec3 center{};
    float radius{0.0f};
};

struct Obb {
    // halfExtents are expressed in local space; transform.scale is applied once
    // when converting to world-space extents.
    Transform transform{};
    Vec3 halfExtents{0.5f, 0.5f, 0.5f};
};

inline bool IsValidAabb(Aabb bounds) noexcept {
    return IsFinite(bounds.min.x) && IsFinite(bounds.min.y) && IsFinite(bounds.min.z) &&
           IsFinite(bounds.max.x) && IsFinite(bounds.max.y) && IsFinite(bounds.max.z) &&
           bounds.min.x <= bounds.max.x && bounds.min.y <= bounds.max.y && bounds.min.z <= bounds.max.z;
}
inline bool IsValidObb(const Obb& box) noexcept {
    return IsFinite(box.transform.position.x) && IsFinite(box.transform.position.y) && IsFinite(box.transform.position.z) &&
           IsFinite(box.transform.rotation.x) && IsFinite(box.transform.rotation.y) && IsFinite(box.transform.rotation.z) && IsFinite(box.transform.rotation.w) &&
           IsFinite(box.transform.scale.x) && IsFinite(box.transform.scale.y) && IsFinite(box.transform.scale.z) &&
           IsFinite(box.halfExtents.x) && IsFinite(box.halfExtents.y) && IsFinite(box.halfExtents.z) &&
           box.halfExtents.x >= 0.0f && box.halfExtents.y >= 0.0f && box.halfExtents.z >= 0.0f;
}

inline Vec3 Center(Aabb bounds) noexcept { return (bounds.min + bounds.max) * 0.5f; }
inline Vec3 Extents(Aabb bounds) noexcept { return (bounds.max - bounds.min) * 0.5f; }
inline Aabb MakeAabb(Vec3 center, Vec3 halfExtents) noexcept { return {center - halfExtents, center + halfExtents}; }
inline Aabb Merge(Aabb lhs, Aabb rhs) noexcept {
    return {{std::min(lhs.min.x, rhs.min.x), std::min(lhs.min.y, rhs.min.y), std::min(lhs.min.z, rhs.min.z)},
            {std::max(lhs.max.x, rhs.max.x), std::max(lhs.max.y, rhs.max.y), std::max(lhs.max.z, rhs.max.z)}};
}
inline Vec3 ClosestPoint(Aabb bounds, Vec3 point) noexcept {
    return {std::clamp(point.x, bounds.min.x, bounds.max.x), std::clamp(point.y, bounds.min.y, bounds.max.y),
            std::clamp(point.z, bounds.min.z, bounds.max.z)};
}
inline Vec3 ClosestPoint(Plane plane, Vec3 point) noexcept {
    return point - plane.normal * plane.signed_distance(point);
}

inline Vec3 ObbCenter(const Obb& box) noexcept { return box.transform.position; }
inline Vec3 ObbHalfExtents(const Obb& box) noexcept {
    return {std::abs(box.halfExtents.x * box.transform.scale.x),
            std::abs(box.halfExtents.y * box.transform.scale.y),
            std::abs(box.halfExtents.z * box.transform.scale.z)};
}
inline std::array<Vec3, 3> ObbAxes(const Obb& box) noexcept {
    const Quat rotation = Normalize(box.transform.rotation);
    return {Rotate(rotation, {1.0f, 0.0f, 0.0f}),
            Rotate(rotation, {0.0f, 1.0f, 0.0f}),
            Rotate(rotation, {0.0f, 0.0f, 1.0f})};
}
inline Vec3 ClosestPoint(const Obb& box, Vec3 point) noexcept {
    const Vec3 center = ObbCenter(box);
    const Vec3 extents = ObbHalfExtents(box);
    const auto axes = ObbAxes(box);
    Vec3 result = center;
    const Vec3 offset = point - center;
    const float coordinates[3] = {extents.x, extents.y, extents.z};
    for (int axis = 0; axis < 3; ++axis) result += axes[axis] * std::clamp(Dot(offset, axes[axis]), -coordinates[axis], coordinates[axis]);
    return result;
}
inline bool Contains(const Obb& box, Vec3 point) noexcept {
    const Vec3 offset = point - ObbCenter(box);
    const Vec3 extents = ObbHalfExtents(box);
    const auto axes = ObbAxes(box);
    return std::abs(Dot(offset, axes[0])) <= extents.x + Epsilon &&
           std::abs(Dot(offset, axes[1])) <= extents.y + Epsilon &&
           std::abs(Dot(offset, axes[2])) <= extents.z + Epsilon;
}

inline bool Intersects(const Obb& lhs, const Obb& rhs) noexcept {
    if (!IsValidObb(lhs) || !IsValidObb(rhs)) return false;
    const auto lhsAxes = ObbAxes(lhs);
    const auto rhsAxes = ObbAxes(rhs);
    const Vec3 lhsExtents = ObbHalfExtents(lhs);
    const Vec3 rhsExtents = ObbHalfExtents(rhs);
    const float a[3] = {lhsExtents.x, lhsExtents.y, lhsExtents.z};
    const float b[3] = {rhsExtents.x, rhsExtents.y, rhsExtents.z};
    float rotation[3][3]{};
    float absoluteRotation[3][3]{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        rotation[i][j] = Dot(lhsAxes[i], rhsAxes[j]);
        absoluteRotation[i][j] = std::abs(rotation[i][j]) + Epsilon;
    }
    const Vec3 delta = ObbCenter(rhs) - ObbCenter(lhs);
    const float translated[3] = {Dot(delta, lhsAxes[0]), Dot(delta, lhsAxes[1]), Dot(delta, lhsAxes[2])};

    for (int i = 0; i < 3; ++i) {
        const float radius = a[i] + b[0] * absoluteRotation[i][0] + b[1] * absoluteRotation[i][1] + b[2] * absoluteRotation[i][2];
        if (std::abs(translated[i]) > radius) return false;
    }
    for (int j = 0; j < 3; ++j) {
        const float distance = std::abs(translated[0] * rotation[0][j] + translated[1] * rotation[1][j] + translated[2] * rotation[2][j]);
        const float radius = a[0] * absoluteRotation[0][j] + a[1] * absoluteRotation[1][j] + a[2] * absoluteRotation[2][j] + b[j];
        if (distance > radius) return false;
    }
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        const int i1 = (i + 1) % 3, i2 = (i + 2) % 3;
        const int j1 = (j + 1) % 3, j2 = (j + 2) % 3;
        const float distance = std::abs(translated[i2] * rotation[i1][j] - translated[i1] * rotation[i2][j]);
        const float radius = a[i1] * absoluteRotation[i2][j] + a[i2] * absoluteRotation[i1][j] +
                             b[j1] * absoluteRotation[i][j2] + b[j2] * absoluteRotation[i][j1];
        if (distance > radius) return false;
    }
    return true;
}

inline bool Intersects(const Obb& box, const Sphere& sphere) noexcept {
    if (!IsValidObb(box) || !IsFinite(sphere.center.x) || !IsFinite(sphere.center.y) || !IsFinite(sphere.center.z) || !IsFinite(sphere.radius)) return false;
    const float radius = std::abs(sphere.radius);
    return LengthSquared(ClosestPoint(box, sphere.center) - sphere.center) <= radius * radius;
}
inline bool Intersects(const Sphere& sphere, const Obb& box) noexcept { return Intersects(box, sphere); }
inline bool Intersects(const Aabb& bounds, const Obb& box) noexcept {
    if (!IsValidAabb(bounds) || !IsValidObb(box)) return false;
    const Obb aabb{{Center(bounds), Quat::Identity(), {1.0f, 1.0f, 1.0f}}, Extents(bounds)};
    return Intersects(aabb, box);
}
inline bool Intersects(const Obb& box, const Aabb& bounds) noexcept { return Intersects(bounds, box); }

inline bool RaycastAabb(Ray ray, Aabb bounds, float& distance, Vec3* point, Vec3* normal) noexcept;

inline bool RaycastObb(const Ray& ray, const Obb& box, float& distance,
                       Vec3* point = nullptr, Vec3* normal = nullptr) noexcept {
    if (!IsValidObb(box) || !IsFinite(ray.origin.x) || !IsFinite(ray.origin.y) || !IsFinite(ray.origin.z) ||
        !IsFinite(ray.direction.x) || !IsFinite(ray.direction.y) || !IsFinite(ray.direction.z)) return false;
    const Quat rotation = Normalize(box.transform.rotation);
    const Quat inverseRotation = Conjugate(rotation);
    const Vec3 center = ObbCenter(box);
    const Vec3 localOrigin = Rotate(inverseRotation, ray.origin - center);
    const Vec3 localDirection = Rotate(inverseRotation, ray.direction);
    const Vec3 extents = ObbHalfExtents(box);
    const Aabb localBounds{{-extents.x, -extents.y, -extents.z}, {extents.x, extents.y, extents.z}};
    float localDistance = 0.0f;
    Vec3 localPoint{}, localNormal{};
    if (!RaycastAabb({localOrigin, localDirection}, localBounds, localDistance, &localPoint, &localNormal)) return false;
    distance = localDistance;
    if (point) *point = center + Rotate(rotation, localPoint);
    if (normal) *normal = Normalize(Rotate(rotation, localNormal));
    return true;
}

inline Aabb TransformBounds(Aabb bounds, const Mat4& matrix) noexcept {
    const std::array<Vec3, 8> corners{{
        {bounds.min.x, bounds.min.y, bounds.min.z}, {bounds.max.x, bounds.min.y, bounds.min.z},
        {bounds.min.x, bounds.max.y, bounds.min.z}, {bounds.max.x, bounds.max.y, bounds.min.z},
        {bounds.min.x, bounds.min.y, bounds.max.z}, {bounds.max.x, bounds.min.y, bounds.max.z},
        {bounds.min.x, bounds.max.y, bounds.max.z}, {bounds.max.x, bounds.max.y, bounds.max.z}}};
    Aabb result{TransformPoint(matrix, corners[0]), TransformPoint(matrix, corners[0])};
    for (std::size_t index = 1; index < corners.size(); ++index) {
        const Vec3 point = TransformPoint(matrix, corners[index]);
        result.min = {std::min(result.min.x, point.x), std::min(result.min.y, point.y), std::min(result.min.z, point.z)};
        result.max = {std::max(result.max.x, point.x), std::max(result.max.y, point.y), std::max(result.max.z, point.z)};
    }
    return result;
}

inline bool Intersects(Aabb bounds, Sphere sphere) noexcept {
    const Vec3 closest = ClosestPoint(bounds, sphere.center);
    return LengthSquared(closest - sphere.center) <= sphere.radius * sphere.radius;
}
inline bool Contains(Aabb bounds, Vec3 point) noexcept {
    return point.x >= bounds.min.x && point.x <= bounds.max.x && point.y >= bounds.min.y && point.y <= bounds.max.y &&
           point.z >= bounds.min.z && point.z <= bounds.max.z;
}
inline bool Intersects(Sphere lhs, Sphere rhs) noexcept {
    const float radius = lhs.radius + rhs.radius;
    return LengthSquared(lhs.center - rhs.center) <= radius * radius;
}

inline bool RaycastPlane(Ray ray, Plane plane, float& distance, Vec3* point = nullptr) noexcept {
    if (!IsFinite(ray.origin.x) || !IsFinite(ray.origin.y) || !IsFinite(ray.origin.z) ||
        !IsFinite(ray.direction.x) || !IsFinite(ray.direction.y) || !IsFinite(ray.direction.z) ||
        !IsFinite(plane.normal.x) || !IsFinite(plane.normal.y) || !IsFinite(plane.normal.z) || !IsFinite(plane.distance)) return false;
    const float denominator = Dot(plane.normal, ray.direction);
    if (std::abs(denominator) <= Epsilon) return false;
    const float hit = -(Dot(plane.normal, ray.origin) + plane.distance) / denominator;
    if (hit < 0.0f) return false;
    distance = hit;
    if (point) *point = ray.origin + ray.direction * hit;
    return true;
}

inline bool RaycastSphere(Ray ray, Sphere sphere, float& distance, Vec3* point = nullptr) noexcept {
    if (!IsFinite(ray.origin.x) || !IsFinite(ray.origin.y) || !IsFinite(ray.origin.z) ||
        !IsFinite(ray.direction.x) || !IsFinite(ray.direction.y) || !IsFinite(ray.direction.z) ||
        !IsFinite(sphere.center.x) || !IsFinite(sphere.center.y) || !IsFinite(sphere.center.z) || !IsFinite(sphere.radius)) return false;
    const Vec3 offset = ray.origin - sphere.center;
    const float directionLengthSquared = LengthSquared(ray.direction);
    if (directionLengthSquared <= Epsilon) return false;
    const float projection = -Dot(offset, ray.direction) / directionLengthSquared;
    const Vec3 closest = ray.origin + ray.direction * projection;
    const float radiusSquared = sphere.radius * sphere.radius;
    const float closestDistanceSquared = LengthSquared(closest - sphere.center);
    if (closestDistanceSquared > radiusSquared) return false;
    const float offsetDistance = std::sqrt(std::max(radiusSquared - closestDistanceSquared, 0.0f) / directionLengthSquared);
    const float hit = projection - offsetDistance >= 0.0f ? projection - offsetDistance : projection + offsetDistance;
    if (hit < 0.0f) return false;
    distance = hit;
    if (point) *point = ray.origin + ray.direction * hit;
    return true;
}

inline bool RaycastAabb(Ray ray, Aabb bounds, float& distance, Vec3* point = nullptr, Vec3* normal = nullptr) noexcept {
    if (!IsValidAabb(bounds) || !IsFinite(ray.origin.x) || !IsFinite(ray.origin.y) || !IsFinite(ray.origin.z) ||
        !IsFinite(ray.direction.x) || !IsFinite(ray.direction.y) || !IsFinite(ray.direction.z)) return false;
    float nearHit = 0.0f;
    float farHit = std::numeric_limits<float>::max();
    Vec3 nearNormal{};
    const float origins[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
    const float directions[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
    const float minimums[3] = {bounds.min.x, bounds.min.y, bounds.min.z};
    const float maximums[3] = {bounds.max.x, bounds.max.y, bounds.max.z};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(directions[axis]) <= Epsilon) {
            if (origins[axis] < minimums[axis] || origins[axis] > maximums[axis]) return false;
            continue;
        }
        const float inverse = 1.0f / directions[axis];
        float first = (minimums[axis] - origins[axis]) * inverse;
        float second = (maximums[axis] - origins[axis]) * inverse;
        Vec3 axisNormal{};
        if (first > second) { std::swap(first, second); axisNormal[axis] = 1.0f; }
        else axisNormal[axis] = -1.0f;
        if (first > nearHit) { nearHit = first; nearNormal = axisNormal; }
        farHit = std::min(farHit, second);
        if (nearHit > farHit) return false;
    }
    distance = nearHit;
    if (point) *point = ray.origin + ray.direction * distance;
    if (normal) *normal = nearNormal;
    return true;
}

struct Frustum {
    enum PlaneIndex : std::size_t { Left, Right, Bottom, Top, Near, Far };
    std::array<Plane, 6> planes{};

    static Frustum FromMatrix(const Mat4& matrix) noexcept {
        Frustum result{};
        const auto row = [&matrix](int index) { return Vec4{matrix(index, 0), matrix(index, 1), matrix(index, 2), matrix(index, 3)}; };
        const Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        const std::array<Vec4, 6> equations{{r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2}};
        for (std::size_t index = 0; index < result.planes.size(); ++index)
            result.planes[index] = Plane{{equations[index].x, equations[index].y, equations[index].z}, equations[index].w}.normalized();
        return result;
    }
    bool Contains(Vec3 point) const noexcept {
        for (const auto& plane : planes) if (plane.signed_distance(point) < 0.0f) return false;
        return true;
    }
    bool Intersects(Aabb bounds) const noexcept {
        for (const auto& plane : planes) {
            const Vec3 positive{plane.normal.x >= 0.0f ? bounds.max.x : bounds.min.x,
                                plane.normal.y >= 0.0f ? bounds.max.y : bounds.min.y,
                                plane.normal.z >= 0.0f ? bounds.max.z : bounds.min.z};
            if (plane.signed_distance(positive) < 0.0f) return false;
        }
        return true;
    }
    bool Intersects(Sphere sphere) const noexcept {
        for (const auto& plane : planes) if (plane.signed_distance(sphere.center) < -sphere.radius) return false;
        return true;
    }
    bool Intersects(const Obb& box) const noexcept {
        const Vec3 center = ObbCenter(box);
        const Vec3 extents = ObbHalfExtents(box);
        const auto axes = ObbAxes(box);
        for (const auto& plane : planes) {
            const Vec3 support = center + axes[0] * (Dot(plane.normal, axes[0]) >= 0.0f ? extents.x : -extents.x) +
                                 axes[1] * (Dot(plane.normal, axes[1]) >= 0.0f ? extents.y : -extents.y) +
                                 axes[2] * (Dot(plane.normal, axes[2]) >= 0.0f ? extents.z : -extents.z);
            if (plane.signed_distance(support) < 0.0f) return false;
        }
        return true;
    }
};

inline Vec3 TriangleNormal(Vec3 a, Vec3 b, Vec3 c) noexcept { return Normalize(Cross(b - a, c - a)); }
inline Vec3 Barycentric(Vec3 point, Vec3 a, Vec3 b, Vec3 c) noexcept {
    const Vec3 v0 = b - a, v1 = c - a, v2 = point - a;
    const float denominator = Dot(v0, v0) * Dot(v1, v1) - Dot(v0, v1) * Dot(v0, v1);
    if (std::abs(denominator) <= Epsilon) return {};
    const float v = (Dot(v1, v1) * Dot(v2, v0) - Dot(v0, v1) * Dot(v2, v1)) / denominator;
    const float w = (Dot(v0, v0) * Dot(v2, v1) - Dot(v0, v1) * Dot(v2, v0)) / denominator;
    return {1.0f - v - w, v, w};
}

}
