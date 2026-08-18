#include "shinkou/Math.h"
#include <cmath>
#include <iostream>
#include <limits>

namespace {
bool near(float lhs, float rhs, float epsilon = 0.0003f) { return std::abs(lhs - rhs) <= epsilon; }
bool near_vec(shinkou::math::Vec3 lhs, shinkou::math::Vec3 rhs) {
    return near(lhs.x, rhs.x) && near(lhs.y, rhs.y) && near(lhs.z, rhs.z);
}
}

int main() {
    using namespace shinkou::math;
    const auto rotation = FromAxisAngle({0.0f, 1.0f, 0.0f}, Pi * 0.5f);
    if (!near_vec(Rotate(rotation, {1, 0, 0}), {0, 0, -1})) return 1;
    if (!near(Length(Normalize(Vec3{3, 4, 0})), 1.0f)) return 2;

    const Transform source{{3.0f, -2.0f, 5.0f}, rotation, {2.0f, 3.0f, 4.0f}};
    const Mat4 matrix = TransformMatrix(source);
    const Mat4 inverse = InverseOrIdentity(matrix);
    const Mat4 identity = matrix * inverse;
    for (int index = 0; index < 16; ++index) {
        const float expected = (index % 5 == 0) ? 1.0f : 0.0f;
        if (!near(identity.m[index], expected, 0.001f)) return 3;
    }

    Transform decomposed{};
    if (!Decompose(matrix, decomposed)) return 4;
    if (!near_vec(decomposed.position, source.position) || !near_vec(decomposed.scale, source.scale)) return 5;
    if (!near_vec(TransformPoint(matrix, {0, 0, 0}), source.position)) return 6;
    if (!near_vec(TransformVector(matrix, {1, 0, 0}), Rotate(rotation, {2, 0, 0}))) return 7;

    const auto projection = Perspective(Pi / 3.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    if (!IsFinite(projection.m[0]) || !IsFinite(projection.m[14])) return 8;
    if (!Overlaps({{0, 0, 0}, {1, 1, 1}}, {{1, 1, 1}, {2, 2, 2}})) return 9;

    Mat4 nonFinite = Mat4::Identity();
    nonFinite.m[0] = std::numeric_limits<float>::quiet_NaN();
    Mat4 inverseCheck{};
    if (Inverse(nonFinite, inverseCheck)) return 10;

    const Transform mirrored{{0, 0, 0}, rotation, {-2, 3, 4}};
    Transform mirroredDecomposed{};
    if (!Decompose(TransformMatrix(mirrored), mirroredDecomposed)) return 11;
    const Mat4 mirroredMatrix = TransformMatrix(mirrored);
    const Mat4 mirroredRoundTrip = TransformMatrix(mirroredDecomposed);
    for (int index = 0; index < 16; ++index) if (!near(mirroredRoundTrip.m[index], mirroredMatrix.m[index], 0.001f)) return 12;
    std::cout << "math tests passed\n";
    return 0;
}
