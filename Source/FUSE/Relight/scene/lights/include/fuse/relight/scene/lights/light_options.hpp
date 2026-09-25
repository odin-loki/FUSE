/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_light_manager.h@0867d3c (the legacy light
// translation options; names, types, defaults, limits and descriptions as upstream).
//
// RL-1.5 owns these rtx.* options (RL-0.6 registry; each also answers to its relight.* twin). Other
// packages read them from here (LightOptions::x()), never declare them again.
//
// Borrowed (declared by another package, read by name with the Remix default as fallback):
//   rtx.sceneScale   (RtxOptions::sceneScale, default 1: the owner is the scene/instances package)
// Not ported here: the fallback light (rtx.fallbackLight*) and rtx.suppressLightKeeping, which belong
// to the light manager on the render side (RL-4/5).
#pragma once

#include <fuse/relight/options/option.hpp>

#include <cfloat>

namespace fuse::relight::scene {

struct LightOptions {
    FUSE_RELIGHT_OPTION("rtx", bool, ignoreGameDirectionalLights, false, "Ignores any directional lights coming from the original game (lights added via toolkit still work).");
    FUSE_RELIGHT_OPTION("rtx", bool, ignoreGamePointLights, false, "Ignores any point lights coming from the original game (lights added via toolkit still work).");
    FUSE_RELIGHT_OPTION("rtx", bool, ignoreGameSpotLights, false, "Ignores any spot lights coming from the original game (lights added via toolkit still work).");
    FUSE_RELIGHT_OPTION("rtx", bool, calculateLightIntensityUsingLeastSquares, true, "Enable usage of least squares for approximating a light's falloff curve rather than a more basic single point approach. This will generally result in more accurate matching of the original application's custom light attenuation curves, especially with non physically based linear-style attenuation.");
    FUSE_RELIGHT_OPTION_ARGS("rtx", float, lightConversionSphereLightFixedRadius, 4.f, "The fixed radius in world units to use for legacy lights converted to sphere lights (currently point and spot lights will convert to sphere lights). Use caution with large light radii as many legacy lights will be placed close to geometry and intersect it, causing suboptimal light sampling performance or other visual artifacts (lights clipping through walls, etc).",
                             args.minValue = 0.0f);
    FUSE_RELIGHT_OPTION_ARGS("rtx", float, lightConversionDistantLightFixedIntensity, 1.0f, "The fixed intensity (in W/sr) to use for legacy lights converted to distant lights (currently directional lights will convert to distant lights).",
                             args.minValue = 0.0f);
    FUSE_RELIGHT_OPTION_ARGS("rtx", float, lightConversionDistantLightFixedAngle, 0.0349f, "The angular size in radians of the distant light source for legacy lights converted to distant lights. Set to ~2 degrees in radians by default. Should only be within the range [0, pi].",
                             args.minValue = 0.0f; args.maxValue = 3.14159265358979323846f);
    FUSE_RELIGHT_OPTION("rtx", float, lightConversionMaxIntensity, FLT_MAX, "The highest intensity value a converted light can have.");
    FUSE_RELIGHT_OPTION("rtx", float, lightConversionIntensityFactor, 1.f, "Scales the converted light intensities.");

    /// rtx.sceneScale (borrowed; 1 when no package declares it).
    static float sceneScale();
};

} // namespace fuse::relight::scene
