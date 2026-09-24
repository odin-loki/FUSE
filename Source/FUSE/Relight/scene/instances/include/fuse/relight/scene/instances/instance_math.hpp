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
// Ported from dxvk-remix src/util/util_bounding_box.h@0867d3c (AxisAlignedBoundingBox: isValid,
// getCentroid, getTransformedCentroid, unionWith, calculateHash).
// FUSE Relight RL-1.7: the small float vector / matrix helpers of the instance tracker.
//
// Matrices are D3DMATRIX-layout float[16] (row-major, row vectors: p' = p x M, translation in elements
// 12..14), the layout every tap / translate record uses. Remix's Matrix4 (column vectors, data[3] = the
// translation column) has the same memory image, so `transform * Vector4(p, 1)` upstream is
// transformPoint(transform, p) here and `transform[3].xyz()` is translation(transform).
#pragma once

#include <fuse/relight/hash/xxh.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace fuse::relight::scene::instances {

using Mat4f = std::array<float, 16>;

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;

    friend Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    friend Vec3 operator/(Vec3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
    friend bool operator==(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
};

struct Vec4 {
    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
    Vec3 xyz() const { return {x, y, z}; }
};

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float dot(const Vec4& a, const Vec4& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline float lengthSqr(Vec3 v) { return dot(v, v); }
inline float length(Vec3 v) { return std::sqrt(lengthSqr(v)); }
inline float length(const Vec4& v) { return std::sqrt(dot(v, v)); }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 normalize(Vec3 v) { return v / length(v); }

inline Mat4f identityMatrix() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

/// Row-vector product (x, y, z, w) x M (upstream `M * Vector4`).
inline Vec4 transform4(const Mat4f& m, const Vec4& v) {
    return {v.x * m[0] + v.y * m[4] + v.z * m[8] + v.w * m[12], v.x * m[1] + v.y * m[5] + v.z * m[9] + v.w * m[13],
            v.x * m[2] + v.y * m[6] + v.z * m[10] + v.w * m[14], v.x * m[3] + v.y * m[7] + v.z * m[11] + v.w * m[15]};
}
inline Vec3 transformPoint(const Mat4f& m, Vec3 p) { return transform4(m, {p.x, p.y, p.z, 1.f}).xyz(); }
inline Vec3 translation(const Mat4f& m) { return {m[12], m[13], m[14]}; }

/// a x b (row vectors: apply a, then b). Upstream `b * a`.
inline Mat4f multiply(const Mat4f& a, const Mat4f& b) {
    Mat4f r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) {
                s += a[i * 4 + k] * b[k * 4 + j];
            }
            r[i * 4 + j] = s;
        }
    }
    return r;
}

/// memcmp equality (Remix compares transforms bytewise: -0 != +0, NaN payloads distinguish).
inline bool bitwiseEqual(const Mat4f& a, const Mat4f& b) { return std::memcmp(a.data(), b.data(), sizeof(Mat4f)) == 0; }

/// transpose(inverse(Matrix3(m))) as a 3x3 row-major block (RtSurface::normalObjectToWorld).
std::array<float, 9> normalMatrix(const Mat4f& m);

/// AxisAlignedBoundingBox (util_bounding_box.h): empty (invalid) until min <= max on every axis.
struct AxisAlignedBoundingBox {
    Vec3 minPos{3.402823466e38f, 3.402823466e38f, 3.402823466e38f};
    Vec3 maxPos{-3.402823466e38f, -3.402823466e38f, -3.402823466e38f};

    bool isValid() const { return minPos.x <= maxPos.x && minPos.y <= maxPos.y && minPos.z <= maxPos.z; }
    Vec3 getCentroid() const { return (minPos + maxPos) * 0.5f; }
    /// The centroid through `transform`, or the transform's translation when the box is invalid.
    Vec3 getTransformedCentroid(const Mat4f& transform) const {
        return isValid() ? transformPoint(transform, getCentroid()) : translation(transform);
    }
    void unionWith(const AxisAlignedBoundingBox& o) {
        minPos = {std::min(minPos.x, o.minPos.x), std::min(minPos.y, o.minPos.y), std::min(minPos.z, o.minPos.z)};
        maxPos = {std::max(maxPos.x, o.maxPos.x), std::max(maxPos.y, o.maxPos.y), std::max(maxPos.z, o.maxPos.z)};
    }
    /// calculateHash: XXH3_64bits over the 24 bytes (min, max).
    hash::Hash64 calculateHash() const;
};

} // namespace fuse::relight::scene::instances
