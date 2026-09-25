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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_lights_data.cpp@0867d3c,
// src/dxvk/rtx_render/rtx_light_utils.cpp@0867d3c and src/util/util_quat.h@0867d3c (getOrientation).
// See legacy_light.hpp. The float expression order of every hashed or intensity-affecting value is
// kept exactly as upstream (including the double-precision `4.0 * a * c` in leastSquareIntensity).
#include <fuse/relight/scene/lights/legacy_light.hpp>
#include <fuse/relight/scene/lights/light_options.hpp>

#include <fuse/relight/hash/xxh.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::relight::scene {

namespace {

/// Remix `cos(float)` on a hashed value, evaluated in double and rounded once: the correctly rounded
/// float cosine, identical on every platform and C runtime.
float stableCos(float x) { return static_cast<float>(std::cos(static_cast<double>(x))); }

float maxComponent(const tap::Color4& c) { return std::max(c.r, std::max(c.g, c.b)); }

float dot3(const Float3& a, const Float3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Float3 cross3(const Float3& a, const Float3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

// rtx_light_utils.cpp leastSquareIntensity.
float leastSquareIntensity(float intensity, float attenuation2, float attenuation1, float attenuation0, float range) {
    // Calculate the distance where light intensity attenuates to 10%
    constexpr float kEpsilon = 0.000001f;
    float lowRange = 0.0f;
    const float lowThreshold = 0.1f;
    if (attenuation2 < kEpsilon) {
        if (attenuation1 > kEpsilon) {
            lowRange = (1.0f / lowThreshold - attenuation0) / attenuation1;
        }
    } else {
        float a = attenuation2;
        float b = attenuation1;
        float c = attenuation0 - 1.0f / lowThreshold;
        // Note: upstream evaluates this in double (4.0 is a double literal) and narrows once.
        float discriminant = static_cast<float>(b * b - 4.0 * a * c);
        if (discriminant >= 0) {
            const float sqRoot = std::sqrt(discriminant);
            const float root1 = (-b + sqRoot) / (2 * a);
            const float root2 = (-b - sqRoot) / (2 * a);
            if (root1 > 0) {
                lowRange = root1;
            }
            if (root2 > 0) {
                lowRange = root2;
            }
        }
    }

    // Calculate the sample range
    if (lowRange > 0) {
        range = std::min(range, lowRange);
    }

    // Place 5 samples between [0, range]
    // Find newIntensity to minimize the error = Sigma((intensity / (a2*xi*xi + a1*xi + a0) - newIntensity / (xi * xi))^2)
    const int kSamples = 5;
    float numerator = 0;
    float denominator = 0;

    for (int i = 0; i < kSamples; i++) {
        float xi = float(i + 1) / kSamples * range;
        float xi2 = xi * xi;
        float xi4 = xi2 * xi2;
        float Ii = intensity / (attenuation2 * xi2 + attenuation1 * xi + attenuation0);
        numerator += Ii / xi2;
        denominator += 1 / xi4;
    }

    float newIntensity = numerator / denominator;
    return newIntensity;
}

float intensityToEndDistance(float intensity) {
    float endDistanceSq = intensity / kLegacyLightEndValue;
    return std::sqrt(endDistanceSq);
}

// rtx_light_utils.cpp solveQuadraticEndDistance.
float solveQuadraticEndDistance(float originalBrightness, float attenuation2, float attenuation1, float attenuation0,
                                float range) {
    const float a = attenuation2;
    const float b = attenuation1;
    const float c = attenuation0;
    float endDistance = 0.0f;

    // Solve for kLegacyLightEndValue using quadratic equation.
    // originalBrightness/(a*d*d+b*d+c) = kLegacyLightEndValue
    // a*d*d+b*d+c-originalBrightness/kLegacyLightEndValue = 0
    const float newC = c - originalBrightness / kLegacyLightEndValue;
    const float discriminant = b * b - 4 * a * newC;

    if (discriminant < 0) {
        // Attenuation never reaches kLegacyLightEndValue.  Just use range.
        endDistance = range;
    } else if (discriminant == 0) {
        const float root = -b / (2 * a);
        if (root > 0) {
            endDistance = root;
        }
    } else {
        // Two roots, use the smaller positive root.
        const float sqRoot = std::sqrt(discriminant);
        const float root1 = (-b + sqRoot) / (2 * a);
        const float root2 = (-b - sqRoot) / (2 * a);
        if (root1 > 0) {
            endDistance = root1;
        }
        if (root2 > 0) {
            endDistance = root2;
        }
    }

    return endDistance;
}

/// util_quat.h getOrientation (from Omniverse): the quaternion rotating `src` onto `dst`.
std::array<float, 4> getOrientation(Float3 src, const Float3& dst) {
    // If the rotation is larger than pi/2 then do it from the other side.
    float tmp = dot3(src, dst);
    bool flip = tmp < 0;
    if (flip) {
        src = {src[0], -src[1], -src[2]};
    }
    const Float3 v = cross3(src, dst);
    std::array<float, 4> q{}; // x, y, z, w
    q[3] = std::sqrt((1.0f + std::abs(tmp)) / 2.0f);
    const float twoW = 2.0f * q[3];
    q[0] = v[0] / twoW;
    q[1] = v[1] / twoW;
    q[2] = v[2] / twoW;
    if (flip) {
        q = {q[3], q[2], -q[1], -q[0]};
    }
    return q;
}

/// util_matrix.h Matrix4(quaternion, translation), written in D3DMATRIX layout (row r = dxvk data[r]).
std::array<float, 16> quaternionTransform(const std::array<float, 4>& q, const Float3& t) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    std::array<float, 16> m{};
    m[0] = static_cast<float>(1.0 - 2.0 * (y * y + z * z));
    m[1] = static_cast<float>(2.0 * (x * y + z * w));
    m[2] = static_cast<float>(2.0 * (z * x - y * w));
    m[4] = static_cast<float>(2.0 * (x * y - z * w));
    m[5] = static_cast<float>(1.0 - 2.0 * (z * z + x * x));
    m[6] = static_cast<float>(2.0 * (y * z + x * w));
    m[8] = static_cast<float>(2.0 * (z * x + y * w));
    m[9] = static_cast<float>(2.0 * (y * z - x * w));
    m[10] = static_cast<float>(1.0 - 2.0 * (y * y + x * x));
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    m[15] = 1.f;
    return m;
}

Float3 toFloat3(const tap::Vec3& v) { return {v.x, v.y, v.z}; }

} // namespace

bool LightRecord::isOff() const {
    const Float3& r = radiance;
    return r[0] < 0 || r[1] < 0 || r[2] < 0 || (r[0] <= 0 && r[1] <= 0 && r[2] <= 0);
}

Float3 safeNormalize(const Float3& v, const Float3& fallback) {
    const float len = std::sqrt(dot3(v, v));
    if (len == 0.0f) {
        return fallback;
    }
    const float inv = 1.0f / len;
    return {v[0] * inv, v[1] * inv, v[2] * inv};
}

float calculateLegacyLightIntensity(const tap::Light& light, const float radius) {
    constexpr float kEpsilon = 0.000001f;

    // Attenuation in D3D9 is 1/(Attenuation2*d*d + Attenuation1*d + Attenuation0), taken with respect to the
    // max component of the diffuse colour. The end distance may exceed Range: old games used Range as an
    // optimisation, physical lights must reflect the intended full distance of the attenuation curve.
    const float a = light.attenuation2;
    const float b = light.attenuation1;
    const float c = light.attenuation0;

    const float originalBrightness = maxComponent(light.diffuse);

    float endDistance = light.range;

    if (c > 0 && originalBrightness / c < kLegacyLightEndValue) {
        // Light constant is already lower than our minimum right next to the light, so just set the radiance to 0.
        endDistance = 0.f;
    } else if (a < kEpsilon) {
        // No squared attenuation term
        if (b > kEpsilon) {
            // linear falloff
            if (LightOptions::calculateLightIntensityUsingLeastSquares()) {
                endDistance = intensityToEndDistance(leastSquareIntensity(
                    originalBrightness, light.attenuation2, light.attenuation1, light.attenuation0, light.range));
            } else {
                // 1/(b*d + c) = kLegacyLightEndValue
                endDistance = ((originalBrightness / kLegacyLightEndValue) - c) / b;
            }
        }
        // else: no falloff - the light is at full power * c until the range runs out.
    } else {
        if (LightOptions::calculateLightIntensityUsingLeastSquares()) {
            endDistance = intensityToEndDistance(leastSquareIntensity(originalBrightness, light.attenuation2,
                                                                      light.attenuation1, light.attenuation0, light.range));
        } else {
            endDistance = solveQuadraticEndDistance(originalBrightness, light.attenuation2, light.attenuation1,
                                                    light.attenuation0, light.range);
        }
    }

    // Radiance of the sphere light that reaches the threshold radiance at the end distance:
    // r = (d^2 * t) / (pi * radius^2).
    const float endDistanceSq = endDistance * endDistance;
    const float kDistanceSqToRadiance = kNewLightEndValue / (kLightPi * radius * radius);

    return std::min(kDistanceSqToRadiance * endDistanceSq * LightOptions::lightConversionIntensityFactor(),
                    LightOptions::lightConversionMaxIntensity());
}

hash::Hash64 stableLightHash(const tap::Light& light) {
    // Note: Changing this code alters the "stable" light hashes from D3D9 and breaks replacement assets.
    switch (light.type) {
    case d3dlight::POINT:
    case d3dlight::SPOT: {
        const Float3 originalPosition = toFloat3(light.position);
        hash::LightShaping shaping; // disabled: shaping hash 0 for point lights
        if (light.type == d3dlight::SPOT) {
            // The stable shaping hash takes the raw D3DLIGHT9 direction (not normalized: a legacy artifact).
            const float coneAngle = light.phi / 2.0f;
            shaping.enabled = true;
            shaping.direction = toFloat3(light.direction);
            shaping.cosConeAngle = stableCos(coneAngle);
            shaping.coneSoftness = stableCos(light.theta / 2.0f) - stableCos(coneAngle);
            shaping.focusExponent = light.falloff;
        }
        return hash::hashSphereLight(originalPosition, kLegacyStableRadius, shaping);
    }
    case d3dlight::DIRECTIONAL: {
        // Note: RtLightType::Rect (not Distant) seeds this hash: an upstream refactoring mistake that shipped
        // with the public toolkit and is kept so existing replacements keep matching.
        const Float3 originalDirection = toFloat3(light.direction);
        hash::Hash64 h = static_cast<hash::Hash64>(hash::LightType::Rect);
        h = hash::xxh64(originalDirection.data(), sizeof(float) * 3, h);
        h = hash::xxh64(&kLegacyStableHalfAngle, sizeof(kLegacyStableHalfAngle), h);
        return h;
    }
    default:
        return hash::kEmptyHash;
    }
}

std::optional<LightRecord> convertLegacyLight(const tap::Light& light) {
    // Some games pass invalid light types; the RtLight requires a valid one (Remix logs and skips).
    if (light.type < d3dlight::POINT || light.type > d3dlight::DIRECTIONAL) {
        return std::nullopt;
    }

    LightRecord out;
    out.d3dIndex = light.index;
    out.d3dType = light.type;
    out.hash = stableLightHash(light);

    if (light.type == d3dlight::DIRECTIONAL) {
        // createFromDirectional + toRtLight (Distant).
        out.type = hash::LightType::Distant;
        out.direction = safeNormalize(toFloat3(light.direction), Float3{0.0f, 0.0f, 1.0f});
        const float angleRadians = LightOptions::lightConversionDistantLightFixedAngle();
        out.halfAngle = angleRadians / 2.0f;
        out.intensity = LightOptions::lightConversionDistantLightFixedIntensity();
        // calculateRadiance: m_Color * m_Intensity * pow(2, exposure 0) * temperature 1.
        out.radiance = {light.diffuse.r * out.intensity, light.diffuse.g * out.intensity, light.diffuse.b * out.intensity};
        return out;
    }

    // createFromPointSpot + toRtLight (Sphere).
    out.type = hash::LightType::Sphere;
    out.position = toFloat3(light.position);
    const float originalBrightness = maxComponent(light.diffuse);
    out.radius = LightOptions::lightConversionSphereLightFixedRadius() * LightOptions::sceneScale();
    out.intensity = calculateLegacyLightIntensity(light, out.radius);
    const Float3 color = {light.diffuse.r / originalBrightness, light.diffuse.g / originalBrightness,
                          light.diffuse.b / originalBrightness};
    out.radiance = {color[0] * out.intensity, color[1] * out.intensity, color[2] * out.intensity};

    // LightData defaults: cone 180 degrees, softness 0, focus 0, z axis (0, 0, 1) -> shaping disabled.
    Float3 zAxis{0.0f, 0.0f, 1.0f};
    float coneAngleRadians = 180.f * (kLightPi / 180.0f);
    float coneSoftness = 0.0f;
    float focus = 0.0f;
    if (light.type == d3dlight::SPOT) {
        // D3D9 spot directions need not be normalized and may be zero (fall back to +Z).
        zAxis = safeNormalize(toFloat3(light.direction), Float3{0.0f, 0.0f, 1.0f});
        // ConeAngle is the outer angle of the spotlight; ConeSoftness how far the transition region reaches.
        coneAngleRadians = light.phi / 2.0f;
        coneSoftness = stableCos(light.theta / 2.0f) - stableCos(coneAngleRadians);
        focus = light.falloff;
    }
    // LightData::getLightShaping / isShapingEnabled.
    out.shaping.enabled = coneAngleRadians != (180.f * (kLightPi / 180.0f)) || coneSoftness != 0.0f || focus != 0.0f;
    out.shaping.direction = zAxis;
    out.shaping.cosConeAngle = stableCos(coneAngleRadians);
    out.shaping.coneSoftness = coneSoftness;
    out.shaping.focusExponent = focus;
    return out;
}

std::array<float, 16> legacyLightTransform(const tap::Light& light) {
    const Float3 kZ{0.0f, 0.0f, 1.0f};
    switch (light.type) {
    case d3dlight::SPOT: {
        const Float3 zAxis = safeNormalize(toFloat3(light.direction), kZ);
        return quaternionTransform(getOrientation(Float3{0.f, 0.f, -1.f}, zAxis), toFloat3(light.position));
    }
    case d3dlight::POINT: {
        std::array<float, 16> m{};
        m[0] = m[5] = m[10] = m[15] = 1.f;
        m[12] = light.position.x;
        m[13] = light.position.y;
        m[14] = light.position.z;
        return m;
    }
    case d3dlight::DIRECTIONAL: {
        const Float3 zAxis = safeNormalize(toFloat3(light.direction), kZ);
        return quaternionTransform(getOrientation(Float3{0.f, 0.f, -1.f}, zAxis), Float3{0.f, 0.f, 0.f});
    }
    default: {
        std::array<float, 16> m{};
        m[0] = m[5] = m[10] = m[15] = 1.f;
        return m;
    }
    }
}

} // namespace fuse::relight::scene
