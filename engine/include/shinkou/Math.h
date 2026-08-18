#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace shinkou::math {

constexpr float Pi = 3.14159265358979323846f;
constexpr float Epsilon = 1.0e-6f;

inline bool IsFinite(float value) noexcept { return std::isfinite(value) != 0; }
inline float Clamp01(float value) noexcept { return std::clamp(value, 0.0f, 1.0f); }
inline float Lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }
inline float Radians(float degrees) noexcept { return degrees * (Pi / 180.0f); }
inline float Degrees(float radians) noexcept { return radians * (180.0f / Pi); }
inline float Wrap(float value, float minimum, float maximum) noexcept {
    const float range = maximum - minimum;
    if (!(range > Epsilon) || !IsFinite(range) || !IsFinite(value)) return minimum;
    const float wrapped = std::fmod(value - minimum, range);
    return (wrapped < 0.0f ? wrapped + range : wrapped) + minimum;
}
inline float MoveTowards(float current, float target, float maxDelta) noexcept {
    const float delta = target - current;
    if (std::abs(delta) <= std::max(maxDelta, 0.0f)) return target;
    return current + (delta > 0.0f ? 1.0f : -1.0f) * std::max(maxDelta, 0.0f);
}
inline float SmoothStep(float edge0, float edge1, float value) noexcept {
    const float t = Clamp01((value - edge0) / (std::abs(edge1 - edge0) > Epsilon ? edge1 - edge0 : 1.0f));
    return t * t * (3.0f - 2.0f * t);
}

template<class T>
struct ArrayView {
    T* pointer{nullptr};
    std::size_t count{0};

    constexpr T* data() const noexcept { return pointer; }
    constexpr std::size_t size() const noexcept { return count; }
    constexpr bool empty() const noexcept { return count == 0; }
    constexpr T& operator[](std::size_t index) const noexcept { return pointer[index]; }
    constexpr T* begin() const noexcept { return pointer; }
    constexpr T* end() const noexcept { return pointer ? pointer + count : nullptr; }
};

template<class T, std::size_t N>
constexpr ArrayView<T> MakeArrayView(T (&array)[N]) noexcept { return {array, N}; }

template<class T>
using ConstArrayView = ArrayView<const T>;

template<class T>
inline bool IsFinite(const T& value) noexcept {
    static_assert(std::is_floating_point_v<T>, "IsFinite only supports floating point scalars");
    return std::isfinite(value) != 0;
}

struct Vec2 {
    float x{0};
    float y{0};

    Vec2 operator+ (Vec2 rhs) const noexcept { return {x + rhs.x, y + rhs.y}; }
    Vec2 operator- (Vec2 rhs) const noexcept { return {x - rhs.x, y - rhs.y}; }
    Vec2 operator- () const noexcept { return {-x, -y}; }
    Vec2 operator* (float s) const noexcept { return {x * s, y * s}; }
    Vec2 operator/ (float s) const noexcept { return std::abs(s) > Epsilon ? Vec2{x / s, y / s} : Vec2{}; }
    Vec2& operator+=(Vec2 rhs) noexcept { x += rhs.x; y += rhs.y; return *this; }
    Vec2& operator-=(Vec2 rhs) noexcept { x -= rhs.x; y -= rhs.y; return *this; }
    Vec2& operator*=(float s) noexcept { x *= s; y *= s; return *this; }
    float& operator[](std::size_t index) noexcept { return index == 0 ? x : y; }
    float operator[](std::size_t index) const noexcept { return index == 0 ? x : y; }
};

inline Vec2 operator*(float s, Vec2 value) noexcept { return value * s; }
inline float Dot(Vec2 a, Vec2 b) noexcept { return a.x * b.x + a.y * b.y; }
inline float LengthSquared(Vec2 value) noexcept { return Dot(value, value); }
inline float Length(Vec2 value) noexcept { return std::hypot(value.x, value.y); }
inline Vec2 Normalize(Vec2 value) noexcept {
    const float length = Length(value);
    return IsFinite(length) && length > Epsilon ? value / length : Vec2{};
}
inline Vec2 Lerp(Vec2 a, Vec2 b, float t) noexcept { return a + (b - a) * t; }

