/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_matrix_helpers.h@0867d3c and
// include/MathLib/MathLib.h@0867d3c (MvpToPlanes, DecomposeProjection). FUSE changes: scalar float
// code instead of MathLib's SSE types; the side planes are normalized with an exact 1 / sqrt instead
// of the approximate _mm_rsqrt_ps (the normalization cancels in every value used here).
#include <fuse/relight/scene/camera/projection.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utility>

namespace fuse::relight::scene {

namespace {

struct Plane {
    float x = 0, y = 0, z = 0, w = 0;
};

float dot33(const Plane& p) { return p.x * p.x + p.y * p.y + p.z * p.z; }

Plane scaled(const Plane& p, float s) { return {p.x * s, p.y * s, p.z * s, p.w * s}; }

/// Column `k` of the D3D matrix (MathLib: column k of the transpose of its float4x4).
Plane column(const Mat4& m, int k) { return {m[0 * 4 + k], m[1 * 4 + k], m[2 * 4 + k], m[3 * 4 + k]}; }

Plane add(const Plane& a, const Plane& b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
Plane sub(const Plane& a, const Plane& b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }

enum { PLANE_LEFT, PLANE_RIGHT, PLANE_BOTTOM, PLANE_TOP, PLANE_NEAR, PLANE_FAR, PLANES_NUM };

/// MathLib MvpToPlanes(NDC_D3D, ...): returns whether the projection is reversed-Z.
bool mvpToPlanes(const Mat4& m, Plane* planes) {
    const Plane c0 = column(m, 0), c1 = column(m, 1), c2 = column(m, 2), c3 = column(m, 3);
    Plane l = add(c3, c0);
    Plane r = sub(c3, c0);
    Plane b = add(c3, c1);
    Plane t = sub(c3, c1);
    Plane f = sub(c3, c2);
    Plane n = c2; // NDC_D3D: z in [0, 1]

    // Side planes.
    l = scaled(l, 1.0f / std::sqrt(dot33(l)));
    r = scaled(r, 1.0f / std::sqrt(dot33(r)));
    b = scaled(b, 1.0f / std::sqrt(dot33(b)));
    t = scaled(t, 1.0f / std::sqrt(dot33(t)));

    // Near & far planes.
    n = scaled(n, 1.0f / std::max(std::sqrt(dot33(n)), FLT_MIN));
    f = scaled(f, 1.0f / std::max(std::sqrt(dot33(f)), FLT_MIN));

    // Reversed projection.
    const bool reversed = std::abs(n.w) > std::abs(f.w);
    if (reversed) {
        std::swap(n, f);
    }

    // Infinite projection.
    if (dot33(f) <= FLT_MIN) {
        f = Plane{-n.x, -n.y, -n.z, f.w};
    }

    planes[PLANE_LEFT] = l;
    planes[PLANE_RIGHT] = r;
    planes[PLANE_BOTTOM] = b;
    planes[PLANE_TOP] = t;
    planes[PLANE_NEAR] = n;
    planes[PLANE_FAR] = f;
    return reversed;
}

} // namespace

DecomposeProjectionParams decomposeProjection(const Mat4& m) {
    Plane plane[PLANES_NUM];
    const bool reversedZ = mvpToPlanes(m, plane);

    // MathLib's a33 / a22 are D3D m[3][3] / m[2][2] (the diagonal is transpose-invariant).
    const bool isOrtho = m[15] == 1.0f;

    const float nearZ = -plane[PLANE_NEAR].w;
    const float farZ = plane[PLANE_FAR].w;

    float x0, x1, y0, y1;
    if (isOrtho) {
        x0 = -plane[PLANE_LEFT].w;
        x1 = plane[PLANE_RIGHT].w;
        y0 = -plane[PLANE_BOTTOM].w;
        y1 = plane[PLANE_TOP].w;
    } else {
        x0 = plane[PLANE_LEFT].z / plane[PLANE_LEFT].x;
        x1 = plane[PLANE_RIGHT].z / plane[PLANE_RIGHT].x;
        y0 = plane[PLANE_BOTTOM].z / plane[PLANE_BOTTOM].y;
        y1 = plane[PLANE_TOP].z / plane[PLANE_TOP].y;
    }

    const bool leftHanded = m[10] > 0.0f;

    // pfSettings15 (the swap is possible because it is the last pass).
    if (leftHanded) {
        std::swap(x0, x1);
        std::swap(y0, y1);
    }
    const float angleY0 = std::atan(isOrtho ? 0.0f : y0);
    const float angleY1 = std::atan(isOrtho ? 0.0f : y1);
    const float angleX0 = std::atan(isOrtho ? 0.0f : x0);
    const float angleX1 = std::atan(isOrtho ? 0.0f : x1);
    const float aspect = (x1 - x0) / (y1 - y0);

    DecomposeProjectionParams p;
    // rtx_matrix_helpers.h: the aspect ratio's sign follows m00 * m11.
    p.aspectRatio = m[0] * m[5] > 0.0f ? aspect : -aspect;
    // FoV is the vertical FoV in radians (PROJ_FOVY).
    p.fov = std::abs(angleY1 - angleY0);
    p.nearPlane = nearZ;
    p.farPlane = farZ;
    p.shearX = (angleX0 + angleX1) * 0.5f;
    p.shearY = (angleY0 + angleY1) * 0.5f;
    p.isLHS = leftHanded;
    p.isReverseZ = reversedZ;
    p.isOrthographic = isOrtho;
    return p;
}

ProjectionJitter projectionJitter(const Mat4& m, std::uint32_t width, std::uint32_t height) {
    ProjectionJitter j;
    // Perspective only: clip.w = z * m[11] (+-1), so the NDC offset of the m[8] / m[9] terms is m[8] / m[11].
    if (m[15] == 1.0f || m[11] == 0.0f) {
        return j;
    }
    j.ndcX = m[8] / m[11];
    j.ndcY = m[9] / m[11];
    j.pixelX = j.ndcX * 0.5f * static_cast<float>(width);
    j.pixelY = -j.ndcY * 0.5f * static_cast<float>(height); // NDC y up, pixels down
    j.subPixel = (j.ndcX != 0.0f || j.ndcY != 0.0f) && std::abs(j.pixelX) <= 1.0f && std::abs(j.pixelY) <= 1.0f;
    return j;
}

Mat4 removeProjectionJitter(const Mat4& m) {
    Mat4 out = m;
    if (m[15] != 1.0f) {
        out[8] = 0.0f;
        out[9] = 0.0f;
    }
    return out;
}

} // namespace fuse::relight::scene
