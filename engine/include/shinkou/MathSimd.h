#pragma once

#include "shinkou/Math.h"
#include <cstddef>

#if defined(__AVX2__) || defined(_M_AVX2)
#    include <immintrin.h>
#    define SHINKOU_MATH_HAS_AVX2 1
#else
#    define SHINKOU_MATH_HAS_AVX2 0
#endif

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#    include <emmintrin.h>
#    define SHINKOU_MATH_HAS_SSE2 1
#else
#    define SHINKOU_MATH_HAS_SSE2 0
#endif

#if defined(_MSC_VER)
#    define SHINKOU_MATH_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#    define SHINKOU_MATH_NOINLINE __attribute__((noinline))
#else
#    define SHINKOU_MATH_NOINLINE
#endif

namespace shinkou::math {

inline constexpr bool SimdAvailable() noexcept { return SHINKOU_MATH_HAS_SSE2 != 0; }
inline constexpr bool Avx2Available() noexcept { return SHINKOU_MATH_HAS_AVX2 != 0; }

struct Vec3SoaConstView {
    const float* x{nullptr};
    const float* y{nullptr};
    const float* z{nullptr};
    std::size_t count{0};
    bool valid() const noexcept { return x != nullptr && y != nullptr && z != nullptr; }
};

struct Vec3SoaView {
    float* x{nullptr};
    float* y{nullptr};
    float* z{nullptr};
    std::size_t count{0};
    bool valid() const noexcept { return x != nullptr && y != nullptr && z != nullptr; }
};

// Transforms four Vec3 values per SIMD iteration without changing the public
// Vec3/Mat4 layout. Exact in-place input/output is supported.
inline void TransformPointsSimd(const Mat4& matrix, ConstArrayView<Vec3> input,
                                ArrayView<Vec3> output) noexcept {
    if (input.size() != output.size()) return;
    // Keep the aliasing contract explicit. The SIMD loop is optimized for
    // independent ranges; exact in-place transforms use a scalar snapshot so
    // compiler vectorization cannot reorder a load past an overlapping store.
    if (input.data() == output.data()) {
        for (std::size_t index = 0; index < input.size(); ++index) output[index] = TransformPoint(matrix, input[index]);
        return;
    }

#if SHINKOU_MATH_HAS_SSE2
    const __m128 m0 = _mm_set1_ps(matrix.m[0]);
    const __m128 m1 = _mm_set1_ps(matrix.m[1]);
    const __m128 m2 = _mm_set1_ps(matrix.m[2]);
    const __m128 m3 = _mm_set1_ps(matrix.m[3]);
    const __m128 m4 = _mm_set1_ps(matrix.m[4]);
    const __m128 m5 = _mm_set1_ps(matrix.m[5]);
    const __m128 m6 = _mm_set1_ps(matrix.m[6]);
    const __m128 m7 = _mm_set1_ps(matrix.m[7]);
    const __m128 m8 = _mm_set1_ps(matrix.m[8]);
    const __m128 m9 = _mm_set1_ps(matrix.m[9]);
    const __m128 m10 = _mm_set1_ps(matrix.m[10]);
    const __m128 m11 = _mm_set1_ps(matrix.m[11]);
    const __m128 m12 = _mm_set1_ps(matrix.m[12]);
    const __m128 m13 = _mm_set1_ps(matrix.m[13]);
    const __m128 m14 = _mm_set1_ps(matrix.m[14]);
    const __m128 m15 = _mm_set1_ps(matrix.m[15]);
    const __m128 one = _mm_set1_ps(1.0f);
    const __m128 epsilon = _mm_set1_ps(Epsilon);
    const __m128 signMask = _mm_set1_ps(-0.0f);

    const std::size_t simdCount = input.size() / 4 * 4;
    for (std::size_t index = 0; index < simdCount; index += 4) {
        const Vec3& p0 = input[index + 0];
        const Vec3& p1 = input[index + 1];
        const Vec3& p2 = input[index + 2];
        const Vec3& p3 = input[index + 3];
        const __m128 x = _mm_set_ps(p3.x, p2.x, p1.x, p0.x);
        const __m128 y = _mm_set_ps(p3.y, p2.y, p1.y, p0.y);
        const __m128 z = _mm_set_ps(p3.z, p2.z, p1.z, p0.z);

        __m128 resultX = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, m0), _mm_mul_ps(y, m4)), _mm_mul_ps(z, m8));
        __m128 resultY = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, m1), _mm_mul_ps(y, m5)), _mm_mul_ps(z, m9));
        __m128 resultZ = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, m2), _mm_mul_ps(y, m6)), _mm_mul_ps(z, m10));
        __m128 resultW = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, m3), _mm_mul_ps(y, m7)), _mm_mul_ps(z, m11));
        resultX = _mm_add_ps(resultX, m12);
        resultY = _mm_add_ps(resultY, m13);
        resultZ = _mm_add_ps(resultZ, m14);
        resultW = _mm_add_ps(resultW, m15);

        const __m128 absoluteW = _mm_andnot_ps(signMask, resultW);
        const __m128 validW = _mm_cmpgt_ps(absoluteW, epsilon);
        const __m128 safeW = _mm_or_ps(_mm_and_ps(validW, resultW), _mm_andnot_ps(validW, one));
        resultX = _mm_div_ps(resultX, safeW);
        resultY = _mm_div_ps(resultY, safeW);
        resultZ = _mm_div_ps(resultZ, safeW);

        alignas(16) float xValues[4];
        alignas(16) float yValues[4];
        alignas(16) float zValues[4];
        _mm_storeu_ps(xValues, resultX);
        _mm_storeu_ps(yValues, resultY);
        _mm_storeu_ps(zValues, resultZ);
        for (std::size_t lane = 0; lane < 4; ++lane) output[index + lane] = {xValues[lane], yValues[lane], zValues[lane]};
    }
    for (std::size_t index = simdCount; index < input.size(); ++index) output[index] = TransformPoint(matrix, input[index]);
