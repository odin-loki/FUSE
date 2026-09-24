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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_point_instancer_system.cpp@0867d3c and
// src/dxvk/shaders/rtx/pass/instance_culling/point_instancer_culling.comp.slang@0867d3c. See point_instancer.hpp.
#include <fuse/relight/scene/instances/instance_options.hpp>
#include <fuse/relight/scene/instances/point_instancer.hpp>

#include <cfloat>
#include <cmath>

namespace fuse::relight::scene::instances {

PointInstancerCulling PointInstancerCulling::fromOptions(const Vec3& cameraPosition) {
    // When culling is disabled, use FLT_MAX so every instance passes the distance test.
    const bool cullingEnabled = PointInstancerOptions::enable();
    PointInstancerCulling c;
    c.cameraPosition = cameraPosition;
    c.cullingRadius = cullingEnabled ? PointInstancerOptions::cullingRadius() : FLT_MAX;
    c.fadeStartRadius = cullingEnabled ? PointInstancerOptions::fadeStartRadius() : 0.f;
    return c;
}

bool pointInstanceVisible(const PointInstancerCulling& cb, std::uint32_t instanceIdx, const Vec3& worldPos) {
    // Squared-distance cull (avoid sqrt in the common rejection case)
    const Vec3 delta = worldPos - cb.cameraPosition;
    const float distSq = dot(delta, delta);
    const float radiusSq = cb.cullingRadius * cb.cullingRadius;
    bool visible = distSq <= radiusSq;

    // Optional density fade: stochastically reject instances in the fade region.
    if (visible && cb.fadeStartRadius > 0.0f && distSq > cb.fadeStartRadius * cb.fadeStartRadius &&
        cb.cullingRadius > cb.fadeStartRadius) {
        const float dist = std::sqrt(distSq);
        const float fadeRange = cb.cullingRadius - cb.fadeStartRadius;
        const float fadeFactor = (dist - cb.fadeStartRadius) / fadeRange; // 0..1
        // Deterministic hash-based rejection (stable per instance, no flickering)
        const std::uint32_t hash = instanceIdx * 2654435761u; // Knuth multiplicative hash
        const float rand01 = float(hash & 0xFFFFu) / 65535.0f;
        if (rand01 < fadeFactor) {
            visible = false;
        }
    }
    return visible;
}

std::vector<PointInstance> expandPointInstancer(const PointInstancerBatch& batch, const PointInstancerCulling& culling) {
    std::vector<PointInstance> out;
    if (!batch.transforms) {
        return out;
    }
    out.reserve(batch.transforms->size());
    std::uint32_t instanceIdx = 0;
    for (const Mat4f& instanceToObject : *batch.transforms) {
        // fullTransform = mul(objectToWorld, instanceToObject) (column vectors) = instanceToObject x objectToWorld.
        PointInstance p;
        p.objectToWorld = multiply(instanceToObject, batch.objectToWorld);
        p.prevObjectToWorld = multiply(instanceToObject, batch.prevObjectToWorld);
        p.surfaceIndex = batch.baseSurfaceIndex + instanceIdx;
        p.mask = pointInstanceVisible(culling, instanceIdx, translation(p.objectToWorld)) ? batch.instanceMask : 0u;
        out.push_back(p);
        ++instanceIdx;
    }
    return out;
}

} // namespace fuse::relight::scene::instances
