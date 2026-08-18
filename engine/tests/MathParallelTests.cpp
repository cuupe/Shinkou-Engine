#include "shinkou/MathGeometry.h"
#include "shinkou/MathParallel.h"
#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
bool near(float lhs, float rhs, float epsilon = 0.001f) { return std::abs(lhs - rhs) <= epsilon; }
}

int main() {
    using namespace shinkou::math;
    static_assert(std::is_trivially_copyable_v<Vec3>);
    static_assert(std::is_trivially_copyable_v<Vec4>);
    static_assert(sizeof(Mat4) == 64);

    Mat3 matrix = Mat3::Identity();
    matrix.m[0] = 2.0f; matrix.m[4] = 3.0f; matrix.m[8] = 4.0f;
    Mat3 inverse{};
    if (!Inverse(matrix, inverse) || !near((matrix * inverse).m[0], 1.0f) || !near((matrix * inverse).m[4], 1.0f) || !near((matrix * inverse).m[8], 1.0f)) return 1;

    const Aabb bounds{{-1, -1, -1}, {1, 1, 1}};
    float distance = 0.0f;
    Vec3 point{}, normal{};
    if (!RaycastAabb({{-3, 0, 0}, {1, 0, 0}}, bounds, distance, &point, &normal) || !near(distance, 2.0f) || !near(point.x, -1.0f) || !near(normal.x, -1.0f)) return 2;
    if (!RaycastSphere({{-3, 0, 0}, {1, 0, 0}}, {{0, 0, 0}, 1.0f}, distance, &point) || !near(distance, 2.0f)) return 3;
    if (!Contains(bounds, {0, 0, 0}) || !Intersects(bounds, Sphere{{2, 0, 0}, 1.0f})) return 4;
    const auto frustum = Frustum::FromMatrix(Mat4::Identity());
    if (!frustum.Contains({0, 0, 0}) || frustum.Contains({2, 0, 0})) return 5;

    constexpr std::size_t count = 10000;
    std::vector<Vec3> input(count), output(count), lerped(count);
    for (std::size_t index = 0; index < count; ++index) input[index] = {static_cast<float>(index), 1.0f, -2.0f};
    ParallelExecutor executor({2, 64, true});
    for (int iteration = 0; iteration < 8; ++iteration) {
        ParallelTransformPoints(executor, Translation({2, 3, 4}), {input.data(), input.size()}, {output.data(), output.size()});
        ParallelLerp(executor, {input.data(), input.size()}, {output.data(), output.size()}, {lerped.data(), lerped.size()}, 0.5f);
    }
    if (!near(output[123].x, 125.0f) || !near(output[123].y, 4.0f) || !near(lerped[123].z, 0.0f)) return 6;

    const Mat4 simdMatrix = TransformMatrix({{2, -1, 3}, FromAxisAngle({0, 1, 0}, Pi * 0.25f), {2, 1, 0.5f}});
    std::vector<Vec3> simdOutput(count);
    TransformPointsSimd(simdMatrix, {input.data(), input.size()}, {simdOutput.data(), simdOutput.size()});
    for (std::size_t index : {std::size_t{0}, std::size_t{123}, count - 1}) {
        const Vec3 expected = TransformPoint(simdMatrix, input[index]);
        if (!near(simdOutput[index].x, expected.x) || !near(simdOutput[index].y, expected.y) || !near(simdOutput[index].z, expected.z)) return 7;
    }

    const Obb unitBox{Transform{{0, 0, 0}, Quat::Identity(), {1, 1, 1}}, {1, 1, 1}};
    const Obb touchingBox{Transform{{1.5f, 0, 0}, Quat::Identity(), {1, 1, 1}}, {1, 1, 1}};
    const Obb separatedBox{Transform{{3.1f, 0, 0}, Quat::Identity(), {1, 1, 1}}, {1, 1, 1}};
    if (!Intersects(unitBox, touchingBox) || Intersects(unitBox, separatedBox)) return 8;
    if (!Contains(unitBox, {0.5f, 0.5f, 0.5f}) || Intersects(unitBox, Sphere{{3, 0, 0}, 0.5f})) return 9;
    if (!Intersects(unitBox, Sphere{{1.4f, 0, 0}, 0.5f})) return 10;
    if (!RaycastObb({{-3, 0, 0}, {1, 0, 0}}, unitBox, distance, &point, &normal) ||
        !near(distance, 2.0f) || !near(point.x, -1.0f) || !near(normal.x, -1.0f)) return 11;

    const Vec3 large = Normalize(Vec3{1.0e30f, 1.0e30f, 0.0f});
    if (!near(Length(large), 1.0f)) return 12;
    const Mat4 invalidProjection = Perspective(0.0f, 0.0f, -1.0f, -2.0f);
    if (invalidProjection.m[0] != 1.0f || invalidProjection.m[5] != 1.0f || invalidProjection.m[10] != 1.0f || invalidProjection.m[15] != 1.0f) return 13;

    std::atomic<std::size_t> visited{0};
    executor.for_each(count, [&](std::size_t) { visited.fetch_add(1, std::memory_order_relaxed); }, 64);
    if (visited.load(std::memory_order_relaxed) != count) return 14;

    std::atomic<std::size_t> concurrentVisits{0};
    std::thread first([&] { executor.for_each(2048, [&](std::size_t) { concurrentVisits.fetch_add(1, std::memory_order_relaxed); }, 32); });
    std::thread second([&] { executor.for_each(2048, [&](std::size_t) { concurrentVisits.fetch_add(1, std::memory_order_relaxed); }, 32); });
    first.join();
    second.join();
    if (concurrentVisits.load(std::memory_order_relaxed) != 4096) return 15;

    std::atomic<std::size_t> nestedVisits{0};
    executor.for_each(512, [&](std::size_t index) {
        if (index == 0) executor.for_each(256, [&](std::size_t) { nestedVisits.fetch_add(1, std::memory_order_relaxed); }, 16);
    }, 16);
    if (nestedVisits.load(std::memory_order_relaxed) != 256) return 16;

    bool caught = false;
    try {
        executor.for_each(256, [&](std::size_t index) {
            if (index == 3) throw std::runtime_error("math worker failure");
        }, 1);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    if (!caught) return 17;

    ParallelExecutor serial({0, 1, false});
    std::size_t serialSum = 0;
    serial.for_each(32, [&](std::size_t index) { serialSum += index; });
    if (serialSum != 496) return 18;
    serial.shutdown();
    if (!serial.is_shutdown()) return 19;
    std::cout << "math parallel tests passed\n";
    return 0;
}