struct alignas(16) Vec4 {
    float x{0};
    float y{0};
    float z{0};
    float w{0};

    Vec4 operator+(Vec4 rhs) const noexcept { return {x + rhs.x, y + rhs.y, z + rhs.z, w + rhs.w}; }
    Vec4 operator-(Vec4 rhs) const noexcept { return {x - rhs.x, y - rhs.y, z - rhs.z, w - rhs.w}; }
    Vec4 operator-() const noexcept { return {-x, -y, -z, -w}; }
    Vec4 operator*(float scalar) const noexcept { return {x * scalar, y * scalar, z * scalar, w * scalar}; }
    Vec4& operator+=(Vec4 rhs) noexcept { x += rhs.x; y += rhs.y; z += rhs.z; w += rhs.w; return *this; }
    Vec4& operator-=(Vec4 rhs) noexcept { x -= rhs.x; y -= rhs.y; z -= rhs.z; w -= rhs.w; return *this; }
    Vec4& operator*=(float scalar) noexcept { x *= scalar; y *= scalar; z *= scalar; w *= scalar; return *this; }
    float& operator[](std::size_t index) noexcept { return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w)); }
    float operator[](std::size_t index) const noexcept { return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w)); }
};

inline Vec4 operator*(float scalar, Vec4 value) noexcept { return value * scalar; }
inline float Dot(Vec4 a, Vec4 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline float LengthSquared(Vec4 value) noexcept { return Dot(value, value); }
inline float Length(Vec4 value) noexcept { return std::hypot(std::hypot(value.x, value.y), std::hypot(value.z, value.w)); }
inline Vec4 Normalize(Vec4 value) noexcept {
    const float length = Length(value);
    return IsFinite(length) && length > Epsilon ? value * (1.0f / length) : Vec4{};
}
inline Vec4 Lerp(Vec4 a, Vec4 b, float t) noexcept { return a + (b - a) * t; }

struct Vec3 {
    float x{0};
    float y{0};
    float z{0};

    Vec3 operator+ (Vec3 rhs) const noexcept { return {x + rhs.x, y + rhs.y, z + rhs.z}; }
    Vec3 operator- (Vec3 rhs) const noexcept { return {x - rhs.x, y - rhs.y, z - rhs.z}; }
    Vec3 operator- () const noexcept { return {-x, -y, -z}; }
    Vec3 operator* (float s) const noexcept { return {x * s, y * s, z * s}; }
    Vec3 operator* (Vec3 rhs) const noexcept { return {x * rhs.x, y * rhs.y, z * rhs.z}; }
    Vec3 operator/ (float s) const noexcept { return std::abs(s) > Epsilon ? Vec3{x / s, y / s, z / s} : Vec3{}; }
    Vec3& operator+=(Vec3 rhs) noexcept { x += rhs.x; y += rhs.y; z += rhs.z; return *this; }
    Vec3& operator-=(Vec3 rhs) noexcept { x -= rhs.x; y -= rhs.y; z -= rhs.z; return *this; }
    Vec3& operator*=(float s) noexcept { x *= s; y *= s; z *= s; return *this; }
    float& operator[](std::size_t index) noexcept { return index == 0 ? x : (index == 1 ? y : z); }
    float operator[](std::size_t index) const noexcept { return index == 0 ? x : (index == 1 ? y : z); }
};

inline Vec3 operator*(float s, Vec3 value) noexcept { return value * s; }
inline float Dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float LengthSquared(Vec3 value) noexcept { return Dot(value, value); }
inline float Length(Vec3 value) noexcept { return std::hypot(std::hypot(value.x, value.y), value.z); }
inline Vec3 Normalize(Vec3 value) noexcept {
    const float length = Length(value);
    return IsFinite(length) && length > Epsilon ? value / length : Vec3{};
}
inline Vec3 Lerp(Vec3 a, Vec3 b, float t) noexcept { return a + (b - a) * t; }

struct Quat {
    float x{0};
    float y{0};
    float z{0};
    float w{1};

    static constexpr Quat Identity() noexcept { return {}; }
    Quat operator-() const noexcept { return {-x, -y, -z, -w}; }
    Quat operator*(float s) const noexcept { return {x * s, y * s, z * s, w * s}; }
    Quat operator+(Quat rhs) const noexcept { return {x + rhs.x, y + rhs.y, z + rhs.z, w + rhs.w}; }
    Quat operator-(Quat rhs) const noexcept { return {x - rhs.x, y - rhs.y, z - rhs.z, w - rhs.w}; }
    Quat& operator*=(float s) noexcept { x *= s; y *= s; z *= s; w *= s; return *this; }
};

inline Quat operator*(float s, Quat value) noexcept { return value * s; }
inline float Dot(Quat a, Quat b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline float LengthSquared(Quat value) noexcept { return Dot(value, value); }
inline Quat Normalize(Quat value) noexcept {
    const float length = std::hypot(std::hypot(value.x, value.y), std::hypot(value.z, value.w));
    return IsFinite(length) && length > Epsilon ? value * (1.0f / length) : Quat::Identity();
}
inline Quat Conjugate(Quat value) noexcept { return {-value.x, -value.y, -value.z, value.w}; }
inline Quat Inverse(Quat value) noexcept {
    const float lengthSquared = LengthSquared(value);
    return lengthSquared > Epsilon ? Conjugate(value) * (1.0f / lengthSquared) : Quat::Identity();
}
inline Quat Multiply(Quat lhs, Quat rhs) noexcept {
    return {lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
            lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
            lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
            lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z};
}
inline Quat operator*(Quat lhs, Quat rhs) noexcept { return Multiply(lhs, rhs); }
inline Vec3 Rotate(Quat rotation, Vec3 value) noexcept {
    const Vec3 q{rotation.x, rotation.y, rotation.z};
    const Vec3 t = Cross(q, value) * 2.0f;
    return value + t * rotation.w + Cross(q, t);
}
inline Quat FromAxisAngle(Vec3 axis, float radians) noexcept {
    const Vec3 normalizedAxis = Normalize(axis);
    const float half = radians * 0.5f;
    const float sine = std::sin(half);
    return Normalize(Quat{normalizedAxis.x * sine, normalizedAxis.y * sine, normalizedAxis.z * sine, std::cos(half)});
}
inline Quat Nlerp(Quat a, Quat b, float t) noexcept {
    if (Dot(a, b) < 0.0f) b = -b;
    return Normalize(a + (b - a) * Clamp01(t));
}
inline Quat Slerp(Quat a, Quat b, float t) noexcept {
    a = Normalize(a);
    b = Normalize(b);
    float cosine = Dot(a, b);
    if (cosine < 0.0f) { b = -b; cosine = -cosine; }
    if (cosine > 0.9995f) return Nlerp(a, b, t);
    const float angle = std::acos(std::clamp(cosine, -1.0f, 1.0f));
    const float sine = std::sin(angle);
    if (std::abs(sine) <= Epsilon) return a;
    const float alpha = std::sin((1.0f - Clamp01(t)) * angle) / sine;
    const float beta = std::sin(Clamp01(t) * angle) / sine;
    return Normalize(a * alpha + b * beta);
}
inline Quat FromEulerXYZ(Vec3 radians) noexcept {
    const Quat x = FromAxisAngle({1.0f, 0.0f, 0.0f}, radians.x);
    const Quat y = FromAxisAngle({0.0f, 1.0f, 0.0f}, radians.y);
    const Quat z = FromAxisAngle({0.0f, 0.0f, 1.0f}, radians.z);
    return Normalize(x * y * z);
}
inline Vec3 ToEulerXYZ(Quat value) noexcept {
    value = Normalize(value);
    const float sinPitch = 2.0f * (value.w * value.x - value.y * value.z);
    const float pitch = std::abs(sinPitch) >= 1.0f ? std::copysign(Pi * 0.5f, sinPitch) : std::asin(sinPitch);
    const float yaw = std::atan2(2.0f * (value.w * value.y + value.z * value.x),
                                 1.0f - 2.0f * (value.x * value.x + value.y * value.y));
    const float roll = std::atan2(2.0f * (value.w * value.z + value.x * value.y),
                                  1.0f - 2.0f * (value.x * value.x + value.z * value.z));
    return {pitch, yaw, roll};
}

struct Mat2 {
    float m[4]{};
    static Mat2 Identity() noexcept { Mat2 result{}; result.m[0] = result.m[3] = 1.0f; return result; }
    float& operator()(int row, int column) noexcept { return m[column * 2 + row]; }
    float operator()(int row, int column) const noexcept { return m[column * 2 + row]; }
};

struct Mat3 {
    float m[9]{};
    static Mat3 Identity() noexcept { Mat3 result{}; result.m[0] = result.m[4] = result.m[8] = 1.0f; return result; }
    float& operator()(int row, int column) noexcept { return m[column * 3 + row]; }
    float operator()(int row, int column) const noexcept { return m[column * 3 + row]; }
};

struct alignas(16) Mat4 {
    // Column-major storage, matching the renderer's shader upload convention.
    float m[16]{};

    static Mat4 Identity() noexcept {
        Mat4 result{};
        result.m[0] = result.m[5] = result.m[10] = result.m[15] = 1.0f;
        return result;
    }
    float& operator()(int row, int column) noexcept { return m[column * 4 + row]; }
    float operator()(int row, int column) const noexcept { return m[column * 4 + row]; }
};

inline Mat2 Multiply(const Mat2& lhs, const Mat2& rhs) noexcept {
    Mat2 result{};
    for (int column = 0; column < 2; ++column) for (int row = 0; row < 2; ++row)
        for (int index = 0; index < 2; ++index) result(row, column) += lhs(row, index) * rhs(index, column);
    return result;
}
inline Mat3 Multiply(const Mat3& lhs, const Mat3& rhs) noexcept {
    Mat3 result{};
    for (int column = 0; column < 3; ++column) for (int row = 0; row < 3; ++row)
        for (int index = 0; index < 3; ++index) result(row, column) += lhs(row, index) * rhs(index, column);
    return result;
}
inline Mat2 operator*(const Mat2& lhs, const Mat2& rhs) noexcept { return Multiply(lhs, rhs); }
inline Mat3 operator*(const Mat3& lhs, const Mat3& rhs) noexcept { return Multiply(lhs, rhs); }
inline float Determinant(const Mat2& value) noexcept { return value.m[0] * value.m[3] - value.m[2] * value.m[1]; }
inline float Determinant(const Mat3& value) noexcept {
    return value.m[0] * (value.m[4] * value.m[8] - value.m[7] * value.m[5])
         - value.m[3] * (value.m[1] * value.m[8] - value.m[7] * value.m[2])
         + value.m[6] * (value.m[1] * value.m[5] - value.m[4] * value.m[2]);
}
inline bool Inverse(const Mat2& value, Mat2& result) noexcept {
    const float determinant = Determinant(value);
    if (!IsFinite(determinant) || std::abs(determinant) <= Epsilon) return false;
    const float inverse = 1.0f / determinant;
    result.m[0] = value.m[3] * inverse; result.m[1] = -value.m[1] * inverse;
    result.m[2] = -value.m[2] * inverse; result.m[3] = value.m[0] * inverse;
    return true;
}
inline bool Inverse(const Mat3& value, Mat3& result) noexcept {
    const float determinant = Determinant(value);
    if (!IsFinite(determinant) || std::abs(determinant) <= Epsilon) return false;
    const float inverse = 1.0f / determinant;
    result.m[0] = (value.m[4] * value.m[8] - value.m[7] * value.m[5]) * inverse;
    result.m[1] = (value.m[7] * value.m[2] - value.m[1] * value.m[8]) * inverse;
    result.m[2] = (value.m[1] * value.m[5] - value.m[4] * value.m[2]) * inverse;
    result.m[3] = (value.m[6] * value.m[5] - value.m[3] * value.m[8]) * inverse;
    result.m[4] = (value.m[0] * value.m[8] - value.m[6] * value.m[2]) * inverse;
    result.m[5] = (value.m[3] * value.m[2] - value.m[0] * value.m[5]) * inverse;
    result.m[6] = (value.m[3] * value.m[7] - value.m[6] * value.m[4]) * inverse;
    result.m[7] = (value.m[6] * value.m[1] - value.m[0] * value.m[7]) * inverse;
    result.m[8] = (value.m[0] * value.m[4] - value.m[3] * value.m[1]) * inverse;
    return true;
}

inline Mat4 Multiply(const Mat4& lhs, const Mat4& rhs) noexcept {
    Mat4 result{};
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            for (int index = 0; index < 4; ++index)
                result(row, column) += lhs(row, index) * rhs(index, column);
    return result;
}
inline Mat4 operator*(const Mat4& lhs, const Mat4& rhs) noexcept { return Multiply(lhs, rhs); }
inline Mat4 Transpose(const Mat4& value) noexcept {
    Mat4 result{};
    for (int row = 0; row < 4; ++row) for (int column = 0; column < 4; ++column) result(row, column) = value(column, row);
    return result;
}
inline Mat4 Translation(Vec3 value) noexcept {
    auto result = Mat4::Identity();
    result.m[12] = value.x; result.m[13] = value.y; result.m[14] = value.z;
    return result;
}
inline Mat4 Scale(Vec3 value) noexcept {
    auto result = Mat4::Identity();
    result.m[0] = value.x; result.m[5] = value.y; result.m[10] = value.z;
    return result;
}
inline Mat4 Rotation(Quat value) noexcept {
    value = Normalize(value);
    const float xx = value.x * value.x, yy = value.y * value.y, zz = value.z * value.z;
    const float xy = value.x * value.y, xz = value.x * value.z, yz = value.y * value.z;
    const float wx = value.w * value.x, wy = value.w * value.y, wz = value.w * value.z;
    Mat4 result = Mat4::Identity();
    result.m[0] = 1.0f - 2.0f * (yy + zz); result.m[1] = 2.0f * (xy + wz); result.m[2] = 2.0f * (xz - wy);
    result.m[4] = 2.0f * (xy - wz); result.m[5] = 1.0f - 2.0f * (xx + zz); result.m[6] = 2.0f * (yz + wx);
    result.m[8] = 2.0f * (xz + wy); result.m[9] = 2.0f * (yz - wx); result.m[10] = 1.0f - 2.0f * (xx + yy);
    return result;
}
inline Vec3 TransformPoint(const Mat4& matrix, Vec3 point) noexcept {
    const float w = point.x * matrix.m[3] + point.y * matrix.m[7] + point.z * matrix.m[11] + matrix.m[15];
    const float inverseW = std::abs(w) > Epsilon ? 1.0f / w : 1.0f;
    return {(point.x * matrix.m[0] + point.y * matrix.m[4] + point.z * matrix.m[8] + matrix.m[12]) * inverseW,
            (point.x * matrix.m[1] + point.y * matrix.m[5] + point.z * matrix.m[9] + matrix.m[13]) * inverseW,
            (point.x * matrix.m[2] + point.y * matrix.m[6] + point.z * matrix.m[10] + matrix.m[14]) * inverseW};
}
inline Vec3 TransformVector(const Mat4& matrix, Vec3 vector) noexcept {
    return {vector.x * matrix.m[0] + vector.y * matrix.m[4] + vector.z * matrix.m[8],
            vector.x * matrix.m[1] + vector.y * matrix.m[5] + vector.z * matrix.m[9],
            vector.x * matrix.m[2] + vector.y * matrix.m[6] + vector.z * matrix.m[10]};
}
inline Vec4 TransformVector4(const Mat4& matrix, Vec4 value) noexcept {
    return {value.x * matrix.m[0] + value.y * matrix.m[4] + value.z * matrix.m[8] + value.w * matrix.m[12],
            value.x * matrix.m[1] + value.y * matrix.m[5] + value.z * matrix.m[9] + value.w * matrix.m[13],
            value.x * matrix.m[2] + value.y * matrix.m[6] + value.z * matrix.m[10] + value.w * matrix.m[14],
            value.x * matrix.m[3] + value.y * matrix.m[7] + value.z * matrix.m[11] + value.w * matrix.m[15]};
}
inline bool Inverse(const Mat4& value, Mat4& result) noexcept {
    float augmented[4][8]{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            augmented[row][column] = value(row, column);
            if (!IsFinite(augmented[row][column])) return false;
        }
        augmented[row][row + 4] = 1.0f;
    }
    for (int column = 0; column < 4; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row)
            if (std::abs(augmented[row][column]) > std::abs(augmented[pivot][column])) pivot = row;
        if (!IsFinite(augmented[pivot][column]) || std::abs(augmented[pivot][column]) <= Epsilon) return false;
        if (pivot != column) for (int item = 0; item < 8; ++item) std::swap(augmented[pivot][item], augmented[column][item]);
        const float divisor = augmented[column][column];
        for (int item = 0; item < 8; ++item) augmented[column][item] /= divisor;
        for (int row = 0; row < 4; ++row) if (row != column) {
            const float factor = augmented[row][column];
            for (int item = 0; item < 8; ++item) augmented[row][item] -= factor * augmented[column][item];
        }
    }
    result = {};
    for (int row = 0; row < 4; ++row) for (int column = 0; column < 4; ++column) result(row, column) = augmented[row][column + 4];
    return true;
}
inline Mat4 InverseOrIdentity(const Mat4& value) noexcept {
    Mat4 result{};
    return Inverse(value, result) ? result : Mat4::Identity();
}
inline Mat4 Perspective(float verticalFieldOfView, float aspect, float nearPlane, float farPlane) noexcept {
    if (!IsFinite(verticalFieldOfView) || !IsFinite(aspect) || !IsFinite(nearPlane) || !IsFinite(farPlane) ||
        verticalFieldOfView <= Epsilon || verticalFieldOfView >= Pi - Epsilon || aspect <= Epsilon || nearPlane < 0.0f || farPlane <= nearPlane) return Mat4::Identity();
    Mat4 result{};
    const float safeAspect = aspect;
    const float safeDepth = farPlane - nearPlane;
    const float tangent = std::tan(verticalFieldOfView * 0.5f);
    const float scaleY = std::abs(tangent) > Epsilon ? 1.0f / tangent : 1.0f;
    result.m[0] = scaleY / safeAspect; result.m[5] = scaleY;
    result.m[10] = farPlane / safeDepth; result.m[11] = 1.0f;
    result.m[14] = -(nearPlane * farPlane) / safeDepth;
    return result;
}
inline Mat4 Orthographic(float left, float right, float bottom, float top, float nearPlane, float farPlane) noexcept {
    if (!IsFinite(left) || !IsFinite(right) || !IsFinite(bottom) || !IsFinite(top) || !IsFinite(nearPlane) || !IsFinite(farPlane) ||
        right - left <= Epsilon || top - bottom <= Epsilon || farPlane - nearPlane <= Epsilon) return Mat4::Identity();
    const float width = right - left, height = top - bottom, depth = farPlane - nearPlane;
    Mat4 result = Mat4::Identity();
    result.m[0] = 2.0f / width; result.m[5] = 2.0f / height; result.m[10] = 1.0f / depth;
    result.m[12] = -(right + left) / width; result.m[13] = -(top + bottom) / height; result.m[14] = -nearPlane / depth;
    return result;
}
inline Mat4 LookAt(Vec3 eye, Vec3 target, Vec3 up = {0.0f, 1.0f, 0.0f}) noexcept {
    const Vec3 forward = Normalize(target - eye);
    const Vec3 safeForward = LengthSquared(forward) > Epsilon ? forward : Vec3{0.0f, 0.0f, 1.0f};
    Vec3 right = Normalize(Cross(up, safeForward));
    if (LengthSquared(right) <= Epsilon) right = Normalize(Cross(Vec3{0.0f, 0.0f, 1.0f}, safeForward));
    const Vec3 correctedUp = Cross(safeForward, right);
    Mat4 result = Mat4::Identity();
    result.m[0] = right.x; result.m[1] = correctedUp.x; result.m[2] = safeForward.x;
    result.m[4] = right.y; result.m[5] = correctedUp.y; result.m[6] = safeForward.y;
    result.m[8] = right.z; result.m[9] = correctedUp.z; result.m[10] = safeForward.z;
    result.m[12] = -Dot(right, eye); result.m[13] = -Dot(correctedUp, eye); result.m[14] = -Dot(safeForward, eye);
    return result;
}

