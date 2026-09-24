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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_camera.cpp@0867d3c (RtCamera::updateAntiCulling's object
// frustum, RtFrustum::calculateFrustumGeometry), src/dxvk/rtx_render/rtx_intersection_test_helpers.h@0867d3c
// (boundingBoxIntersectsFrustum, boundingBoxIntersectsFrustumSATInternal) and
// src/dxvk/rtx_render/rtx_draw_call_tracker.cpp@0867d3c (aabbIntersectsFrustum).
//
// The object anti-culling frustum is the main camera's view frustum with the vertical FOV scaled by
// rtx.antiCulling.object.fovScale and the far plane by rtx.antiCulling.object.farPlaneScale (or infinite
// with enableInfinityFarFrustum), in view space. Its planes are built analytically here (MathLib's cFrustum
// extracts the same planes from the projection matrix: inward normals, near plane at the camera's near
// distance); the SAT test's near-plane extents and edge vectors use the unscaled FOV and far plane, as
// upstream. An object is "inside" when its bounding box, through objectToWorld x worldToView, intersects
// the frustum (SAT with enableHighPrecisionAntiCulling, else the fast per-plane test).
#pragma once

#include <fuse/relight/scene/camera/camera_manager.hpp>
#include <fuse/relight/scene/instances/instance_math.hpp>

#include <array>

namespace fuse::relight::scene::instances {

/// A view-space plane: dot(normal, p) + w >= 0 inside.
struct FrustumPlane {
    Vec3 normal;
    float w = 0.f;
};

/// cFrustum plane order.
enum FrustumPlaneIndex : std::uint32_t { PlaneLeft = 0, PlaneRight, PlaneBottom, PlaneTop, PlaneNear, PlaneFar, PlaneCount };

struct AntiCullingFrustum {
    std::array<FrustumPlane, PlaneCount> planes{};
    float nearPlane = 0.f;
    float farPlane = 0.f;            ///< the far plane's w (scaled far distance)
    float nearPlaneRightExtent = 0.f;
    float nearPlaneUpExtent = 0.f;
    std::array<Vec3, 4> edgeVectors{}; ///< normalized near -> far corner vectors
    bool isLHS = true;
    bool isInfinite = false;
    Mat4f worldToView = identityMatrix();

    /// RtCamera::updateAntiCulling (object frustum) for a camera: fov (vertical, radians), aspect, near, far,
    /// handedness and worldToView from `camera`, scales from the rtx.antiCulling.object.* options.
    static AntiCullingFrustum fromCamera(const CameraState& camera);
    /// The same with explicit parameters.
    static AntiCullingFrustum build(float fov, float aspectRatio, float nearPlane, float farPlane, bool isLHS,
                                    const Mat4f& worldToView, float fovScale, float farPlaneScale, bool infinite);

    /// DrawCallTracker's aabbIntersectsFrustum: SAT (highPrecision) or the fast test.
    bool intersects(const AxisAlignedBoundingBox& aabb, const Mat4f& objectToWorld, bool highPrecision) const;
};

/// boundingBoxIntersectsFrustum: every plane must have one of the "OBB vertices" in front of it. Upstream
/// builds the 8 vertices from the view-space images of min and max only (not the 8 transformed corners; the
/// last vertex repeats the first); kept as is.
bool boundingBoxIntersectsFrustum(const AntiCullingFrustum& frustum, const Vec3& minPos, const Vec3& maxPos,
                                  const Mat4f& objectToView);

/// boundingBoxIntersectsFrustumSATInternal: separating axis test of the view-space OBB against the frustum.
bool boundingBoxIntersectsFrustumSAT(const AntiCullingFrustum& frustum, const Vec3& minPos, const Vec3& maxPos,
                                     const Mat4f& objectToView);

} // namespace fuse::relight::scene::instances
