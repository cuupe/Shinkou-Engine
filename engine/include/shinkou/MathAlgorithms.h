#pragma once

#include "shinkou/Math.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace shinkou::math {

template<class T>
inline T Hermite(T p0, T v0, T p1, T v1, float t) noexcept {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return p0 * (2.0f * t3 - 3.0f * t2 + 1.0f) +
           v0 * (t3 - 2.0f * t2 + t) +
           p1 * (-2.0f * t3 + 3.0f * t2) +
           v1 * (t3 - t2);
}

template<class T>
inline T CubicBezier(T p0, T p1, T p2, T p3, float t) noexcept {
    const float u = 1.0f - t;
    return p0 * (u * u * u) + p1 * (3.0f * u * u * t) +
           p2 * (3.0f * u * t * t) + p3 * (t * t * t);
}

template<class T>
inline T CatmullRom(T p0, T p1, T p2, T p3, float t, float tension = 0.0f) noexcept {
    const float scale = 0.5f * (1.0f - tension);
    const T v0 = (p2 - p0) * scale;
    const T v1 = (p3 - p1) * scale;
    return Hermite(p1, v0, p2, v1, t);
}

inline Vec3 SlerpDirection(Vec3 a, Vec3 b, float t) noexcept {
    a = Normalize(a);
    b = Normalize(b);
    if (LengthSquared(a) <= Epsilon || LengthSquared(b) <= Epsilon) return {};
    const float cosine = std::clamp(Dot(a, b), -1.0f, 1.0f);
    if (cosine > 0.9995f) return Normalize(Lerp(a, b, Clamp01(t)));
    if (cosine < -0.9995f) {
        Vec3 axis = std::abs(a.x) < std::abs(a.y) && std::abs(a.x) < std::abs(a.z) ? Vec3{1, 0, 0} :
                    (std::abs(a.y) < std::abs(a.z) ? Vec3{0, 1, 0} : Vec3{0, 0, 1});
        axis = Normalize(Cross(a, axis));
        return Normalize(a * std::cos(Pi * Clamp01(t)) + axis * std::sin(Pi * Clamp01(t)));
    }
    const float angle = std::acos(cosine);
    const float sine = std::sin(angle);
    if (std::abs(sine) <= Epsilon) return a;
    const float weight = Clamp01(t);
    return Normalize(a * (std::sin((1.0f - weight) * angle) / sine) +
                     b * (std::sin(weight * angle) / sine));
}

struct RootSolveResult {
    float value{0.0f};
    std::size_t iterations{0};
    bool converged{false};
};

template<class Function>
inline RootSolveResult Bisection(Function&& function, float lower, float upper,
                                 float tolerance = 1.0e-5f, std::size_t maxIterations = 64) noexcept {
    RootSolveResult result{lower, 0, false};
    if (!IsFinite(lower) || !IsFinite(upper) || !IsFinite(tolerance) || tolerance <= 0.0f || lower > upper || maxIterations == 0) return result;
    float fLower = function(lower);
    float fUpper = function(upper);
    if (!IsFinite(fLower) || !IsFinite(fUpper) || fLower * fUpper > 0.0f) return result;
    if (std::abs(fLower) <= tolerance) return {lower, 0, true};
    if (std::abs(fUpper) <= tolerance) return {upper, 0, true};
    for (std::size_t iteration = 1; iteration <= maxIterations; ++iteration) {
        const float middle = lower + (upper - lower) * 0.5f;
        const float fMiddle = function(middle);
        result = {middle, iteration, false};
        if (!IsFinite(fMiddle)) return result;
        if (std::abs(fMiddle) <= tolerance || std::abs(upper - lower) <= tolerance) {
            result.converged = true;
            return result;
        }
        if ((fLower < 0.0f) == (fMiddle < 0.0f)) { lower = middle; fLower = fMiddle; }
        else { upper = middle; fUpper = fMiddle; }
    }
    return result;
}

template<class Function, class Derivative>
inline RootSolveResult Newton(Function&& function, Derivative&& derivative, float initial,
                              float tolerance = 1.0e-5f, std::size_t maxIterations = 32) noexcept {
    RootSolveResult result{initial, 0, false};
    if (!IsFinite(initial) || !IsFinite(tolerance) || tolerance <= 0.0f || maxIterations == 0) return result;
    float value = initial;
    for (std::size_t iteration = 1; iteration <= maxIterations; ++iteration) {
        const float f = function(value);
        const float slope = derivative(value);
        if (!IsFinite(f) || !IsFinite(slope) || std::abs(slope) <= Epsilon) return {value, iteration, false};
        const float next = value - f / slope;
        result = {next, iteration, IsFinite(next) && (std::abs(f) <= tolerance || std::abs(next - value) <= tolerance)};
        if (!IsFinite(next) || result.converged) return result;
        value = next;
    }
    return result;
}

struct PolynomialRoots {
    std::array<float, 3> values{};
    std::size_t count{0};
};

inline void SortRoots(PolynomialRoots& roots) noexcept {
    std::sort(roots.values.begin(), roots.values.begin() + static_cast<std::ptrdiff_t>(roots.count));
}