#else
    for (std::size_t index = 0; index < input.size(); ++index) output[index] = TransformPoint(matrix, input[index]);
#endif
}

// Direction transform variant: identical layout and aliasing guarantees, but
// deliberately omits translation and perspective division.
inline void TransformVectorsSimd(const Mat4& matrix, ConstArrayView<Vec3> input,
                                 ArrayView<Vec3> output) noexcept {
    if (input.size() != output.size()) return;
#if SHINKOU_MATH_HAS_SSE2
    const __m128 m0 = _mm_set1_ps(matrix.m[0]);
    const __m128 m1 = _mm_set1_ps(matrix.m[1]);
    const __m128 m2 = _mm_set1_ps(matrix.m[2]);
    const __m128 m4 = _mm_set1_ps(matrix.m[4]);
    const __m128 m5 = _mm_set1_ps(matrix.m[5]);
    const __m128 m6 = _mm_set1_ps(matrix.m[6]);
    const __m128 m8 = _mm_set1_ps(matrix.m[8]);
    const __m128 m9 = _mm_set1_ps(matrix.m[9]);
    const __m128 m10 = _mm_set1_ps(matrix.m[10]);
    const std::size_t simdCount = input.size() / 4 * 4;
    for (std::size_t index = 0; index < simdCount; index += 4) {
        const Vec3& p0 = input[index + 0]; const Vec3& p1 = input[index + 1];
        const Vec3& p2 = input[index + 2]; const Vec3& p3 = input[index + 3];
        const __m128 x = _mm_set_ps(p3.x, p2.x, p1.x, p0.x);
        const __m128 y = _mm_set_ps(p3.y, p2.y, p1.y, p0.y);
        const __m128 z = _mm_set_ps(p3.z, p2.z, p1.z, p0.z);
        const __m128 resultX = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, m0), _mm_mul_ps(y, m4)), _mm_mul_ps(z, m8));
        const __m128 resultY = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, m1), _mm_mul_ps(y, m5)), _mm_mul_ps(z, m9));
        const __m128 resultZ = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, m2), _mm_mul_ps(y, m6)), _mm_mul_ps(z, m10));
        alignas(16) float xValues[4]; alignas(16) float yValues[4]; alignas(16) float zValues[4];
        _mm_storeu_ps(xValues, resultX); _mm_storeu_ps(yValues, resultY); _mm_storeu_ps(zValues, resultZ);
        for (std::size_t lane = 0; lane < 4; ++lane) output[index + lane] = {xValues[lane], yValues[lane], zValues[lane]};
    }
    for (std::size_t index = simdCount; index < input.size(); ++index) output[index] = TransformVector(matrix, input[index]);
