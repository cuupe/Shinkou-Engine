#include "shinkou/MathAlgorithms.h"
#include "shinkou/MathParallel.h"
#include <chrono>
#include <cstddef>
#include <iostream>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double seconds_since(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
}

int main() {
    using namespace shinkou::math;
    constexpr std::size_t count = 1u << 20;
    constexpr int repetitions = 20;
    std::vector<Vec3> input(count), output(count), reference(count);
    std::vector<float> inputX(count), inputY(count), inputZ(count), outputX(count), outputY(count), outputZ(count);
    for (std::size_t index = 0; index < count; ++index) input[index] = {static_cast<float>(index & 1023u), 1.0f, -2.0f};
    for (std::size_t index = 0; index < count; ++index) { inputX[index] = input[index].x; inputY[index] = input[index].y; inputZ[index] = input[index].z; }
    const Mat4 matrix = TransformMatrix({{2, -1, 3}, FromAxisAngle({0, 1, 0}, Pi * 0.25f), {2, 1, 0.5f}});

    auto start = Clock::now();
    for (int iteration = 0; iteration < repetitions; ++iteration)
        for (std::size_t index = 0; index < count; ++index) output[index] = TransformPoint(matrix, input[index]);
    const double scalarSeconds = seconds_since(start);

    start = Clock::now();
    for (int iteration = 0; iteration < repetitions; ++iteration) TransformPointsSimd(matrix, {input.data(), input.size()}, {output.data(), output.size()});
    const double simdSeconds = seconds_since(start);

    ParallelExecutor executor;
    start = Clock::now();
    for (int iteration = 0; iteration < repetitions; ++iteration) ParallelTransformPoints(executor, matrix, {input.data(), input.size()}, {output.data(), output.size()});
    const double parallelSeconds = seconds_since(start);

    start = Clock::now();
    for (int iteration = 0; iteration < repetitions; ++iteration)
        ParallelTransformPointsSoa(executor, matrix, {inputX.data(), inputY.data(), inputZ.data(), count}, {outputX.data(), outputY.data(), outputZ.data(), count});
    const double soaSeconds = seconds_since(start);

    for (std::size_t index = 0; index < count; ++index) reference[index] = TransformPoint(matrix, input[index]);
    double error = 0.0;
    for (std::size_t index = 0; index < count; ++index) error += std::abs(output[index].x - reference[index].x);
    for (std::size_t index = 0; index < count; ++index) error += std::abs(outputX[index] - reference[index].x);
    const double operations = static_cast<double>(count) * repetitions;
    std::cout << "math benchmark\n"
              << "  simd available: " << (SimdAvailable() ? "yes" : "no") << "\n"
              << "  workers: " << executor.worker_count() << "\n"
              << "  scalar Mpoints/s: " << operations / scalarSeconds / 1.0e6 << "\n"
              << "  simd Mpoints/s: " << operations / simdSeconds / 1.0e6 << "\n"
              << "  parallel Mpoints/s: " << operations / parallelSeconds / 1.0e6 << "\n"
              << "  parallel SoA Mpoints/s: " << operations / soaSeconds / 1.0e6 << "\n"
              << "  checksum: " << error << "\n";
    return error < 0.1 ? 0 : 1;
}
