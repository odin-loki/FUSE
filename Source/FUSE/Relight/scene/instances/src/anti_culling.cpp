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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_camera.cpp@0867d3c, rtx_intersection_test_helpers.h@0867d3c
// and rtx_draw_call_tracker.cpp@0867d3c. See anti_culling.hpp.
#include <fuse/relight/scene/instances/anti_culling.hpp>
#include <fuse/relight/scene/instances/instance_options.hpp>

#include <cfloat>
#include <cmath>
#include <limits>

namespace fuse::relight::scene::instances {

namespace {

FrustumPlane plane(Vec3 n, float w) {
    const float len = length(n);
    return {n / len, w / len};
}

Vec4 v4(Vec3 v, float w) { return {v.x, v.y, v.z, w}; }

} // namespace

AntiCullingFrustum AntiCullingFrustum::build(float fov, float aspectRatio, float nearPlane, float farPlane, bool isLHS,
                                             const Mat4f& worldToView, float fovScale, float farPlaneScale, bool infinite) {
    AntiCullingFrustum f;
    f.isLHS = isLHS;
    f.isInfinite = infinite;
    f.worldToView = worldToView;
    f.nearPlane = nearPlane;

    // frustumMatrix.SetupByHalfFovy(fov * fovScale * 0.5, aspect, near, far * farPlaneScale) -> m_frustum.Setup.
    const float tanY = std::tan(fov * fovScale * 0.5f);
    const float tanX = tanY * aspectRatio;
    const float s = isLHS ? 1.f : -1.f; // view direction: +Z (LHS) or -Z (RHS)
    const float scaledFar = infinite ? std::numeric_limits<float>::infinity() : farPlane * farPlaneScale;
    f.planes[PlaneLeft] = plane({1.f, 0.f, s * tanX}, 0.f);
    f.planes[PlaneRight] = plane({-1.f, 0.f, s * tanX}, 0.f);
    f.planes[PlaneBottom] = plane({0.f, 1.f, s * tanY}, 0.f);
    f.planes[PlaneTop] = plane({0.f, -1.f, s * tanY}, 0.f);
    f.planes[PlaneNear] = {{0.f, 0.f, s}, -nearPlane};
    f.planes[PlaneFar] = {{0.f, 0.f, -s}, scaledFar};
    f.farPlane = scaledFar;

    // m_frustum.calculateFrustumGeometry(nearPlane, farPlane, fov, aspectRatio, isLHS): unscaled.
    const float tanHalfFov = std::tan(fov * 0.5f);
    f.nearPlaneUpExtent = nearPlane * tanHalfFov;
    f.nearPlaneRightExtent = f.nearPlaneUpExtent * aspectRatio;
    const float farPlaneUpExtent = farPlane * tanHalfFov;
    const float farPlaneRightExtent = farPlaneUpExtent * aspectRatio;
    const float N = isLHS ? nearPlane : -nearPlane;
    const float F = isLHS ? farPlane : -farPlane;
    const Vec3 nearVertices[4] = {{-f.nearPlaneRightExtent, -f.nearPlaneUpExtent, N},
                                  {-f.nearPlaneRightExtent, f.nearPlaneUpExtent, N},
                                  {f.nearPlaneRightExtent, f.nearPlaneUpExtent, N},
                                  {f.nearPlaneRightExtent, -f.nearPlaneUpExtent, N}};
    const Vec3 farVertices[4] = {{-farPlaneRightExtent, -farPlaneUpExtent, F},
                                 {-farPlaneRightExtent, farPlaneUpExtent, F},
                                 {farPlaneRightExtent, farPlaneUpExtent, F},
                                 {farPlaneRightExtent, -farPlaneUpExtent, F}};
    for (int i = 0; i < 4; ++i) {
        f.edgeVectors[i] = normalize(farVertices[i] - nearVertices[i]);
    }
    return f;
}

AntiCullingFrustum AntiCullingFrustum::fromCamera(const CameraState& camera) {
    Mat4f worldToView;
    for (std::size_t i = 0; i < 16; ++i) {
        worldToView[i] = camera.worldToView[i];
    }
    return build(camera.fov, camera.aspectRatio, camera.nearPlane, camera.farPlane, camera.isLHS, worldToView,
                 AntiCullingOptions::Object::fovScale(), AntiCullingOptions::Object::farPlaneScale(),
                 AntiCullingOptions::Object::enableInfinityFarFrustum());
}

bool AntiCullingFrustum::intersects(const AxisAlignedBoundingBox& aabb, const Mat4f& objectToWorld, bool highPrecision) const {
    // objectToView = camera.getWorldToView(false) * objectToWorld (column vectors).
    const Mat4f objectToView = multiply(objectToWorld, worldToView);
    if (highPrecision) {
        return boundingBoxIntersectsFrustumSAT(*this, aabb.minPos, aabb.maxPos, objectToView);
    }
    return boundingBoxIntersectsFrustum(*this, aabb.minPos, aabb.maxPos, objectToView);
}

bool boundingBoxIntersectsFrustum(const AntiCullingFrustum& frustum, const Vec3& minPos, const Vec3& maxPos,
                                  const Mat4f& objectToView) {
    const Vec4 minPosView = transform4(objectToView, v4(minPos, 1.0f));
    const Vec4 maxPosView = transform4(objectToView, v4(maxPos, 1.0f));
    const Vec4 obbVertices[8] = {
        {minPosView.x, minPosView.y, minPosView.z, 1.0f}, {maxPosView.x, minPosView.y, minPosView.z, 1.0f},
        {minPosView.x, maxPosView.y, minPosView.z, 1.0f}, {minPosView.x, minPosView.y, maxPosView.z, 1.0f},
        {maxPosView.x, maxPosView.y, minPosView.z, 1.0f}, {minPosView.x, maxPosView.y, maxPosView.z, 1.0f},
        {maxPosView.x, minPosView.y, maxPosView.z, 1.0f}, {minPosView.x, minPosView.y, minPosView.z, 1.0f},
    };
    for (std::uint32_t planeIdx = 0; planeIdx < PlaneCount; ++planeIdx) {
        const FrustumPlane& p = frustum.planes[planeIdx];
        const Vec4 plane4{p.normal.x, p.normal.y, p.normal.z, p.w};
        bool insidePlane = false;
        for (const Vec4& v : obbVertices) {
            if (dot(plane4, v) >= 0.0f) {
                insidePlane = true;
                break;
            }
        }
        if (!insidePlane) {
            return false;
        }
    }
    return true;
}

bool boundingBoxIntersectsFrustumSAT(const AntiCullingFrustum& frustum, const Vec3& minPos, const Vec3& maxPos,
                                     const Mat4f& objectToView) {
    const float nearPlane = frustum.nearPlane;
    const float farPlane = frustum.farPlane;
    const float nearPlaneRightExtent = frustum.nearPlaneRightExtent;
    const float nearPlaneUpExtent = frustum.nearPlaneUpExtent;
    const bool isLHS = frustum.isLHS;
    const bool isInfFrustum = frustum.isInfinite;

    // The 3 normalized OBB axes (also the OBB edge directions).
    const Vec4 obbCenterView = transform4(objectToView, v4((minPos + maxPos) * 0.5f, 1.0f));
    // When the OBB is flat on an axis, keep a unit axis for the direction and a zero extent.
    const Vec3 extentScale{maxPos.x - minPos.x > FLT_EPSILON ? 0.5f : 0.0f, maxPos.y - minPos.y > FLT_EPSILON ? 0.5f : 0.0f,
                           maxPos.z - minPos.z > FLT_EPSILON ? 0.5f : 0.0f};
    const Vec4 obbAxisView[3] = {
        transform4(objectToView, extentScale.x != 0.0f ? Vec4{maxPos.x - minPos.x, 0.f, 0.f, 0.f} : Vec4{1.f, 0.f, 0.f, 0.f}),
        transform4(objectToView, extentScale.y != 0.0f ? Vec4{0.f, maxPos.y - minPos.y, 0.f, 0.f} : Vec4{0.f, 1.f, 0.f, 0.f}),
        transform4(objectToView, extentScale.z != 0.0f ? Vec4{0.f, 0.f, maxPos.z - minPos.z, 0.f} : Vec4{0.f, 0.f, 1.f, 0.f}),
    };
    const Vec4 obbExtents{length(obbAxisView[0]), length(obbAxisView[1]), length(obbAxisView[2]), 0.0f};
    const auto scaled = [](const Vec4& a, float len, float scale) {
        return Vec4{a.x / len * scale, a.y / len * scale, a.z / len * scale, a.w / len * scale};
    };
    const Vec4 obbAxisNormalized[3] = {scaled(obbAxisView[0], obbExtents.x, extentScale.x),
                                       scaled(obbAxisView[1], obbExtents.y, extentScale.y),
                                       scaled(obbAxisView[2], obbExtents.z, extentScale.z)};

    const auto calProjectedObbExtent = [&](const Vec4& axis) -> float {
        const Vec4 proj{std::abs(dot(obbAxisNormalized[0], axis)), std::abs(dot(obbAxisNormalized[1], axis)),
                        std::abs(dot(obbAxisNormalized[2], axis)), 0.0f};
        return dot(proj, obbExtents);
    };

    // Fast frustum projection (geometrictools IntersectionBox3Frustum3).
    const auto calProjectedFrustumExtent = [&](const Vec4& axis, float& p0, float& p1) {
        const float MoX = std::abs(axis.x);
        const float MoY = std::abs(axis.y);
        const float MoZ = isLHS ? axis.z : -axis.z;
        const float p = nearPlaneRightExtent * MoX + nearPlaneUpExtent * MoY;
        const float farNearRatio = farPlane / nearPlane;
        p0 = nearPlane * MoZ - p;
        if (p0 < 0.0f) {
            p0 = isInfFrustum ? -std::numeric_limits<float>::infinity() : p0 * farNearRatio;
        }
        p1 = nearPlane * MoZ + p;
        if (p1 > 0.0f) {
            p1 = isInfFrustum ? std::numeric_limits<float>::infinity() : p1 * farNearRatio;
        }
    };

    const auto checkSeparableAxis = [&](const Vec4& axis) -> bool {
        const float projObbCenter = dot(obbCenterView, axis);
        const float projObbExtent = calProjectedObbExtent(axis);
        const float obbMin = projObbCenter - projObbExtent;
        const float obbMax = projObbCenter + projObbExtent;
        float p0 = 0.0f, p1 = 0.0f;
        calProjectedFrustumExtent(axis, p0, p1);
        return obbMin > p1 || obbMax < p0;
    };

    // Frustum normals (5 axes). Z first.
    {
        const float projObbCenter = obbCenterView.z;
        const float obbExtent = calProjectedObbExtent(Vec4{0.0f, 0.0f, 1.0f, 0.0f});
        if (isLHS) {
            if (projObbCenter + obbExtent < nearPlane || (!isInfFrustum && projObbCenter - obbExtent > farPlane)) {
                return false;
            }
        } else {
            // Upstream's right-handed near / far checks, kept as is.
            if (projObbCenter - obbExtent > nearPlane || (!isInfFrustum && projObbCenter - obbExtent > farPlane)) {
                return false;
            }
        }
        // Side planes
        for (std::uint32_t planeIdx = 0; planeIdx <= PlaneTop; ++planeIdx) {
            const Vec3& n = frustum.planes[planeIdx].normal;
            if (checkSeparableAxis(Vec4{n.x, n.y, n.z, 0.0f})) {
                return false;
            }
        }
    }

    // OBB axes (3 axes)
    for (const Vec4& axis : obbAxisNormalized) {
        if (checkSeparableAxis(axis)) {
            return false;
        }
    }

    // Cross products between OBB edges and frustum edges (18 axes)
    for (const Vec4& a : obbAxisNormalized) { // x frustumRight (1, 0, 0)
        if (checkSeparableAxis(Vec4{0.0f, a.z, -a.y, 0.0f})) {
            return false;
        }
    }
    for (const Vec4& a : obbAxisNormalized) { // x frustumUp (0, 1, 0)
        if (checkSeparableAxis(Vec4{-a.z, 0.0f, a.x, 0.0f})) {
            return false;
        }
    }
    for (const Vec4& a : obbAxisNormalized) {
        for (const Vec3& edge : frustum.edgeVectors) {
            const Vec4 crossProductAxis = v4(cross(a.xyz(), edge), 0.0f);
            // Make sure the 2 edges are NOT parallel with each other
            if (dot(crossProductAxis, crossProductAxis) > 0.1f && checkSeparableAxis(crossProductAxis)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace fuse::relight::scene::instances