#else
    for (std::size_t index = 0; index < input.size(); ++index) output[index] = TransformVector(matrix, input[index]);
#endif
}

inline void LerpSimd(ConstArrayView<Vec3> a, ConstArrayView<Vec3> b,
                     ArrayView<Vec3> output, float weight) noexcept {
    if (a.size() != b.size() || a.size() != output.size()) return;
#if SHINKOU_MATH_HAS_SSE2
    const __m128 t = _mm_set1_ps(weight);
    const __m128 oneMinusT = _mm_set1_ps(1.0f - weight);
    const std::size_t simdCount = a.size() / 4 * 4;
    for (std::size_t index = 0; index < simdCount; index += 4) {
        const Vec3& a0 = a[index + 0]; const Vec3& a1 = a[index + 1]; const Vec3& a2 = a[index + 2]; const Vec3& a3 = a[index + 3];
        const Vec3& b0 = b[index + 0]; const Vec3& b1 = b[index + 1]; const Vec3& b2 = b[index + 2]; const Vec3& b3 = b[index + 3];
        const __m128 ax = _mm_set_ps(a3.x, a2.x, a1.x, a0.x), ay = _mm_set_ps(a3.y, a2.y, a1.y, a0.y), az = _mm_set_ps(a3.z, a2.z, a1.z, a0.z);
        const __m128 bx = _mm_set_ps(b3.x, b2.x, b1.x, b0.x), by = _mm_set_ps(b3.y, b2.y, b1.y, b0.y), bz = _mm_set_ps(b3.z, b2.z, b1.z, b0.z);
        const __m128 x = _mm_add_ps(_mm_mul_ps(ax, oneMinusT), _mm_mul_ps(bx, t));
        const __m128 y = _mm_add_ps(_mm_mul_ps(ay, oneMinusT), _mm_mul_ps(by, t));
        const __m128 z = _mm_add_ps(_mm_mul_ps(az, oneMinusT), _mm_mul_ps(bz, t));
        alignas(16) float xValues[4]; alignas(16) float yValues[4]; alignas(16) float zValues[4];
        _mm_storeu_ps(xValues, x); _mm_storeu_ps(yValues, y); _mm_storeu_ps(zValues, z);
        for (std::size_t lane = 0; lane < 4; ++lane) output[index + lane] = {xValues[lane], yValues[lane], zValues[lane]};
    }
    for (std::size_t index = simdCount; index < a.size(); ++index) output[index] = Lerp(a[index], b[index], weight);
#else
    for (std::size_t index = 0; index < a.size(); ++index) output[index] = Lerp(a[index], b[index], weight);
#endif
}

