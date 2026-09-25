/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/dxvk/rtx_render/graph/components/animation_utils.h@0867d3c
//
// FUSE Relight RL-3.5: easing and looping shared by Remap, Loop and MeshProximity (definitions in
// src/components_state.cpp).
#pragma once

#include <fuse/relight/logic/graph_types.hpp>

#include <cstdint>
#include <utility>

namespace fuse::relight::logic {

enum class LoopingType : std::uint32_t {
    Loop = 0,
    PingPong = 1,
    NoLoop = 2,
    Clamp = 3,
};

enum class InterpolationType : std::uint32_t {
    Linear = 0,
    Cubic = 1,
    EaseIn = 2,
    EaseOut = 3,
    EaseInOut = 4,
    Sine = 5,
    Exponential = 6,
    Bounce = 7,
    Elastic = 8,
};

/// USD enum tokens (e.g. "PingPong") -> values, as the components' enumValues.
const PropertySpec::EnumPropertyMap& loopingTypeEnumValues();
const PropertySpec::EnumPropertyMap& interpolationTypeEnumValues();

/// Easing of a normalized time value (0-1).
float applyInterpolation(InterpolationType interpolation, float time);
/// Maps `value` into [minRange, maxRange] by the looping type; second = in the reverse phase of ping-pong.
std::pair<float, bool> applyLooping(float value, float minRange, float maxRange, LoopingType loopingType);

} // namespace fuse::relight::logic
