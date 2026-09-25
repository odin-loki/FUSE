/*
* Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_lights.cpp@0867d3c (RtLightShaping::getHash and the
// RtSphereLight/RtRectLight/RtDiskLight/RtCylinderLight/RtDistantLight::updateCachedHash functions).
// FUSE changes: free functions over plain float arrays instead of the RtLight classes.
#include <fuse/relight/hash/light_hash.hpp>

namespace fuse::relight::hash {

namespace {

Hash64 foldShaping(Hash64 h, const LightShaping& shaping) noexcept {
    return xxh64(&h, sizeof(h), hashLightShaping(shaping));
}

} // namespace

Hash64 hashLightShaping(const LightShaping& s) noexcept {
    Hash64 h = 0;
    if (s.enabled) {
        h = xxh64(s.direction.data(), sizeof(float) * 3, h);
        h = xxh64(&s.cosConeAngle, sizeof(s.cosConeAngle), h);
        h = xxh64(&s.coneSoftness, sizeof(s.coneSoftness), h);
        h = xxh64(&s.focusExponent, sizeof(s.focusExponent), h);
    }
    return h;
}

Hash64 hashSphereLight(const Float3& position, float radius, const LightShaping& shaping) noexcept {
    Hash64 h = Hash64(LightType::Sphere);
    // Note: Radiance not included to somewhat uniquely identify lights when constructed from D3D9 lights.
    h = xxh64(position.data(), sizeof(float) * 3, h);
    h = xxh64(&radius, sizeof(radius), h);
    return foldShaping(h, shaping);
}

Hash64 hashRectLight(const Float3& position, const Float2& dimensions, const Float3& xAxis, const Float3& yAxis,
                     const Float3& direction, const LightShaping& shaping) noexcept {
    Hash64 h = Hash64(LightType::Rect);
    h = xxh64(position.data(), sizeof(float) * 3, h);
    h = xxh64(dimensions.data(), sizeof(float) * 2, h);
    h = xxh64(xAxis.data(), sizeof(float) * 3, h);
    h = xxh64(yAxis.data(), sizeof(float) * 3, h);
    h = xxh64(direction.data(), sizeof(float) * 3, h);
    return foldShaping(h, shaping);
}

Hash64 hashDiskLight(const Float3& position, const Float2& halfDimensions, const Float3& xAxis, const Float3& yAxis,
                     const Float3& direction, const LightShaping& shaping) noexcept {
    Hash64 h = Hash64(LightType::Disk);
    h = xxh64(position.data(), sizeof(float) * 3, h);
    h = xxh64(halfDimensions.data(), sizeof(float) * 2, h);
    h = xxh64(xAxis.data(), sizeof(float) * 3, h);
    h = xxh64(yAxis.data(), sizeof(float) * 3, h);
    h = xxh64(direction.data(), sizeof(float) * 3, h);
    return foldShaping(h, shaping);
}

Hash64 hashCylinderLight(const Float3& position, float radius, const Float3& axis, float axisLength) noexcept {
    Hash64 h = Hash64(LightType::Cylinder);
    h = xxh64(position.data(), sizeof(float) * 3, h);
    h = xxh64(&radius, sizeof(radius), h);
    h = xxh64(axis.data(), sizeof(float) * 3, h);
    h = xxh64(&axisLength, sizeof(axisLength), h);
    return h;
}

Hash64 hashDistantLight(const Float3& direction, float halfAngle) noexcept {
    Hash64 h = Hash64(LightType::Distant);
    h = xxh64(direction.data(), sizeof(float) * 3, h);
    h = xxh64(&halfAngle, sizeof(halfAngle), h);
    return h;
}

} // namespace fuse::relight::hash
