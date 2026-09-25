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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_matrix_helpers.h@0867d3c (decomposeProjection,
// DecomposeProjectionParams) and include/MathLib/MathLib.h@0867d3c (DecomposeProjection settings
// output and MvpToPlanes for NDC_D3D, MathLib 1.6, MIT).
//
// Camera decomposition (plan §1.5 "Camera"): FOV, aspect, near/far, handedness, reversed Z and shear
// from a D3D9 PROJECTION transform. Matrices are row-major D3DMATRIX (m[row * 4 + col]) as the tap
// delivers them; Remix memcpy's the same bytes into MathLib's column-major float4x4, i.e. MathLib sees
// the transpose (the column-vector form), which is what the plane extraction below reproduces.
//
// Jitter detection is a FUSE addition (Remix applies its own TAA / DLSS jitter and only rejects
// cameras whose shear exceeds 0.01 rad): games with their own TAA add a sub-pixel off-centre term to
// the projection every frame, which the camera manager reports and can strip (removeProjectionJitter)
// so the decomposition and motion vectors see a stable camera.
#pragma once

#include <array>
#include <cstdint>

namespace fuse::relight::scene {

using Mat4 = std::array<float, 16>;

/// rtx_matrix_helpers.h DecomposeProjectionParams (+ isOrthographic, MathLib's PROJ_ORTHO flag).
struct DecomposeProjectionParams {
    float fov = 0.f;         ///< vertical FOV in radians (MathLib PROJ_FOVY)
    float aspectRatio = 0.f; ///< negative when m00 * m11 <= 0 (a mirrored projection)
    float nearPlane = 0.f;
    float farPlane = 0.f;
    float shearX = 0.f; ///< PROJ_DIRX: mean of the left / right frustum angles (radians)
    float shearY = 0.f; ///< PROJ_DIRY
    bool isLHS = false;
    bool isReverseZ = false;
    bool isOrthographic = false;
};

/// rtx_matrix_helpers.h decomposeProjection(matrix, params): MathLib DecomposeProjection with
/// NDC_D3D origin and depth.
DecomposeProjectionParams decomposeProjection(const Mat4& d3dProjection);

/// Sub-pixel projection offset (FUSE): the NDC offset a perspective projection adds to every vertex
/// (m[8] / m[11] and m[9] / m[11]), and the same in pixels of a `width` x `height` viewport (y down).
struct ProjectionJitter {
    float ndcX = 0.f, ndcY = 0.f;
    float pixelX = 0.f, pixelY = 0.f;
    /// Non-zero and within one pixel on both axes: the signature of TAA jitter.
    bool subPixel = false;
};
ProjectionJitter projectionJitter(const Mat4& d3dProjection, std::uint32_t width, std::uint32_t height);

/// The projection without its off-centre terms (m[8] = m[9] = 0); orthographic projections unchanged.
Mat4 removeProjectionJitter(const Mat4& d3dProjection);

} // namespace fuse::relight::scene
