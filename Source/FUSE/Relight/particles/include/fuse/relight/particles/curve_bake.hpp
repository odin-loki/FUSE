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

// FUSE Relight RL-3.6: curve baking for the particle animation channels (the part RL-3.2 left open: the importer
// keeps the `primvars:particle:*` curve arrays raw; the runtime bakes them here).
//
// A float channel is a keyframed curve (times in [0, 1] of normalized particle age, values, per-key tangent types,
// optional Bezier tangents and tangent times); a colour channel is a linear gradient. Both bake to kCurveResolution
// uniform samples at u = i / (kCurveResolution - 1); an invalid or absent curve bakes to its default and reports
// false (the caller then falls back to the legacy spawn / target pair, as upstream).
//
// Ported from dxvk-remix src/lssusd/curve_utils.h @0867d3c.
//
// Modifications (FUSE): namespace and naming; the vector helpers are written for the FUSE Float4 / fixed-size arrays
// instead of templates over engine vector types; the implementation lives in src/curve_bake.cpp.
#pragma once

#include <fuse/relight/particles/particle_types.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace fuse::relight::particles {

/// Samples of a baked channel (upstream kDefaultAnimationResolution).
inline constexpr std::uint32_t kCurveResolution = 256;

enum class TangentType : std::uint8_t { Linear, Auto, Smooth, Flat, Step, Custom };
/// "linear", "auto", "smooth", "flat", "step", "custom"; anything else is Linear (upstream parseTangentType).
TangentType parseTangentType(std::string_view token);

struct FloatCurve {
    std::vector<float> times;
    std::vector<float> values;
    std::vector<TangentType> inTangentTypes;
    std::vector<TangentType> outTangentTypes;
    std::vector<float> inTangentValues;
    std::vector<float> outTangentValues;
    std::vector<float> inTangentTimes;
    std::vector<float> outTangentTimes;
    std::vector<bool> tangentBrokens; ///< read and kept (the baker does not use it, as upstream)

    bool valid() const { return !times.empty() && !values.empty() && times.size() == values.size(); }
    bool hasFullTangents() const { return inTangentValues.size() == times.size() && outTangentValues.size() == times.size(); }
    bool hasFullTangentTimes() const { return inTangentTimes.size() == times.size() && outTangentTimes.size() == times.size(); }
};

struct ColorGradient {
    std::vector<float> times;
    std::vector<std::array<float, 4>> values;
    bool valid() const { return !times.empty() && !values.empty() && times.size() == values.size(); }
};

/// Index of the key interval holding `t` (the first / last interval outside the key range).
std::size_t findKeyframeInterval(const float* times, std::size_t count, float t);
/// Cubic Bezier segment evaluated at time `t` with Newton-Raphson on X (non-uniform tangent times).
float evalBezierFCurve(float x0, float x1, float x2, float x3, float y0, float y1, float y2, float y3, float t);

/// Bakes `curve` to `resolution` samples; `defaultValue` everywhere (and false) when the curve is invalid.
bool bakeFloatCurve(const FloatCurve& curve, std::vector<float>& out, std::uint32_t resolution = kCurveResolution,
                    float defaultValue = 0.f);
bool bakeColorGradient(const ColorGradient& gradient, std::vector<std::array<float, 4>>& out,
                       std::uint32_t resolution = kCurveResolution, std::array<float, 4> defaultValue = {1.f, 1.f, 1.f, 1.f});
/// Channel combination: per-sample the baked channel, or the default component of an absent channel; false (and the
/// default everywhere) when no channel exists.
bool combineChannels(const std::vector<const std::vector<float>*>& channels, const std::vector<bool>& present,
                     const std::vector<float>& defaults, std::vector<std::array<float, 4>>& out,
                     std::uint32_t resolution = kCurveResolution);

} // namespace fuse::relight::particles
