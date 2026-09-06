#include "shinkou/MathAlgorithms.h"
#include "shinkou/MathSpatial.h"
#include <cmath>
#include <complex>
#include <iostream>
#include <vector>

namespace {
bool near(float lhs, float rhs, float epsilon = 0.002f) { return std::abs(lhs - rhs) <= epsilon; }
}

int main() {
    using namespace shinkou::math;

    const auto lookAt = LookAt({0, 0, 0}, {0, 0, 1}, {0, 0, 1});
    if (!IsFinite(lookAt.m[0]) || !IsFinite(lookAt.m[5]) || !near(Length(Vec3{lookAt.m[0], lookAt.m[1], lookAt.m[2]}), 1.0f)) return 1;

    const Triangle triangle{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}};
    float distance = 0.0f;
    Vec3 point{}, normal{};
    if (!RaycastTriangle({{0.25f, 0.25f, -2.0f}, {0, 0, 1}}, triangle, distance, &point, &normal) ||
        !near(distance, 2.0f) || !near(point.x, 0.25f) || !near(point.y, 0.25f) || !near(normal.z, 1.0f)) return 2;
    if (!near(DistanceSquared(triangle, {1, 1, 1}), 1.0f)) return 3;
    const Segment segment{{-1, 0, 0}, {1, 0, 0}};
    if (!near(DistanceSquared(segment, Vec3{0, 2, 0}), 4.0f) || !near(DistanceSquared(segment, Segment{{0, 1, 0}, {0, 3, 0}}), 1.0f)) return 161;
    const Capsule capsule{segment, 0.5f};
    if (!Intersects(capsule, Sphere{{0, 0.75f, 0}, 0.25f}) || Intersects(capsule, Sphere{{0, 2, 0}, 0.25f})) return 171;

    const std::vector<BvhPrimitive> primitives{
        {{{-1, -1, 4}, {1, 1, 6}}, 11},
        {{{4, -1, 4}, {6, 1, 6}}, 22},
        {{{-1, 4, 4}, {1, 6, 6}}, 33}};
    StaticBvh bvh;
    bvh.build({primitives.data(), primitives.size()}, 1);
    std::size_t queryCount = 0;
    bvh.query({{-2, -2, 3}, {2, 2, 7}}, [&](std::uint32_t id) { if (id == 11) ++queryCount; });
    if (queryCount != 1) return 4;
    std::uint32_t rayId = 0;
    bvh.query_ray({{0, 0, 0}, {0, 0, 1}}, 100.0f, [&](std::uint32_t id, float) { rayId = id; });
    if (rayId != 11) return 5;

    if (!near(CubicBezier(0.0f, 1.0f, 2.0f, 3.0f, 0.0f), 0.0f) || !near(CubicBezier(0.0f, 1.0f, 2.0f, 3.0f, 1.0f), 3.0f)) return 6;
    const auto bisect = Bisection([](float value) { return value * value - 2.0f; }, 0.0f, 2.0f);
    if (!bisect.converged || !near(bisect.value, std::sqrt(2.0f), 0.0001f)) return 7;
    const auto newton = Newton([](float value) { return value * value - 2.0f; }, [](float value) { return 2.0f * value; }, 1.0f);
    if (!newton.converged || !near(newton.value, std::sqrt(2.0f), 0.0001f)) return 8;

    const auto quadratic = SolveQuadratic(1.0f, -5.0f, 6.0f);
    if (quadratic.count != 2 || !near(quadratic.values[0], 2.0f) || !near(quadratic.values[1], 3.0f)) return 9;
    const auto cubic = SolveCubic(1.0f, -6.0f, 11.0f, -6.0f);
    if (cubic.count != 3 || !near(cubic.values[0], 1.0f) || !near(cubic.values[1], 2.0f) || !near(cubic.values[2], 3.0f)) return 10;

    std::vector<std::complex<float>> signal(8);
    signal[0] = 1.0f;
    const auto original = signal;
    if (!FastFourierTransform({signal.data(), signal.size()}) || !FastFourierTransform({signal.data(), signal.size()}, true)) return 11;
    for (std::size_t index = 0; index < signal.size(); ++index)
        if (!near(signal[index].real(), original[index].real(), 0.0001f) || !near(signal[index].imag(), original[index].imag(), 0.0001f)) return 12;

    if (!near(Halton(1, 2), 0.5f) || !near(Halton(2, 2), 0.25f) || !near(SimpsonIntegrate([](float value) { return value * value; }, 0.0f, 1.0f), 1.0f / 3.0f, 0.0001f)) return 13;
    Pcg32 random(42);
    const Vec3 randomDirection = random.next_unit_vector();
    if (!near(Length(randomDirection), 1.0f, 0.0001f) || random.next_float() >= 1.0f) return 14;

    DenseMatrix matrix(3, 3);
    matrix(0, 0) = 3; matrix(0, 1) = 2; matrix(0, 2) = -1;
    matrix(1, 0) = 2; matrix(1, 1) = -2; matrix(1, 2) = 4;
    matrix(2, 0) = -1; matrix(2, 1) = 0.5f; matrix(2, 2) = -1;
    const float rhsValues[] = {1, -2, 0};
    float solutionValues[3]{};
    if (!SolveLinearSystem(matrix, MakeArrayView(rhsValues), MakeArrayView(solutionValues)) ||
        !near(solutionValues[0], 1.0f) || !near(solutionValues[1], -2.0f) || !near(solutionValues[2], -2.0f)) return 15;

    std::cout << "advanced math tests passed\n";
    return 0;
}