inline SHINKOU_MATH_NOINLINE void TransformPointsSoaSimd(const Mat4& matrix, Vec3SoaConstView input,
                                                         Vec3SoaView output) noexcept {
    if (!input.valid() || !output.valid() || input.count != output.count) return;
#if SHINKOU_MATH_HAS_AVX2 && !defined(__MINGW32__)
    const __m256 m0 = _mm256_set1_ps(matrix.m[0]), m1 = _mm256_set1_ps(matrix.m[1]), m2 = _mm256_set1_ps(matrix.m[2]), m3 = _mm256_set1_ps(matrix.m[3]);
    const __m256 m4 = _mm256_set1_ps(matrix.m[4]), m5 = _mm256_set1_ps(matrix.m[5]), m6 = _mm256_set1_ps(matrix.m[6]), m7 = _mm256_set1_ps(matrix.m[7]);
    const __m256 m8 = _mm256_set1_ps(matrix.m[8]), m9 = _mm256_set1_ps(matrix.m[9]), m10 = _mm256_set1_ps(matrix.m[10]), m11 = _mm256_set1_ps(matrix.m[11]);
    const __m256 m12 = _mm256_set1_ps(matrix.m[12]), m13 = _mm256_set1_ps(matrix.m[13]), m14 = _mm256_set1_ps(matrix.m[14]), m15 = _mm256_set1_ps(matrix.m[15]);
    const __m256 one = _mm256_set1_ps(1.0f), epsilon = _mm256_set1_ps(Epsilon), signMask = _mm256_set1_ps(-0.0f);
    const std::size_t vectorCount = input.count / 8 * 8;
    for (std::size_t index = 0; index < vectorCount; index += 8) {
        const __m256 x = _mm256_loadu_ps(input.x + index), y = _mm256_loadu_ps(input.y + index), z = _mm256_loadu_ps(input.z + index);
        __m256 resultX = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(x, m0), _mm256_mul_ps(y, m4)), _mm256_mul_ps(z, m8));
        __m256 resultY = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(x, m1), _mm256_mul_ps(y, m5)), _mm256_mul_ps(z, m9));
        __m256 resultZ = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(x, m2), _mm256_mul_ps(y, m6)), _mm256_mul_ps(z, m10));
        __m256 resultW = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(x, m3), _mm256_mul_ps(y, m7)), _mm256_mul_ps(z, m11));
        resultX = _mm256_add_ps(resultX, m12); resultY = _mm256_add_ps(resultY, m13); resultZ = _mm256_add_ps(resultZ, m14); resultW = _mm256_add_ps(resultW, m15);
        const __m256 validW = _mm256_cmp_ps(_mm256_andnot_ps(signMask, resultW), epsilon, _CMP_GT_OQ);
        const __m256 safeW = _mm256_blendv_ps(one, resultW, validW);
        _mm256_storeu_ps(output.x + index, _mm256_div_ps(resultX, safeW));
        _mm256_storeu_ps(output.y + index, _mm256_div_ps(resultY, safeW));
        _mm256_storeu_ps(output.z + index, _mm256_div_ps(resultZ, safeW));
    }
    for (std::size_t index = vectorCount; index < input.count; ++index) {
        const Vec3 point = TransformPoint(matrix, {input.x[index], input.y[index], input.z[index]});
        output.x[index] = point.x; output.y[index] = point.y; output.z[index] = point.z;
    }
#elif SHINKOU_MATH_HAS_SSE2
    const __m128 m0 = _mm_set1_ps(matrix.m[0]), m1 = _mm_set1_ps(matrix.m[1]), m2 = _mm_set1_ps(matrix.m[2]), m3 = _mm_set1_ps(matrix.m[3]);
    const __m128 m4 = _mm_set1_ps(matrix.m[4]), m5 = _mm_set1_ps(matrix.m[5]), m6 = _mm_set1_ps(matrix.m[6]), m7 = _mm_set1_ps(matrix.m[7]);
    const __m128 m8 = _mm_set1_ps(matrix.m[8]), m9 = _mm_set1_ps(matrix.m[9]), m10 = _mm_set1_ps(matrix.m[10]), m11 = _mm_set1_ps(matrix.m[11]);
    const __m128 m12 = _mm_set1_ps(matrix.m[12]), m13 = _mm_set1_ps(matrix.m[13]), m14 = _mm_set1_ps(matrix.m[14]), m15 = _mm_set1_ps(matrix.m[15]);
    const __m128 one = _mm_set1_ps(1.0f), epsilon = _mm_set1_ps(Epsilon), signMask = _mm_set1_ps(-0.0f);
    const std::size_t vectorCount = input.count / 4 * 4;
    for (std::size_t index = 0; index < vectorCount; index += 4) {
        const __m128 x = _mm_loadu_ps(input.x + index), y = _mm_loadu_ps(input.y + index), z = _mm_loadu_ps(input.z + index);
        __m128 resultX = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, m0), _mm_mul_ps(y, m4)), _mm_mul_ps(z, m8));
        __m128 resultY = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, m1), _mm_mul_ps(y, m5)), _mm_mul_ps(z, m9));
        __m128 resultZ = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, m2), _mm_mul_ps(y, m6)), _mm_mul_ps(z, m10));
        __m128 resultW = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, m3), _mm_mul_ps(y, m7)), _mm_mul_ps(z, m11));
        resultX = _mm_add_ps(resultX, m12); resultY = _mm_add_ps(resultY, m13); resultZ = _mm_add_ps(resultZ, m14); resultW = _mm_add_ps(resultW, m15);
        const __m128 validW = _mm_cmpgt_ps(_mm_andnot_ps(signMask, resultW), epsilon);
        const __m128 safeW = _mm_or_ps(_mm_and_ps(validW, resultW), _mm_andnot_ps(validW, one));
        _mm_storeu_ps(output.x + index, _mm_div_ps(resultX, safeW));
        _mm_storeu_ps(output.y + index, _mm_div_ps(resultY, safeW));
        _mm_storeu_ps(output.z + index, _mm_div_ps(resultZ, safeW));
    }
    for (std::size_t index = vectorCount; index < input.count; ++index) {
        const Vec3 point = TransformPoint(matrix, {input.x[index], input.y[index], input.z[index]});
        output.x[index] = point.x; output.y[index] = point.y; output.z[index] = point.z;
    }