inline PolynomialRoots SolveQuadratic(float a, float b, float c) noexcept {
    PolynomialRoots result{};
    if (!IsFinite(a) || !IsFinite(b) || !IsFinite(c)) return result;
    if (std::abs(a) <= Epsilon) {
        if (std::abs(b) > Epsilon) { result.values[0] = -c / b; result.count = 1; }
        return result;
    }
    const float discriminant = b * b - 4.0f * a * c;
    if (!IsFinite(discriminant) || discriminant < 0.0f) return result;
    const float root = std::sqrt(std::max(discriminant, 0.0f));
    if (root <= Epsilon) {
        result.values[0] = -0.5f * b / a;
        result.count = 1;
        return result;
    }
    // This form avoids cancellation for one of the two roots.
    const float q = -0.5f * (b + std::copysign(root, b));
    result.values[0] = q / a;
    result.values[1] = c / q;
    result.count = 2;
    SortRoots(result);
    return result;
}

inline PolynomialRoots SolveCubic(float a, float b, float c, float d) noexcept {
    if (std::abs(a) <= Epsilon) return SolveQuadratic(b, c, d);
    PolynomialRoots result{};
    if (!IsFinite(a) || !IsFinite(b) || !IsFinite(c) || !IsFinite(d)) return result;
    const float invA = 1.0f / a;
    const float bb = b * invA;
    const float p = c * invA - bb * bb / 3.0f;
    const float q = 2.0f * bb * bb * bb / 27.0f - bb * c * invA / 3.0f + d * invA;
    const float discriminant = q * q * 0.25f + p * p * p / 27.0f;
    if (!IsFinite(discriminant)) return result;
    const float offset = bb / 3.0f;
    if (discriminant > Epsilon) {
        const float root = std::sqrt(discriminant);
        result.values[0] = std::cbrt(-q * 0.5f + root) + std::cbrt(-q * 0.5f - root) - offset;
        result.count = 1;
    } else if (std::abs(discriminant) <= Epsilon) {
        const float first = std::cbrt(-q * 0.5f);
        result.values[0] = 2.0f * first - offset;
        result.values[1] = -first - offset;
        result.count = std::abs(result.values[0] - result.values[1]) <= 1.0e-5f ? 1 : 2;
        SortRoots(result);
    } else {
        const float radius = 2.0f * std::sqrt(std::max(-p / 3.0f, 0.0f));
        const float cosine = std::clamp((-q * 0.5f) / std::max(radius * radius * radius / 8.0f, Epsilon), -1.0f, 1.0f);
        const float angle = std::acos(cosine) / 3.0f;
        result.values[0] = radius * std::cos(angle) - offset;
        result.values[1] = radius * std::cos(angle - TwoPi / 3.0f) - offset;
        result.values[2] = radius * std::cos(angle - 2.0f * TwoPi / 3.0f) - offset;
        result.count = 3;
        SortRoots(result);
    }
    return result;
}

inline bool IsPowerOfTwo(std::size_t value) noexcept { return value != 0 && (value & (value - 1)) == 0; }

// In-place radix-2 Cooley-Tukey FFT. The transform is normalized on inverse.
inline bool FastFourierTransform(ArrayView<std::complex<float>> samples, bool inverse = false) noexcept {
    if (!IsPowerOfTwo(samples.size())) return false;
    const std::size_t count = samples.size();
    for (std::size_t i = 1, j = 0; i < count; ++i) {
        std::size_t bit = count >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(samples[i], samples[j]);
    }
    for (std::size_t length = 2; length <= count; length <<= 1) {
        const float angle = (inverse ? TwoPi : -TwoPi) / static_cast<float>(length);
        const std::complex<float> step{std::cos(angle), std::sin(angle)};
        for (std::size_t start = 0; start < count; start += length) {
            std::complex<float> factor{1.0f, 0.0f};
            const std::size_t half = length / 2;
            for (std::size_t index = 0; index < half; ++index) {
                const auto even = samples[start + index];
                const auto odd = factor * samples[start + index + half];
                samples[start + index] = even + odd;
                samples[start + index + half] = even - odd;
                factor *= step;
            }
        }
    }
    if (inverse) {
        const float scale = 1.0f / static_cast<float>(count);
        for (std::size_t index = 0; index < count; ++index) samples[index] *= scale;
    }
    return true;
}

inline float Halton(std::uint32_t index, std::uint32_t base) noexcept {
    if (base < 2) return 0.0f;
    float result = 0.0f;
    float fraction = 1.0f / static_cast<float>(base);
    while (index != 0) {
        result += static_cast<float>(index % base) * fraction;
        index /= base;
        fraction /= static_cast<float>(base);
    }
    return result;
}

inline Vec2 Halton2D(std::uint32_t index, std::uint32_t baseX = 2, std::uint32_t baseY = 3) noexcept {
    return {Halton(index, baseX), Halton(index, baseY)};
}

