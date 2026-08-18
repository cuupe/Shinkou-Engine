#pragma once

#include "shinkou/Math.h"
#include <cstddef>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#    include <emmintrin.h>
#    define SHINKOU_MATH_HAS_SSE2 1
#else
#    define SHINKOU_MATH_HAS_SSE2 0
#endif

namespace shinkou::math {

inline constexpr bool SimdAvailable() noexcept { return SHINKOU_MATH_HAS_SSE2 != 0; }

// Transforms four Vec3 values per SIMD iteration without changing the public
// Vec3/Mat4 layout. Exact in-place input/output is supported.
inline void TransformPointsSimd(const Mat4& matrix, ConstArrayView<Vec3> input,
                                ArrayView<Vec3> output) noexcept {
    if (input.size() != output.size()) return;

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
        _mm_store_ps(xValues, resultX);
        _mm_store_ps(yValues, resultY);
        _mm_store_ps(zValues, resultZ);
        for (std::size_t lane = 0; lane < 4; ++lane) output[index + lane] = {xValues[lane], yValues[lane], zValues[lane]};
    }
    for (std::size_t index = simdCount; index < input.size(); ++index) output[index] = TransformPoint(matrix, input[index]);
#else
    for (std::size_t index = 0; index < input.size(); ++index) output[index] = TransformPoint(matrix, input[index]);
#endif
}

}

#undef SHINKOU_MATH_HAS_SSE2