#else
    for (std::size_t index = 0; index < input.count; ++index) {
        const Vec3 point = TransformPoint(matrix, {input.x[index], input.y[index], input.z[index]});
        output.x[index] = point.x; output.y[index] = point.y; output.z[index] = point.z;
    }
#endif
}

inline SHINKOU_MATH_NOINLINE void LerpSoaSimd(Vec3SoaConstView a, Vec3SoaConstView b,
                                              Vec3SoaView output, float weight) noexcept {
    if (!a.valid() || !b.valid() || !output.valid() || a.count != b.count || a.count != output.count) return;
#if SHINKOU_MATH_HAS_AVX2 && !defined(__MINGW32__)
    const __m256 t = _mm256_set1_ps(weight), oneMinusT = _mm256_set1_ps(1.0f - weight);
    const std::size_t vectorCount = a.count / 8 * 8;
    for (std::size_t index = 0; index < vectorCount; index += 8) {
        _mm256_storeu_ps(output.x + index, _mm256_add_ps(_mm256_mul_ps(_mm256_loadu_ps(a.x + index), oneMinusT), _mm256_mul_ps(_mm256_loadu_ps(b.x + index), t)));
        _mm256_storeu_ps(output.y + index, _mm256_add_ps(_mm256_mul_ps(_mm256_loadu_ps(a.y + index), oneMinusT), _mm256_mul_ps(_mm256_loadu_ps(b.y + index), t)));
        _mm256_storeu_ps(output.z + index, _mm256_add_ps(_mm256_mul_ps(_mm256_loadu_ps(a.z + index), oneMinusT), _mm256_mul_ps(_mm256_loadu_ps(b.z + index), t)));
    }
    for (std::size_t index = vectorCount; index < a.count; ++index) {
        output.x[index] = a.x[index] + (b.x[index] - a.x[index]) * weight;
        output.y[index] = a.y[index] + (b.y[index] - a.y[index]) * weight;
        output.z[index] = a.z[index] + (b.z[index] - a.z[index]) * weight;
    }
#elif SHINKOU_MATH_HAS_SSE2
    const __m128 t = _mm_set1_ps(weight), oneMinusT = _mm_set1_ps(1.0f - weight);
    const std::size_t vectorCount = a.count / 4 * 4;
    for (std::size_t index = 0; index < vectorCount; index += 4) {
        _mm_storeu_ps(output.x + index, _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(a.x + index), oneMinusT), _mm_mul_ps(_mm_loadu_ps(b.x + index), t)));
        _mm_storeu_ps(output.y + index, _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(a.y + index), oneMinusT), _mm_mul_ps(_mm_loadu_ps(b.y + index), t)));
        _mm_storeu_ps(output.z + index, _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(a.z + index), oneMinusT), _mm_mul_ps(_mm_loadu_ps(b.z + index), t)));
    }
    for (std::size_t index = vectorCount; index < a.count; ++index) {
        output.x[index] = a.x[index] + (b.x[index] - a.x[index]) * weight;
        output.y[index] = a.y[index] + (b.y[index] - a.y[index]) * weight;
        output.z[index] = a.z[index] + (b.z[index] - a.z[index]) * weight;
    }
#else
    for (std::size_t index = 0; index < a.count; ++index) {
        output.x[index] = a.x[index] + (b.x[index] - a.x[index]) * weight;
        output.y[index] = a.y[index] + (b.y[index] - a.y[index]) * weight;
        output.z[index] = a.z[index] + (b.z[index] - a.z[index]) * weight;
    }
#endif
}

}

#undef SHINKOU_MATH_HAS_SSE2
#undef SHINKOU_MATH_HAS_AVX2
#undef SHINKOU_MATH_NOINLINE
