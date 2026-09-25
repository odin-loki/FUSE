/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_point_instancer_system.{h,cpp}@0867d3c (PointInstancerBatch,
// dispatchCulling's constants) and src/dxvk/shaders/rtx/pass/instance_culling/point_instancer_culling.comp.slang
// @0867d3c (the per-instance culling, fade and transform expansion).
//
// A point instancer (USD PointInstancer replacements: foliage, ground clutter) is one RtInstance whose draw
// carries N instanceToObject transforms. Every frame each of the N expands to its own TLAS entry: full
// transform = objectToWorld x instanceToObject (and the same with the previous objectToWorld for motion
// vectors), its own surface index (base + i), and a visibility mask: culled beyond
// rtx.pointInstancer.cullingRadius from the camera, and stochastically thinned (a Knuth multiplicative hash
// of the instance index, stable across frames) between fadeStartRadius and cullingRadius. With
// rtx.pointInstancer.enable off every instance passes (radius FLT_MAX, no fade).
//
// Upstream runs this on the GPU; this is the CPU reference with the same arithmetic (float), used by the
// scene model's per-frame batches and by tests. It is the input the future GPU pass (RL-3.x) must match.
#pragma once

#include <fuse/relight/scene/instances/instance_math.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace fuse::relight::scene::instances {

struct PointInstancerBatch {
    std::uint64_t instanceId = 0; ///< the owning RtInstance (FUSE)
    std::shared_ptr<const std::vector<Mat4f>> transforms; ///< instanceToObject
    Mat4f objectToWorld = identityMatrix();
    Mat4f prevObjectToWorld = identityMatrix();
    std::uint32_t baseSurfaceIndex = 0;          ///< surfaceIndexOfFirstInstance
    std::uint32_t instanceMask = 0xff;
    bool subsurfaceTlas = false;                 ///< the extra SSS TLAS batch of a subsurface instancer
};

struct PointInstance {
    Mat4f objectToWorld = identityMatrix();      ///< fullTransform
    Mat4f prevObjectToWorld = identityMatrix();  ///< prevFull
    std::uint32_t surfaceIndex = 0;              ///< baseSurfaceIndex + i
    std::uint32_t mask = 0;                      ///< instanceMask, or 0 when culled
    bool visible() const { return mask != 0; }
};

struct PointInstancerCulling {
    Vec3 cameraPosition;
    float cullingRadius = 5000.f;
    float fadeStartRadius = 0.f;

    /// The rtx.pointInstancer.* options (FLT_MAX / 0 when disabled).
    static PointInstancerCulling fromOptions(const Vec3& cameraPosition);
};

/// The shader's visibility decision for instance `index` at world position `worldPos`.
bool pointInstanceVisible(const PointInstancerCulling& culling, std::uint32_t index, const Vec3& worldPos);

/// Expands a batch (one entry per transform, in order).
std::vector<PointInstance> expandPointInstancer(const PointInstancerBatch& batch, const PointInstancerCulling& culling);

} // namespace fuse::relight::scene::instances