class Pcg32 {
    std::uint64_t state_{0x853c49e6748fea9bULL};
    std::uint64_t increment_{0xda3e39cb94b95bdbULL};

public:
    explicit Pcg32(std::uint64_t seed = 0x4d595df4d0f33173ULL, std::uint64_t sequence = 1) noexcept {
        seed_state(seed, sequence);
    }
    void seed_state(std::uint64_t seed, std::uint64_t sequence = 1) noexcept {
        state_ = 0;
        increment_ = (sequence << 1u) | 1u;
        next_u32();
        state_ += seed;
        next_u32();
    }
    std::uint32_t next_u32() noexcept {
        const std::uint64_t oldState = state_;
        state_ = oldState * 6364136223846793005ULL + increment_;
        const std::uint32_t xorshifted = static_cast<std::uint32_t>(((oldState >> 18u) ^ oldState) >> 27u);
        const std::uint32_t rotation = static_cast<std::uint32_t>(oldState >> 59u);
        return (xorshifted >> rotation) | (xorshifted << ((-rotation) & 31));
    }
    float next_float() noexcept { return static_cast<float>(next_u32() >> 8u) * (1.0f / 16777216.0f); }
    Vec3 next_unit_vector() noexcept {
        const float z = next_float() * 2.0f - 1.0f;
        const float angle = next_float() * TwoPi;
        const float radius = std::sqrt(std::max(0.0f, 1.0f - z * z));
        return {radius * std::cos(angle), radius * std::sin(angle), z};
    }
};

template<class Function>
inline float SimpsonIntegrate(Function&& function, float lower, float upper,
                              std::size_t intervals = 64) noexcept {
    if (!IsFinite(lower) || !IsFinite(upper) || intervals == 0) return 0.0f;
    if ((intervals & 1u) != 0) ++intervals;
    const float step = (upper - lower) / static_cast<float>(intervals);
    float sum = function(lower) + function(upper);
    for (std::size_t index = 1; index < intervals; ++index)
        sum += function(lower + step * static_cast<float>(index)) * (index & 1u ? 4.0f : 2.0f);
    return sum * step / 3.0f;
}

class DenseMatrix {
    std::size_t rows_{0};
    std::size_t columns_{0};
    std::vector<float> values_;

public:
    DenseMatrix() = default;
    DenseMatrix(std::size_t rows, std::size_t columns, float value = 0.0f)
        : rows_(rows), columns_(columns), values_(rows * columns, value) {}
    std::size_t rows() const noexcept { return rows_; }
    std::size_t columns() const noexcept { return columns_; }
    float& operator()(std::size_t row, std::size_t column) noexcept { return values_[row * columns_ + column]; }
    float operator()(std::size_t row, std::size_t column) const noexcept { return values_[row * columns_ + column]; }
    void fill(float value) noexcept { std::fill(values_.begin(), values_.end(), value); }
};

inline bool SolveLinearSystem(DenseMatrix matrix, ConstArrayView<float> rightHandSide,
                              ArrayView<float> result, float pivotTolerance = 1.0e-7f) noexcept {
    const std::size_t dimension = matrix.rows();
    if (dimension == 0 || matrix.columns() != dimension || rightHandSide.size() != dimension || result.size() != dimension ||
        !IsFinite(pivotTolerance) || pivotTolerance <= 0.0f) return false;
    std::vector<float> augmented(dimension * (dimension + 1), 0.0f);
    for (std::size_t row = 0; row < dimension; ++row) {
        for (std::size_t column = 0; column < dimension; ++column) {
            augmented[row * (dimension + 1) + column] = matrix(row, column);
            if (!IsFinite(augmented[row * (dimension + 1) + column])) return false;
        }
        augmented[row * (dimension + 1) + dimension] = rightHandSide[row];
        if (!IsFinite(rightHandSide[row])) return false;
    }
    for (std::size_t column = 0; column < dimension; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < dimension; ++row)
            if (std::abs(augmented[row * (dimension + 1) + column]) > std::abs(augmented[pivot * (dimension + 1) + column])) pivot = row;
        const float pivotValue = augmented[pivot * (dimension + 1) + column];
        if (!IsFinite(pivotValue) || std::abs(pivotValue) <= pivotTolerance) return false;
        if (pivot != column) for (std::size_t item = column; item <= dimension; ++item)
            std::swap(augmented[pivot * (dimension + 1) + item], augmented[column * (dimension + 1) + item]);
        const float divisor = augmented[column * (dimension + 1) + column];
        for (std::size_t item = column; item <= dimension; ++item) augmented[column * (dimension + 1) + item] /= divisor;
        for (std::size_t row = 0; row < dimension; ++row) if (row != column) {
            const float factor = augmented[row * (dimension + 1) + column];
            for (std::size_t item = column; item <= dimension; ++item) augmented[row * (dimension + 1) + item] -= factor * augmented[column * (dimension + 1) + item];
        }
    }
    for (std::size_t row = 0; row < dimension; ++row) {
        result[row] = augmented[row * (dimension + 1) + dimension];
        if (!IsFinite(result[row])) return false;
    }
    return true;
}

}
