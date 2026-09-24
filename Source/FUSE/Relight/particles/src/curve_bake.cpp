/*
* Copyright (c) 2024-2026, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/lssusd/curve_utils.h@0867d3c

// FUSE Relight RL-3.6: curve baking (see curve_bake.hpp).
//
// Modifications (FUSE): a non-template implementation over FUSE containers.
#include <fuse/relight/particles/curve_bake.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::relight::particles {

TangentType parseTangentType(std::string_view token) {
    if (token == "auto") {
        return TangentType::Auto;
    }
    if (token == "smooth") {
        return TangentType::Smooth;
    }
    if (token == "flat") {
        return TangentType::Flat;
    }
    if (token == "step") {
        return TangentType::Step;
    }
    if (token == "custom") {
        return TangentType::Custom;
    }
    return TangentType::Linear;
}

std::size_t findKeyframeInterval(const float* times, std::size_t count, float t) {
    if (count <= 1) {
        return 0;
    }
    if (t <= times[0]) {
        return 0;
    }
    if (t >= times[count - 1]) {
        return count - 2;
    }
    for (std::size_t j = 0; j + 1 < count; ++j) {
        if (t >= times[j] && t <= times[j + 1]) {
            return j;
        }
    }
    return count - 2;
}

float evalBezierFCurve(float x0, float x1, float x2, float x3, float y0, float y1, float y2, float y3, float target) {
    if (std::fabs(x3 - x0) < 1e-9f) {
        return y0;
    }
    float t = (target - x0) / (x3 - x0);
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    for (int i = 0; i < 8; ++i) {
        const float u = 1.f - t;
        const float u2 = u * u, u3 = u2 * u;
        const float t2 = t * t, t3 = t2 * t;
        const float x = u3 * x0 + 3.f * u2 * t * x1 + 3.f * u * t2 * x2 + t3 * x3;
        const float error = x - target;
        if (std::fabs(error) < 1e-6f) {
            break;
        }
        const float dx = 3.f * u2 * (x1 - x0) + 6.f * u * t * (x2 - x1) + 3.f * t2 * (x3 - x2);
        if (std::fabs(dx) < 1e-9f) {
            break;
        }
        t = t - error / dx;
        t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    }
    const float u = 1.f - t;
    const float u2 = u * u, u3 = u2 * u;
    const float t2 = t * t, t3 = t2 * t;
    return u3 * y0 + 3.f * u2 * t * y1 + 3.f * u * t2 * y2 + t3 * y3;
}

namespace {

float bezierInterpolate(float p0, float p1, float p2, float p3, float t) {
    const float u = 1.f - t;
    return u * u * u * p0 + 3.f * u * u * t * p1 + 3.f * u * t * t * p2 + t * t * t * p3;
}

float curveLerp(float a, float b, float t) { return a + (b - a) * t; }

} // namespace

bool bakeFloatCurve(const FloatCurve& curve, std::vector<float>& out, std::uint32_t resolution, float defaultValue) {
    if (!curve.valid()) {
        out.assign(resolution, defaultValue);
        return false;
    }
    const bool tangents = curve.hasFullTangents();
    const bool tangentTimes = curve.hasFullTangentTimes();
    out.resize(resolution);
    for (std::uint32_t i = 0; i < resolution; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(resolution - 1);
        const std::size_t i0 = findKeyframeInterval(curve.times.data(), curve.times.size(), u);
        const std::size_t i1 = std::min(i0 + 1, curve.times.size() - 1);
        if (i0 == i1 || curve.times[i1] == curve.times[i0]) {
            out[i] = curve.values[i0];
            continue;
        }
        const TangentType outType = curve.outTangentTypes.size() > i0 ? curve.outTangentTypes[i0] : TangentType::Linear;
        if (outType == TangentType::Step) {
            out[i] = curve.values[i0];
        } else if (tangents) {
            const float y0 = curve.values[i0];
            const float y3 = curve.values[i1];
            const float y1 = y0 + curve.outTangentValues[i0];
            const float y2 = y3 + curve.inTangentValues[i1];
            if (tangentTimes) {
                const float x0 = curve.times[i0];
                const float x3 = curve.times[i1];
                out[i] = evalBezierFCurve(x0, x0 + curve.outTangentTimes[i0], x3 + curve.inTangentTimes[i1], x3, y0, y1, y2, y3, u);
            } else {
                const float t = (u - curve.times[i0]) / (curve.times[i1] - curve.times[i0]);
                out[i] = bezierInterpolate(y0, y1, y2, y3, t);
            }
        } else {
            const float t = (u - curve.times[i0]) / (curve.times[i1] - curve.times[i0]);
            out[i] = curveLerp(curve.values[i0], curve.values[i1], t);
        }
    }
    return true;
}

bool bakeColorGradient(const ColorGradient& gradient, std::vector<std::array<float, 4>>& out, std::uint32_t resolution,
                       std::array<float, 4> defaultValue) {
    if (!gradient.valid()) {
        out.assign(resolution, defaultValue);
        return false;
    }
    out.resize(resolution);
    for (std::uint32_t i = 0; i < resolution; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(resolution - 1);
        const std::size_t i0 = findKeyframeInterval(gradient.times.data(), gradient.times.size(), u);
        const std::size_t i1 = std::min(i0 + 1, gradient.times.size() - 1);
        if (i0 == i1 || gradient.times[i1] == gradient.times[i0]) {
            out[i] = gradient.values[i0];
            continue;
        }
        const float t = (u - gradient.times[i0]) / (gradient.times[i1] - gradient.times[i0]);
        for (int c = 0; c < 4; ++c) {
            out[i][c] = curveLerp(gradient.values[i0][c], gradient.values[i1][c], t);
        }
    }
    return true;
}

bool combineChannels(const std::vector<const std::vector<float>*>& channels, const std::vector<bool>& present,
                     const std::vector<float>& defaults, std::vector<std::array<float, 4>>& out, std::uint32_t resolution) {
    const bool any = std::find(present.begin(), present.end(), true) != present.end();
    out.assign(resolution, {0.f, 0.f, 0.f, 0.f});
    for (std::uint32_t i = 0; i < resolution; ++i) {
        for (std::size_t c = 0; c < channels.size() && c < 4; ++c) {
            const bool has = any && present[c] && channels[c] != nullptr && i < channels[c]->size();
            out[i][c] = has ? (*channels[c])[i] : defaults[c];
        }
    }
    return any;
}

} // namespace fuse::relight::particles
