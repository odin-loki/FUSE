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
// Ported from dxvk-remix src/util/util_matrix.h@0867d3c (inverse, inverseAffine, Matrix4 * Vector4),
// src/d3d9/d3d9_rtx.cpp@0867d3c (processRenderState transforms, prepareVertexCapture constants) and
// src/dxso/dxso_compiler.cpp@0867d3c (emitVertexCaptureOp arithmetic)

// FUSE Relight RL-1.6: the CPU back-transform of captured vertices (plan §2.5).
//
// Remix does this on the GPU, inside the vertex shader it compiles: with the draw's
// D3D9RtxVertexCaptureData constants
//   invProj       = inverse(viewToProjection)
//   viewToWorld   = inverseAffine(worldToView)
//   worldToObject = inverseAffine(objectToWorld)
//   normalTransform = objectToWorld
// it stores, for the clip-space position p (gl_Position):
//   view   = (invProj * p).xyz            (no perspective divide: w of invProj * p is dropped)
//   world  = (viewToWorld * (view, 1)).xyz
//   object = (worldToObject * (world, 1)).xyz
// and normal0 = normalTransform3x3 * NORMAL0.xyz, where the 3x3 is read from the row-major UBO
// member column by column (so it is the upper 3x3 of the D3D matrix applied as M * n, i.e. the
// transpose of the D3D row-vector convention: kept as is). Relight's shader stores p itself and
// this file applies the same arithmetic on the CPU.
//
// Matrices are D3DMATRIX in memory order (m[r * 4 + c] = _{r+1}{c+1}), which is also Remix's Matrix4
// (data[r] = D3D row r, used as a column in Matrix4 * Vector4).
//
// processRenderState semantics: objectToWorld is D3DTS_WORLD unless the draw uses a programmable VS
// with vertex capture off or rtx.useWorldMatricesForShaders off (then identity); worldToView =
// D3DTS_VIEW, viewToProjection = D3DTS_PROJECTION; sanitize() sets [3][3] = 1 where it is 0 in
// objectToWorld and worldToView.
#pragma once

#include <fuse/relight/capture/vertex_capture/capture_layout.hpp>

#include <array>
#include <cstdint>

namespace fuse::relight::capture::vertex_capture {

using Matrix4 = std::array<float, 16>; ///< D3DMATRIX memory order
using Vector4 = std::array<float, 4>;

[[nodiscard]] Matrix4 identityMatrix();

/// Remix Matrix4 * Vector4: m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3] * v.w (rows of the D3D
/// memory), summed as (mul0 + mul1) + (mul2 + mul3).
[[nodiscard]] Vector4 transform(const Matrix4& m, const Vector4& v);

/// util_matrix inverse(): general 4x4 inverse through cofactors, in double, rounded to float.
[[nodiscard]] Matrix4 inverse(const Matrix4& m);

/// util_matrix inverseAffine(): 3x3 inverse in double plus the translation; falls back to
/// inverse() when |det| < 1e-24.
[[nodiscard]] Matrix4 inverseAffine(const Matrix4& m);

/// DrawCallTransforms as processRenderState fills it for a draw.
struct DrawTransforms {
    Matrix4 objectToWorld = identityMatrix();
    Matrix4 worldToView = identityMatrix();
    Matrix4 viewToProjection = identityMatrix();
};

/// processRenderState: `world`, `view`, `projection` are the draw's D3DTS_WORLD / VIEW /
/// PROJECTION; `useObjectToWorld` is !programmableVS || (useVertexCapture && useWorldMatricesForShaders).
[[nodiscard]] DrawTransforms drawTransforms(const float world[16], const float view[16], const float projection[16],
                                            bool useObjectToWorld);

/// The D3D9RtxVertexCaptureData matrices prepareVertexCapture uploads.
struct BackTransform {
    Matrix4 invProj = identityMatrix();
    Matrix4 viewToWorld = identityMatrix();
    Matrix4 worldToObject = identityMatrix();
    Matrix4 normalTransform = identityMatrix();
};

[[nodiscard]] BackTransform backTransformFor(const DrawTransforms& transforms);

/// clip -> object space exactly as emitVertexCaptureOp computes it.
[[nodiscard]] std::array<float, 3> clipToObject(const BackTransform& bt, const float clip[4]);

/// normalTransform3x3 * n (see the file comment).
[[nodiscard]] std::array<float, 3> transformNormal(const BackTransform& bt, const float normal[3]);

/// One GPU slot to Remix's CapturedVertex. Members the shader did not write keep Remix's values
/// for them (texcoord (0, 0), normal (0, 0, 0), colour 0xFFFFFFFF).
[[nodiscard]] CapturedVertex backTransform(const BackTransform& bt, const RawCapturedVertex& raw);

} // namespace fuse::relight::capture::vertex_capture
