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
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_lights_data.cpp@0867d3c (LightData::tryCreate,
// createFromPointSpot, createFromDirectional, toRtLight, calculateRadiance, getLightShaping,
// isShapingEnabled) and src/dxvk/rtx_render/rtx_light_utils.cpp@0867d3c (LightUtils::
// calculateIntensity, leastSquareIntensity, solveQuadraticEndDistance, getLightTransform).
//
// D3DLIGHT9 -> light record (plan §1.5, §4.1.4). A game light becomes an RtSphereLight (point and
// spot; spot lights get shaping) or an RtDistantLight (directional), with Remix's "stable" D3D9 light
// hash: the key a Remix mod uses to replace the light (light_<hash> prims, replacement DB key
// remix.light). FUSE changes: plain value types instead of LightData / RtLight; cosines are taken in
// double precision and rounded to float so the hash inputs are identical on every platform.
#pragma once

#include <fuse/relight/hash/light_hash.hpp>
#include <fuse/relight/tap/relight_tap.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace fuse::relight::scene {

/// D3DLIGHTTYPE.
namespace d3dlight {
inline constexpr std::uint32_t POINT = 1, SPOT = 2, DIRECTIONAL = 3;
}

/// The brightness the original light has to fall to (sRGB, 1/255) and the one the new light reaches at
/// the same distance (rtx_lights.h kLegacyLightEndValue / kNewLightEndValue).
inline constexpr float kLegacyLightEndValue = 1.0f / 255.0f;
inline constexpr float kNewLightEndValue = 0.01f;
/// dxvk kPi (float).
inline constexpr float kLightPi = 3.14159265358979323846f;
/// The radius / half angle Remix folds into the stable D3D9 light hash, whatever the options say
/// (rtx_lights_data.cpp: "legacy artifact" constants).
inline constexpr float kLegacyStableRadius = 4.0f;
inline constexpr float kLegacyStableHalfAngle = 0.0349f / 2.0f;

using Float3 = std::array<float, 3>;

/// A converted game light: the RtLight Remix builds from a D3DLIGHT9 (fields of RtSphereLight or
/// RtDistantLight, by `type`).
struct LightRecord {
    std::uint32_t d3dIndex = 0; ///< SetLight index
    std::uint32_t d3dType = 0;  ///< D3DLIGHTTYPE
    hash::LightType type = hash::LightType::Sphere;
    /// Sphere: position (the D3DLIGHT9 position) and radius (rtx.lightConversionSphereLightFixedRadius *
    /// rtx.sceneScale).
    Float3 position{0.f, 0.f, 0.f};
    float radius = 0.f;
    /// Sphere shaping (spot lights): normalized axis, cos(phi / 2), cos(theta / 2) - cos(phi / 2), falloff.
    hash::LightShaping shaping;
    /// Distant: normalized direction and half angle (rtx.lightConversionDistantLightFixedAngle / 2).
    Float3 direction{0.f, 0.f, 1.f};
    float halfAngle = 0.f;
    /// LightData::calculateRadiance: colour * intensity.
    Float3 radiance{0.f, 0.f, 0.f};
    /// Sphere: the intensity LightUtils::calculateIntensity derived from the attenuation curve.
    float intensity = 0.f;
    float volumetricRadianceScale = 1.f;
    /// RtLight::getInitialHash() of a D3D9 light: the stable hash (stableLightHash()).
    hash::Hash64 hash = hash::kEmptyHash;

    /// LightManager::addLight: a light with a negative radiance component, or no positive one, is "off"
    /// (subtractive lights of old games) and is not added.
    bool isOff() const;
};

/// LightUtils::calculateIntensity: the intensity a sphere light of `radius` needs to reach
/// kNewLightEndValue where the D3D9 attenuation curve falls below kLegacyLightEndValue (reads
/// rtx.calculateLightIntensityUsingLeastSquares, rtx.lightConversionIntensityFactor and
/// rtx.lightConversionMaxIntensity).
float calculateLegacyLightIntensity(const tap::Light& light, float radius);

/// The stable hash of a D3D9 light (LightData::createFromPointSpot / createFromDirectional):
/// point / spot: RtSphereLight hash of the raw position, radius 4 and (spot) a shaping hash over the
/// raw (unnormalized) direction, cos(phi / 2), cos(theta / 2) - cos(phi / 2) and falloff;
/// directional: seeded with RtLightType::Rect (an upstream artifact kept for compatibility), XXH64 of
/// the raw direction and half angle 0.0349 / 2. 0 for an invalid light type.
hash::Hash64 stableLightHash(const tap::Light& light);

/// LightData::tryCreate(light)->toRtLight(): nothing for an invalid light type (Remix logs and skips).
std::optional<LightRecord> convertLegacyLight(const tap::Light& light);

/// LightUtils::getLightTransform: the best-fit object-to-world transform of a legacy light (row-major
/// D3DMATRIX layout, m[row * 4 + col]); replacement lights with a relative transform are placed with
/// it. Point: translation; spot: orientation (-Z to the direction) plus translation; directional:
/// orientation only; identity otherwise.
std::array<float, 16> legacyLightTransform(const tap::Light& light);

/// Remix safeNormalize(v, fallback): v / |v|, or `fallback` for a zero-length vector.
Float3 safeNormalize(const Float3& v, const Float3& fallback);

} // namespace fuse::relight::scene