struct Transform {
    Vec3 position{};
    Quat rotation{};
    Vec3 scale{1, 1, 1};
};

inline Transform Blend(Transform a, Transform b, float t) noexcept {
    return {Lerp(a.position, b.position, t), Slerp(a.rotation, b.rotation, t), Lerp(a.scale, b.scale, t)};
}
inline Transform Combine(Transform parent, Transform local) noexcept {
    return {parent.position + Rotate(parent.rotation, local.position * parent.scale),
            parent.rotation * local.rotation, parent.scale * local.scale};
}
inline Mat4 TransformMatrix(const Transform& transform) noexcept {
    return Translation(transform.position) * Rotation(transform.rotation) * Scale(transform.scale);
}
inline bool Decompose(const Mat4& matrix, Transform& result) noexcept {
    for (float value : matrix.m) if (!IsFinite(value)) return false;
    result.position = {matrix.m[12], matrix.m[13], matrix.m[14]};
    const Vec3 x{matrix.m[0], matrix.m[1], matrix.m[2]}, y{matrix.m[4], matrix.m[5], matrix.m[6]}, z{matrix.m[8], matrix.m[9], matrix.m[10]};
    result.scale = {Length(x), Length(y), Length(z)};
    if (!IsFinite(result.scale.x) || !IsFinite(result.scale.y) || !IsFinite(result.scale.z) ||
        result.scale.x <= Epsilon || result.scale.y <= Epsilon || result.scale.z <= Epsilon) return false;
    const Vec3 normalizedX = x / result.scale.x;
    const Vec3 normalizedY = y / result.scale.y;
    const Vec3 normalizedZ = z / result.scale.z;
    if (Dot(Cross(normalizedX, normalizedY), normalizedZ) < 0.0f) {
        if (result.scale.x >= result.scale.y && result.scale.x >= result.scale.z) result.scale.x = -result.scale.x;
        else if (result.scale.y >= result.scale.z) result.scale.y = -result.scale.y;
        else result.scale.z = -result.scale.z;
    }
    Mat4 rotation = matrix;
    for (int i = 0; i < 3; ++i) { rotation.m[i] /= result.scale.x; rotation.m[4 + i] /= result.scale.y; rotation.m[8 + i] /= result.scale.z; }
    const float trace = rotation.m[0] + rotation.m[5] + rotation.m[10];
    if (trace > 0.0f) { const float s = std::sqrt(trace + 1.0f) * 2.0f; result.rotation = {(rotation.m[6] - rotation.m[9]) / s, (rotation.m[8] - rotation.m[2]) / s, (rotation.m[1] - rotation.m[4]) / s, 0.25f * s}; }
    else if (rotation.m[0] > rotation.m[5] && rotation.m[0] > rotation.m[10]) { const float s = std::sqrt(1.0f + rotation.m[0] - rotation.m[5] - rotation.m[10]) * 2.0f; result.rotation = {0.25f * s, (rotation.m[4] + rotation.m[1]) / s, (rotation.m[8] + rotation.m[2]) / s, (rotation.m[6] - rotation.m[9]) / s}; }
    else if (rotation.m[5] > rotation.m[10]) { const float s = std::sqrt(1.0f + rotation.m[5] - rotation.m[0] - rotation.m[10]) * 2.0f; result.rotation = {(rotation.m[4] + rotation.m[1]) / s, 0.25f * s, (rotation.m[9] + rotation.m[6]) / s, (rotation.m[8] - rotation.m[2]) / s}; }
    else { const float s = std::sqrt(1.0f + rotation.m[10] - rotation.m[0] - rotation.m[5]) * 2.0f; result.rotation = {(rotation.m[8] + rotation.m[2]) / s, (rotation.m[9] + rotation.m[6]) / s, 0.25f * s, (rotation.m[1] - rotation.m[4]) / s}; }
    result.rotation = Normalize(result.rotation);
    return true;
}

struct Aabb { Vec3 min{}; Vec3 max{}; };
inline bool Overlaps(Aabb a, Aabb b) noexcept {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y && a.min.z <= b.max.z && a.max.z >= b.min.z;
}
}
