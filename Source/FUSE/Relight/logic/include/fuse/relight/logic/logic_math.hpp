/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/util/util_matrix.h@0867d3c (decomposeMatrix) and the operator set of
// src/util/util_vector.h@0867d3c that the graph components rely on.
//
// FUSE Relight RL-3.5: the small float math of the Logic graph runtime (docs/plans/FUSE_REMIX_PORT_PLAN.md,
// Wave R3). The vector types reproduce exactly the operators upstream's Vector2/3/4 offer, because the flexible
// components (Add, Multiply, ...) instantiate one variant per *syntactically valid* operand pair:
//   vector + vector, vector - vector, vector * vector, vector / vector   (same size, per component)
//   vector * float, vector / float, float * vector                        (scalar on the allowed side only)
// The splat constructor is explicit, so float + vector does not compile, as upstream.
//
// Matrix4 keeps DXVK's layout: data[i] is the i-th basis row of a D3D (row-vector) matrix, so the translation is
// data[3].xyz and FUSE's row-major D3D matrices (m[12..14] = translation) map element for element.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace fuse::relight::logic {

inline constexpr float kPi = 3.14159265358979323846f;

struct Vector2 {
    float x = 0.0f, y = 0.0f;
    constexpr Vector2() = default;
    constexpr explicit Vector2(float s) : x(s), y(s) {}
    constexpr Vector2(float x_, float y_) : x(x_), y(y_) {}
    float& operator[](std::size_t i) { return i == 0 ? x : y; }
    float operator[](std::size_t i) const { return i == 0 ? x : y; }
    Vector2 operator-() const { return {-x, -y}; }
    Vector2 operator+(const Vector2& o) const { return {x + o.x, y + o.y}; }
    Vector2 operator-(const Vector2& o) const { return {x - o.x, y - o.y}; }
    Vector2 operator*(const Vector2& o) const { return {x * o.x, y * o.y}; }
    Vector2 operator/(const Vector2& o) const { return {x / o.x, y / o.y}; }
    Vector2 operator*(float s) const { return {x * s, y * s}; }
    Vector2 operator/(float s) const { return {x / s, y / s}; }
    bool operator==(const Vector2& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Vector2& o) const { return !(*this == o); }
    /// Upstream's (all components) ordering; only needed so std::variant comparisons compile.
    bool operator<(const Vector2& o) const { return x < o.x && y < o.y; }
};

struct Vector3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    constexpr Vector3() = default;
    constexpr explicit Vector3(float s) : x(s), y(s), z(s) {}
    constexpr Vector3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    float& operator[](std::size_t i) { return i == 0 ? x : (i == 1 ? y : z); }
    float operator[](std::size_t i) const { return i == 0 ? x : (i == 1 ? y : z); }
    Vector3 operator-() const { return {-x, -y, -z}; }
    Vector3 operator+(const Vector3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vector3 operator-(const Vector3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vector3 operator*(const Vector3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    Vector3 operator/(const Vector3& o) const { return {x / o.x, y / o.y, z / o.z}; }
    Vector3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vector3 operator/(float s) const { return {x / s, y / s, z / s}; }
    bool operator==(const Vector3& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Vector3& o) const { return !(*this == o); }
    bool operator<(const Vector3& o) const { return x < o.x && y < o.y && z < o.z; }
};

struct Vector4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    constexpr Vector4() = default;
    constexpr explicit Vector4(float s) : x(s), y(s), z(s), w(s) {}
    constexpr Vector4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vector4(const Vector3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    float& operator[](std::size_t i) { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    float operator[](std::size_t i) const { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    Vector3 xyz() const { return {x, y, z}; }
    Vector4 operator-() const { return {-x, -y, -z, -w}; }
    Vector4 operator+(const Vector4& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    Vector4 operator-(const Vector4& o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    Vector4 operator*(const Vector4& o) const { return {x * o.x, y * o.y, z * o.z, w * o.w}; }
    Vector4 operator/(const Vector4& o) const { return {x / o.x, y / o.y, z / o.z, w / o.w}; }
    Vector4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
    Vector4 operator/(float s) const { return {x / s, y / s, z / s, w / s}; }
    bool operator==(const Vector4& o) const { return x == o.x && y == o.y && z == o.z && w == o.w; }
    bool operator!=(const Vector4& o) const { return !(*this == o); }
    bool operator<(const Vector4& o) const { return x < o.x && y < o.y && z < o.z && w < o.w; }
};

inline Vector2 operator*(float s, const Vector2& v) { return v * s; }
inline Vector3 operator*(float s, const Vector3& v) { return v * s; }
inline Vector4 operator*(float s, const Vector4& v) { return v * s; }

inline float dot(const Vector2& a, const Vector2& b) { return a.x * b.x + a.y * b.y; }
inline float dot(const Vector3& a, const Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float dot(const Vector4& a, const Vector4& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
template <typename V>
float lengthSqr(const V& v) {
    return dot(v, v);
}
template <typename V>
float length(const V& v) {
    return std::sqrt(lengthSqr(v));
}
inline float length(float v) { return std::fabs(v); }
inline Vector3 normalize(const Vector3& v) { return v / length(v); }
inline Vector3 max(const Vector3& a, const Vector3& b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}
inline Vector3 clamp(const Vector3& v, const Vector3& lo, const Vector3& hi) {
    return {std::clamp(v.x, lo.x, hi.x), std::clamp(v.y, lo.y, hi.y), std::clamp(v.z, lo.z, hi.z)};
}
/// Upstream util_math clamp (no ordering assertion, unlike std::clamp).
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
/// Upstream util_math lerp: a + t * (b - a).
template <typename T>
T lerp(const T& a, const T& b, float t) {
    return a + t * (b - a);
}

struct Matrix4 {
    std::array<Vector4, 4> data{Vector4(1, 0, 0, 0), Vector4(0, 1, 0, 0), Vector4(0, 0, 1, 0), Vector4(0, 0, 0, 1)};
    Vector4& operator[](std::size_t i) { return data[i]; }
    const Vector4& operator[](std::size_t i) const { return data[i]; }
    /// From 16 row-major D3D values (m[12..14] = translation).
    static Matrix4 fromRowMajor(const float* m);
    static Matrix4 fromRowMajor(const double* m);
};

/// Matrix * column vector in DXVK's convention (= row vector * D3D matrix): sum_i data[i] * v[i].
Vector4 transform(const Matrix4& m, const Vector4& v);
/// DXVK Matrix4 product a * b (b applied first to a column vector).
Matrix4 multiply(const Matrix4& a, const Matrix4& b);
/// General inverse (identity when singular).
Matrix4 inverse(const Matrix4& m);

/// Upstream decomposeMatrix: translation, rotation quaternion (x, y, z, w) and per-axis scale.
void decomposeMatrix(const Matrix4& transform, Vector3& position, Vector4& rotation, Vector3& scale);

struct AxisAlignedBoundingBox {
    Vector3 minPos{1.0f, 1.0f, 1.0f};
    Vector3 maxPos{-1.0f, -1.0f, -1.0f};
    bool isValid() const { return minPos.x <= maxPos.x && minPos.y <= maxPos.y && minPos.z <= maxPos.z; }
    Vector3 centroid() const { return (minPos + maxPos) * 0.5f; }
    Vector3 getTransformedCentroid(const Matrix4& objectToWorld) const {
        return transform(objectToWorld, Vector4(centroid(), 1.0f)).xyz();
    }
};

} // namespace fuse::relight::logic
