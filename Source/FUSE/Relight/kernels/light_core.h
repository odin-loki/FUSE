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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// The D3DLIGHT9 conversion (rlConvertD3dLight and its helpers) is the device port of
// Source/FUSE/Relight/scene/lights/src/legacy_light.cpp (RL-1.5), itself ported from dxvk-remix
// src/dxvk/rtx_render/rtx_lights_data.cpp@0867d3c (createFromPointSpot / createFromDirectional / toRtLight) and
// src/dxvk/rtx_render/rtx_light_utils.cpp@0867d3c (calculateIntensity, leastSquareIntensity,
// solveQuadraticEndDistance), with the same float expression order. Everything else in this file (the light record,
// shaping evaluation, sampling, pdfs) is FUSE's own code written from the public UsdLux schema and the papers cited
// below; no NVIDIA shader source was used.
//
// FUSE Relight RL-4.4: the Relight light model (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.2, §5.3, §5.8).
//
// SINGLE SOURCE. This file is compiled three ways, with the dialect macros set by the includer:
//   C++   kernels/light_cpp.hpp        the CPU reference (compute_kernel bodies in kernels/light_kernels.hpp);
//   Slang render/lights/shaders/rl_lights.slang;
//   GLSL  render/lights/shaders/rl_lights.glsl (the fallback, for glslangValidator-only hosts).
// Common subset of the three (HLSL-style type names; GLSL maps float3 -> vec3 by #define): no `const` locals, no
// lerp / mix / saturate, f-suffixed literals, every struct field assigned before use, no GLSL / HLSL keywords
// (sample, input, output, filter, point, line, triangle, ...) as identifiers.
// Required macros: FUSE_RL_FN (function prefix), FUSE_RL_CONST (global constant), FUSE_RL_OUT(T),
// FUSE_RL_LIGHT_WORDS(name) / FUSE_RL_D3D_WORDS(name) (the packed records: float4[6] / float4[5]),
// FUSE_RL_STABLE_COS(x) (cos of a hashed / converted angle: C++ evaluates it in double and rounds once, exactly as
// RL-1.5; the GPU uses its float cos), FUSE_RL_DISCRIMINANT(a, b, c) (upstream's `b * b - 4.0 * a * c`, a double
// expression: C++ evaluates it in double, the GPU in float).
//
// LIGHT MODEL (RlLight; kinds after UsdLux / Remix's RtLight types):
//   sphere     centre `position`, `radius`; emits radiance `radiance` from its whole surface (outward).
//   rect       centre `position`, half axes `u`, `v` (orthogonal; lit side = +(u x v)), one-sided unless
//              kRlFlagTwoSided. UsdLux RectLight: u = X * width / 2, v = -Y * height / 2 (emits along -Z).
//   disk       centre `position`, radii `u`, `v` (orthogonal; lit side = +(u x v)).
//   cylinder   centre `position`, `u` = axis x half length, `radius`; the lateral surface emits outward (UsdLux
//              CylinderLight: the caps do not emit; FUSE treats them as absent).
//   distant    `u` = unit direction the light travels, `radius` = half angle (rad). `radiance` is the irradiance at
//              normal incidence (the D3D conversion's colour x intensity, UsdLux DistantLight intensity): spread
//              uniformly over the cone of solid angle `area` = 2 pi (1 - cos halfAngle) as radiance / area; a zero
//              half angle is a delta light.
//   triangle   emissive triangle: `position` = v0, `u` = v1 - v0, `v` = v2 - v0 (lit side = +(u x v), counter-clockwise
//              seen from it), one-sided unless kRlFlagTwoSided; `radiance` = the material's emissive radiance.
// `area` holds the emitting area (sphere 4 pi r^2, rect 4 |u x v|, disk pi |u x v|, cylinder 4 pi r h, triangle
// |u x v| / 2) or, for distant lights, the cone solid angle.
//
// SHAPING (kRlFlagShaped; UsdLux ShapingAPI / Remix light shaping): axis `axis`, cone half angle with cosine `cosCone`,
// `softness` (a cosine delta: full intensity from cos = cosCone + softness inwards) and `focus` (cos^focus). FUSE
// evaluates it for the direction from the light's CENTRE to the shaded point (rlShaping): the WP-7.1 tree's spot
// proxy then bounds the emission exactly (zero importance outside the cone <=> zero radiance), which keeps the light
// set unbiased. For the D3D9 spot conversion: cosCone = cos(phi / 2), softness = cos(theta / 2) - cos(phi / 2),
// focus = D3D falloff, i.e. smoothstep(cos(phi/2), cos(theta/2), cos) x cos^falloff.
//
// SAMPLING (solid angle densities at the shaded point p; all sampling routines are the oracles of §5.8):
//   sphere     uniform cone sampling of the subtended cone [Shirley, Wang and Zimmerman 1996, "Monte Carlo Techniques
//              for Direct Lighting Calculations", ACM TOG 15(1)]; p inside the sphere: uniform sphere, no emission.
//   rect, disk uniform area sampling (disk: Shirley-Chiu concentric mapping), pdf = d^2 / (|cos theta_l| A).
//   triangle   uniform area sampling (sqrt barycentric warp), same pdf.
//   cylinder   uniform area sampling of the lateral surface; the direction density sums both intersections of the
//              ray with the tube (d^2 / (|cos| A) each), and the sample reports the first hit's radiance, so
//              sample / pdf / eval are consistent as a directional estimator.
//   distant    uniform cone sampling of the angular cone; delta for a zero half angle (pdf 1, kRlSampleDelta).
// rlLightPdf(L, p, wi) is the density rlLightSample produces wi with; rlLightEval(L, p, wi) the radiance of L seen
// along wi (0 when the ray misses L or sees its back). Light selection is the WP-7.1 light tree (the tree proxies are
// built by render/lights/src/light_set.cpp), so the light-set density is pmf(light | p, n) x rlLightPdf.
//
// PACKED RECORDS. RlLight: float4[6] (kRlLightWords), RlD3dLight (the raw D3DLIGHT9 of a game light): float4[5]
// (kRlD3dWords). Integer fields (kind, flags, D3D type) are stored as floats (exact below 2^24).

// ---- constants --------------------------------------------------------------------------------------------------

FUSE_RL_CONST float kRlPi = 3.14159265358979f;
FUSE_RL_CONST float kRlTwoPi = 6.28318530717959f;
FUSE_RL_CONST float kRlInvFourPi = 0.0795774715459477f;
/// Distance reported for delta / distant samples.
FUSE_RL_CONST float kRlFarDistance = 1e30f;

FUSE_RL_CONST uint kRlLightWords = 6u;
FUSE_RL_CONST uint kRlD3dWords = 5u;
/// Light-set sampling batch ("relight.lights.sample", RlSetSampleKernel): float4 words per query / result.
///   query  q0 = (p, u0), q1 = (n, u1) (n = 0: no normal), q2 = (evalWi, u2), q3 = (evalLight or -1, 0, 0, 0)
///   result r0 = (position, light or -1), r1 = (wi, pmf x pdf; delta: pmf), r2 = (radiance, pmf),
///          r3 = (radiance of evalLight along evalWi, pmf x pdf of evalLight for evalWi), r4 = (dist, flags, pdf, 0)
FUSE_RL_CONST uint kRlQueryWords = 4u;
FUSE_RL_CONST uint kRlResultWords = 5u;

// RlLight.kind
FUSE_RL_CONST uint kRlKindNone = 0u;
FUSE_RL_CONST uint kRlKindSphere = 1u;
FUSE_RL_CONST uint kRlKindRect = 2u;
FUSE_RL_CONST uint kRlKindDisk = 3u;
FUSE_RL_CONST uint kRlKindCylinder = 4u;
FUSE_RL_CONST uint kRlKindDistant = 5u;
FUSE_RL_CONST uint kRlKindTriangle = 6u;
FUSE_RL_CONST uint kRlKindCount = 7u;
// RlLight.flags
FUSE_RL_CONST uint kRlFlagTwoSided = 1u;
FUSE_RL_CONST uint kRlFlagShaped = 2u;
// RlLightSample.flags
FUSE_RL_CONST uint kRlSampleValid = 1u;
FUSE_RL_CONST uint kRlSampleDelta = 2u;

// D3DLIGHTTYPE
FUSE_RL_CONST uint kRlD3dPoint = 1u;
FUSE_RL_CONST uint kRlD3dSpot = 2u;
FUSE_RL_CONST uint kRlD3dDirectional = 3u;

// RL-1.5 / rtx_lights.h constants (the float values RL-1.5 computes: 1.0f / 255.0f, and the float pi).
FUSE_RL_CONST float kRlLegacyLightEndValue = 0.003921568859368563f;
FUSE_RL_CONST float kRlNewLightEndValue = 0.01f;
FUSE_RL_CONST float kRlLightPi = 3.1415927410125732f;

// ---- records ----------------------------------------------------------------------------------------------------

struct RlLight {
    uint kind;
    uint flags;
    float3 position; ///< centre (sphere, rect, disk, cylinder) / v0 (triangle)
    float3 u;        ///< see the light model above
    float3 v;
    float radius;    ///< sphere / cylinder radius; distant: half angle (rad)
    float3 radiance;
    float area;      ///< emitting area; distant: cone solid angle
    float3 axis;     ///< shaping axis (unit)
    float cosCone;
    float softness;
    float focus;
    float volumetricScale; ///< UsdLux / Remix volumetric_radiance_scale (carried for the volumetrics pass)
};

/// One D3DLIGHT9 (tap::Light) as the GPU conversion reads it.
struct RlD3dLight {
    uint type;       ///< D3DLIGHTTYPE
    float3 diffuse;
    float3 position;
    float range;
    float3 direction;
    float falloff;
    float attenuation0;
    float attenuation1;
    float attenuation2;
    float theta;
    float phi;
};

/// rtx.lightConversion* (read on the host from RL-1.5's LightOptions).
struct RlConvertParams {
    float sphereRadius;      ///< rtx.lightConversionSphereLightFixedRadius x rtx.sceneScale
    float intensityFactor;   ///< rtx.lightConversionIntensityFactor
    float maxIntensity;      ///< rtx.lightConversionMaxIntensity
    float distantIntensity;  ///< rtx.lightConversionDistantLightFixedIntensity
    float distantAngle;      ///< rtx.lightConversionDistantLightFixedAngle (rad, full angle)
    uint leastSquares;       ///< rtx.calculateLightIntensityUsingLeastSquares
};

struct RlLightSample {
    float3 position; ///< point on the light (distant: p + wi x kRlFarDistance)
    float3 wi;       ///< unit direction from the shaded point to the light
    float dist;
    float3 radiance; ///< radiance arriving along wi (delta distant: irradiance)
    float pdf;       ///< solid angle density (delta: 1)
    uint flags;      ///< kRlSample*
};

// ---- small helpers ----------------------------------------------------------------------------------------------

FUSE_RL_FN float rlMaxComponent(float3 c) { return max(c.x, max(c.y, c.z)); }

FUSE_RL_FN float3 rlSafeNormalize(float3 v, float3 fallback) {
    float len = sqrt(dot(v, v));
    if (len == 0.0f) {
        return fallback;
    }
    float inv = 1.0f / len;
    return float3(v.x * inv, v.y * inv, v.z * inv);
}

/// Orthonormal basis around a unit vector [Duff, Burgess, Christensen, Hery, Kensler, Liani and Villemin 2017,
/// "Building an Orthonormal Basis, Revisited", JCGT 6(1)].
FUSE_RL_FN void rlBasis(float3 w, FUSE_RL_OUT(float3) t, FUSE_RL_OUT(float3) b) {
    float s = w.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (s + w.z);
    float bb = w.x * w.y * a;
    t = float3(1.0f + s * w.x * w.x * a, s * bb, -s * w.x);
    b = float3(bb, s + w.y * w.y * a, -w.y);
}

/// Shirley-Chiu concentric square -> unit disk mapping.
FUSE_RL_FN void rlConcentric(float u1, float u2, FUSE_RL_OUT(float) x, FUSE_RL_OUT(float) y) {
    float a = u1 * 2.0f - 1.0f;
    float b = u2 * 2.0f - 1.0f;
    x = 0.0f;
    y = 0.0f;
    if (a == 0.0f && b == 0.0f) {
        return;
    }
    if (abs(a) > abs(b)) {
        float t = (b / a) * 0.785398163397448f;
        x = a * cos(t);
        y = a * sin(t);
    } else {
        float t = (a / b) * 0.785398163397448f;
        x = b * sin(t);
        y = b * cos(t);
    }
}

FUSE_RL_FN bool rlIsPlanar(uint kind) { return kind == kRlKindRect || kind == kRlKindDisk || kind == kRlKindTriangle; }

/// Unit lit-side normal of a planar light.
FUSE_RL_FN float3 rlPlanarNormal(RlLight L) { return rlSafeNormalize(cross(L.u, L.v), float3(0.0f, 0.0f, 1.0f)); }

/// The point shaping is measured from (the light's centre).
FUSE_RL_FN float3 rlLightCenter(RlLight L) {
    if (L.kind == kRlKindTriangle) {
        return L.position + (L.u + L.v) * (1.0f / 3.0f);
    }
    return L.position;
}

/// Angular shaping for the direction from the light to the shaded point (see SHAPING above).
FUSE_RL_FN float rlShaping(RlLight L, float3 dirFromLight) {
    if ((L.flags & kRlFlagShaped) == 0u) {
        return 1.0f;
    }
    float c = dot(L.axis, dirFromLight);
    if (c < L.cosCone) {
        return 0.0f;
    }
    float s = 1.0f;
    if (L.softness > 0.0f) {
        float t = clamp((c - L.cosCone) / L.softness, 0.0f, 1.0f);
        s = t * t * (3.0f - 2.0f * t);
    }
    if (L.focus > 0.0f) {
        s = s * pow(max(c, 0.0f), L.focus);
    }
    return s;
}

/// Shaping at shaded point p (direction centre -> p; the axis when p is the centre).
FUSE_RL_FN float rlShapingAt(RlLight L, float3 p) {
    if ((L.flags & kRlFlagShaped) == 0u) {
        return 1.0f;
    }
    return rlShaping(L, rlSafeNormalize(p - rlLightCenter(L), L.axis));
}

/// 1 - cos(half angle) of a distant light's cone (area / 2 pi).
FUSE_RL_FN float rlDistantOneMinusCos(RlLight L) { return L.area * (1.0f / kRlTwoPi); }

// ---- packing ----------------------------------------------------------------------------------------------------

FUSE_RL_FN RlLight rlLightUnpack(FUSE_RL_LIGHT_WORDS(w)) {
    RlLight L;
    L.position = float3(w[0].x, w[0].y, w[0].z);
    L.kind = uint(w[0].w);
    L.u = float3(w[1].x, w[1].y, w[1].z);
    L.radius = w[1].w;
    L.v = float3(w[2].x, w[2].y, w[2].z);
    L.flags = uint(w[2].w);
    L.radiance = float3(w[3].x, w[3].y, w[3].z);
    L.area = w[3].w;
    L.axis = float3(w[4].x, w[4].y, w[4].z);
    L.cosCone = w[4].w;
    L.softness = w[5].x;
    L.focus = w[5].y;
    L.volumetricScale = w[5].z;
    return L;
}

/// Word i (0..5) of the packed record (inverse of rlLightUnpack; w[5].w = 0).
FUSE_RL_FN float4 rlLightWord(RlLight L, uint i) {
    if (i == 0u) {
        return float4(L.position.x, L.position.y, L.position.z, float(L.kind));
    }
    if (i == 1u) {
        return float4(L.u.x, L.u.y, L.u.z, L.radius);
    }
    if (i == 2u) {
        return float4(L.v.x, L.v.y, L.v.z, float(L.flags));
    }
    if (i == 3u) {
        return float4(L.radiance.x, L.radiance.y, L.radiance.z, L.area);
    }
    if (i == 4u) {
        return float4(L.axis.x, L.axis.y, L.axis.z, L.cosCone);
    }
    return float4(L.softness, L.focus, L.volumetricScale, 0.0f);
}

FUSE_RL_FN RlD3dLight rlD3dLightUnpack(FUSE_RL_D3D_WORDS(w)) {
    RlD3dLight d;
    d.type = uint(w[0].x);
    d.diffuse = float3(w[0].y, w[0].z, w[0].w);
    d.position = float3(w[1].x, w[1].y, w[1].z);
    d.range = w[1].w;
    d.direction = float3(w[2].x, w[2].y, w[2].z);
    d.falloff = w[2].w;
    d.attenuation0 = w[3].x;
    d.attenuation1 = w[3].y;
    d.attenuation2 = w[3].z;
    d.theta = w[3].w;
    d.phi = w[4].x;
    return d;
}

/// A record with every field set: kind none, no emission.
FUSE_RL_FN RlLight rlLightNone() {
    RlLight L;
    L.kind = kRlKindNone;
    L.flags = 0u;
    L.position = float3(0.0f);
    L.u = float3(0.0f);
    L.v = float3(0.0f);
    L.radius = 0.0f;
    L.radiance = float3(0.0f);
    L.area = 0.0f;
    L.axis = float3(0.0f, 0.0f, 1.0f);
    L.cosCone = -1.0f;
    L.softness = 0.0f;
    L.focus = 0.0f;
    L.volumetricScale = 1.0f;
    return L;
}

/// Emitting area / distant solid angle of a record whose geometry is set (the host and the GPU conversion call it).
FUSE_RL_FN float rlLightArea(RlLight L) {
    if (L.kind == kRlKindSphere) {
        return 4.0f * kRlPi * L.radius * L.radius;
    }
    if (L.kind == kRlKindRect) {
        return 4.0f * length(cross(L.u, L.v));
    }
    if (L.kind == kRlKindDisk) {
        return kRlPi * length(cross(L.u, L.v));
    }
    if (L.kind == kRlKindCylinder) {
        return 4.0f * kRlPi * L.radius * length(L.u);
    }
    if (L.kind == kRlKindTriangle) {
        return 0.5f * length(cross(L.u, L.v));
    }
    if (L.kind == kRlKindDistant) {
        // 2 pi (1 - cos h) = 4 pi sin^2(h / 2): no cancellation for small angles.
        float s = sin(L.radius * 0.5f);
        return 4.0f * kRlPi * s * s;
    }
    return 0.0f;
}

// ---- D3DLIGHT9 conversion (port of RL-1.5 legacy_light.cpp; see the header comment) -------------------------------

FUSE_RL_FN float rlLeastSquareIntensity(float intensity, float attenuation2, float attenuation1, float attenuation0,
                                        float range) {
    float kEpsilon = 0.000001f;
    float lowRange = 0.0f;
    float lowThreshold = 0.1f;
    if (attenuation2 < kEpsilon) {
        if (attenuation1 > kEpsilon) {
            lowRange = (1.0f / lowThreshold - attenuation0) / attenuation1;
        }
    } else {
        float a = attenuation2;
        float b = attenuation1;
        float c = attenuation0 - 1.0f / lowThreshold;
        float discriminant = FUSE_RL_DISCRIMINANT(a, b, c);
        if (discriminant >= 0.0f) {
            float sqRoot = sqrt(discriminant);
            float root1 = (-b + sqRoot) / (2.0f * a);
            float root2 = (-b - sqRoot) / (2.0f * a);
            if (root1 > 0.0f) {
                lowRange = root1;
            }
            if (root2 > 0.0f) {
                lowRange = root2;
            }
        }
    }
    if (lowRange > 0.0f) {
        range = min(range, lowRange);
    }
    float numerator = 0.0f;
    float denominator = 0.0f;
    for (int i = 0; i < 5; i++) {
        float xi = float(i + 1) / 5.0f * range;
        float xi2 = xi * xi;
        float xi4 = xi2 * xi2;
        float Ii = intensity / (attenuation2 * xi2 + attenuation1 * xi + attenuation0);
        numerator += Ii / xi2;
        denominator += 1.0f / xi4;
    }
    return numerator / denominator;
}

FUSE_RL_FN float rlIntensityToEndDistance(float intensity) { return sqrt(intensity / kRlLegacyLightEndValue); }

FUSE_RL_FN float rlSolveQuadraticEndDistance(float originalBrightness, float attenuation2, float attenuation1,
                                             float attenuation0, float range) {
    float a = attenuation2;
    float b = attenuation1;
    float c = attenuation0;
    float endDistance = 0.0f;
    float newC = c - originalBrightness / kRlLegacyLightEndValue;
    float discriminant = b * b - 4.0f * a * newC;
    if (discriminant < 0.0f) {
        endDistance = range;
    } else if (discriminant == 0.0f) {
        float root = -b / (2.0f * a);
        if (root > 0.0f) {
            endDistance = root;
        }
    } else {
        float sqRoot = sqrt(discriminant);
        float root1 = (-b + sqRoot) / (2.0f * a);
        float root2 = (-b - sqRoot) / (2.0f * a);
        if (root1 > 0.0f) {
            endDistance = root1;
        }
        if (root2 > 0.0f) {
            endDistance = root2;
        }
    }
    return endDistance;
}

/// LightUtils::calculateIntensity (RL-1.5 calculateLegacyLightIntensity).
FUSE_RL_FN float rlLegacyLightIntensity(RlD3dLight d, float radius, RlConvertParams prm) {
    float kEpsilon = 0.000001f;
    float a = d.attenuation2;
    float b = d.attenuation1;
    float c = d.attenuation0;
    float originalBrightness = rlMaxComponent(d.diffuse);
    float endDistance = d.range;
    if (c > 0.0f && originalBrightness / c < kRlLegacyLightEndValue) {
        endDistance = 0.0f;
    } else if (a < kEpsilon) {
        if (b > kEpsilon) {
            if (prm.leastSquares != 0u) {
                endDistance = rlIntensityToEndDistance(rlLeastSquareIntensity(originalBrightness, d.attenuation2,
                                                                              d.attenuation1, d.attenuation0, d.range));
            } else {
                endDistance = ((originalBrightness / kRlLegacyLightEndValue) - c) / b;
            }
        }
    } else {
        if (prm.leastSquares != 0u) {
            endDistance = rlIntensityToEndDistance(rlLeastSquareIntensity(originalBrightness, d.attenuation2,
                                                                          d.attenuation1, d.attenuation0, d.range));
        } else {
            endDistance = rlSolveQuadraticEndDistance(originalBrightness, d.attenuation2, d.attenuation1,
                                                      d.attenuation0, d.range);
        }
    }
    float endDistanceSq = endDistance * endDistance;
    float kDistanceSqToRadiance = kRlNewLightEndValue / (kRlLightPi * radius * radius);
    return min(kDistanceSqToRadiance * endDistanceSq * prm.intensityFactor, prm.maxIntensity);
}

/// LightRecord::isOff: a negative radiance component, or no positive one.
FUSE_RL_FN bool rlRadianceOff(float3 r) {
    return r.x < 0.0f || r.y < 0.0f || r.z < 0.0f || (r.x <= 0.0f && r.y <= 0.0f && r.z <= 0.0f);
}

/// D3DLIGHT9 -> RlLight (RL-1.5 convertLegacyLight): point / spot -> sphere (spot shaped), directional -> distant.
/// Invalid types and "off" lights (rlRadianceOff) give kind none.
FUSE_RL_FN RlLight rlConvertD3dLight(RlD3dLight d, RlConvertParams prm) {
    RlLight L = rlLightNone();
    if (d.type < kRlD3dPoint || d.type > kRlD3dDirectional) {
        return L;
    }
    if (d.type == kRlD3dDirectional) {
        L.kind = kRlKindDistant;
        L.u = rlSafeNormalize(d.direction, float3(0.0f, 0.0f, 1.0f));
        L.radius = prm.distantAngle / 2.0f;
        float intensity = prm.distantIntensity;
        L.radiance = float3(d.diffuse.x * intensity, d.diffuse.y * intensity, d.diffuse.z * intensity);
        L.area = rlLightArea(L);
    } else {
        L.kind = kRlKindSphere;
        L.position = d.position;
        float originalBrightness = rlMaxComponent(d.diffuse);
        L.radius = prm.sphereRadius;
        float intensity = rlLegacyLightIntensity(d, L.radius, prm);
        float3 color = float3(d.diffuse.x / originalBrightness, d.diffuse.y / originalBrightness,
                              d.diffuse.z / originalBrightness);
        L.radiance = float3(color.x * intensity, color.y * intensity, color.z * intensity);
        float3 zAxis = float3(0.0f, 0.0f, 1.0f);
        float coneAngleRadians = kRlLightPi;
        float coneSoftness = 0.0f;
        float focus = 0.0f;
        if (d.type == kRlD3dSpot) {
            zAxis = rlSafeNormalize(d.direction, float3(0.0f, 0.0f, 1.0f));
            coneAngleRadians = d.phi / 2.0f;
            coneSoftness = FUSE_RL_STABLE_COS(d.theta / 2.0f) - FUSE_RL_STABLE_COS(coneAngleRadians);
            focus = d.falloff;
        }
        bool shaped = coneAngleRadians != kRlLightPi || coneSoftness != 0.0f || focus != 0.0f;
        L.flags = shaped ? kRlFlagShaped : 0u;
        L.axis = zAxis;
        L.cosCone = FUSE_RL_STABLE_COS(coneAngleRadians);
        L.softness = coneSoftness;
        L.focus = focus;
        L.area = rlLightArea(L);
    }
    if (rlRadianceOff(L.radiance)) {
        L = rlLightNone();
    }
    return L;
}

// ---- intersection helpers ---------------------------------------------------------------------------------------

/// Ray p + t wi against a planar light: t (> 0) of the hit inside the light's outline, -1 on a miss.
FUSE_RL_FN float rlPlanarHit(RlLight L, float3 p, float3 wi) {
    float3 nu = cross(L.u, L.v);
    float denom = dot(nu, wi);
    if (denom == 0.0f) {
        return -1.0f;
    }
    float t = dot(nu, L.position - p) / denom;
    if (!(t > 0.0f)) {
        return -1.0f;
    }
    float3 h = p + wi * t - L.position;
    if (L.kind == kRlKindTriangle) {
        float nn = dot(nu, nu);
        float b1 = dot(cross(h, L.v), nu) / nn;
        float b2 = dot(cross(L.u, h), nu) / nn;
        if (b1 < 0.0f || b2 < 0.0f || b1 + b2 > 1.0f) {
            return -1.0f;
        }
        return t;
    }
    float a = dot(h, L.u) / dot(L.u, L.u);
    float b = dot(h, L.v) / dot(L.v, L.v);
    if (L.kind == kRlKindRect) {
        if (abs(a) > 1.0f || abs(b) > 1.0f) {
            return -1.0f;
        }
        return t;
    }
    if (a * a + b * b > 1.0f) {
        return -1.0f;
    }
    return t;
}

/// Ray p + t wi against a cylinder light's lateral surface: the nearest hit (tNear, its outward normal nNear) and the
/// direction density of the area sampler, summed over both hits. tNear = -1 on a miss.
struct RlCylinderHits {
    float tNear;
    float3 nNear;
    float pdf;
};

FUSE_RL_FN RlCylinderHits rlCylinderHits(RlLight L, float3 p, float3 wi) {
    RlCylinderHits r;
    r.tNear = -1.0f;
    r.nNear = float3(0.0f);
    r.pdf = 0.0f;
    float h = length(L.u);
    if (!(h > 0.0f) || !(L.radius > 0.0f) || !(L.area > 0.0f)) {
        return r;
    }
    float3 A = L.u * (1.0f / h);
    float3 o = p - L.position;
    float oa = dot(o, A);
    float wa = dot(wi, A);
    float3 op = o - A * oa;
    float3 wp = wi - A * wa;
    float a = dot(wp, wp);
    if (!(a > 0.0f)) {
        return r;
    }
    float b = dot(op, wp);
    // disc = b^2 - a (|op|^2 - r^2) = a (r^2 - |l|^2), l = the axis-to-ray offset at closest approach: no cancellation
    // of the large |op|^2 terms near tangency [Haines, Guenther and Akenine-Moller 2019, "Precision Improvements for
    // Ray / Sphere Intersection", Ray Tracing Gems ch. 7].
    float3 l = op - wp * (b / a);
    float disc = a * (L.radius * L.radius - dot(l, l));
    if (disc < 0.0f) {
        return r;
    }
    float sq = sqrt(disc);
    for (int k = 0; k < 2; k++) {
        float t = k == 0 ? (-b - sq) / a : (-b + sq) / a;
        if (!(t > 0.0f)) {
            continue;
        }
        float z = oa + t * wa;
        if (abs(z) > h) {
            continue;
        }
        float3 n = (op + wp * t) * (1.0f / L.radius);
        // |n . wi| = |b + t a| / r = sqrt(disc) / r for both roots (n is perpendicular to the axis, so n . wi =
        // n . wp): exact algebra, and well conditioned near tangency where dot(n, wi) of the rounded hit point is not.
        float cosL = sq / L.radius;
        if (!(cosL > 0.0f)) {
            continue;
        }
        r.pdf += t * t / (cosL * L.area);
        if (r.tNear < 0.0f) {
            r.tNear = t;
            r.nNear = n;
        }
    }
    return r;
}

// ---- eval / pdf / sample ----------------------------------------------------------------------------------------

/// Radiance of L seen from p along the unit direction wi (0 on a miss or the back of a one-sided emitter).
FUSE_RL_FN float3 rlLightEval(RlLight L, float3 p, float3 wi) {
    float3 zero = float3(0.0f);
    if (L.kind == kRlKindSphere) {
        float3 d = L.position - p;
        float r2 = L.radius * L.radius;
        if (dot(d, d) <= r2) {
            return zero;
        }
        float bproj = dot(wi, d);
        if (!(bproj > 0.0f)) {
            return zero;
        }
        float3 q = d - wi * bproj;
        if (r2 - dot(q, q) < 0.0f) {
            return zero;
        }
        return L.radiance * rlShapingAt(L, p);
    }
    if (rlIsPlanar(L.kind)) {
        if (rlPlanarHit(L, p, wi) < 0.0f) {
            return zero;
        }
        float cosL = -dot(rlPlanarNormal(L), wi);
        if (!(cosL > 0.0f) && (L.flags & kRlFlagTwoSided) == 0u) {
            return zero;
        }
        return L.radiance * rlShapingAt(L, p);
    }
    if (L.kind == kRlKindCylinder) {
        RlCylinderHits hits = rlCylinderHits(L, p, wi);
        if (hits.tNear < 0.0f || !(dot(hits.nNear, wi) < 0.0f)) {
            return zero;
        }
        return L.radiance * rlShapingAt(L, p);
    }
    if (L.kind == kRlKindDistant) {
        if (!(L.area > 0.0f)) {
            return zero;
        }
        if (dot(wi, -L.u) < 1.0f - rlDistantOneMinusCos(L)) {
            return zero;
        }
        return L.radiance * (1.0f / L.area);
    }
    return zero;
}

/// Solid angle density with which rlLightSample(L, p, .) produces wi (0 for delta lights and misses).
FUSE_RL_FN float rlLightPdf(RlLight L, float3 p, float3 wi) {
    if (L.kind == kRlKindSphere) {
        float3 d = L.position - p;
        float d2 = dot(d, d);
        float r2 = L.radius * L.radius;
        if (d2 <= r2) {
            return kRlInvFourPi;
        }
        float bproj = dot(wi, d);
        if (!(bproj > 0.0f)) {
            return 0.0f;
        }
        float3 q = d - wi * bproj;
        if (r2 - dot(q, q) < 0.0f) {
            return 0.0f;
        }
        float sin2Max = r2 / d2;
        float cosMax = sqrt(max(0.0f, 1.0f - sin2Max));
        float omc = sin2Max / (1.0f + cosMax);
        return 1.0f / (kRlTwoPi * omc);
    }
    if (rlIsPlanar(L.kind)) {
        float t = rlPlanarHit(L, p, wi);
        if (t < 0.0f || !(L.area > 0.0f)) {
            return 0.0f;
        }
        float cosL = abs(dot(rlPlanarNormal(L), wi));
        if (!(cosL > 0.0f)) {
            return 0.0f;
        }
        return t * t / (cosL * L.area);
    }
    if (L.kind == kRlKindCylinder) {
        return rlCylinderHits(L, p, wi).pdf;
    }
    if (L.kind == kRlKindDistant) {
        if (!(L.area > 0.0f)) {
            return 0.0f;
        }
        if (dot(wi, -L.u) < 1.0f - rlDistantOneMinusCos(L)) {
            return 0.0f;
        }
        return 1.0f / L.area;
    }
    return 0.0f;
}

FUSE_RL_FN RlLightSample rlLightSampleInvalid() {
    RlLightSample s;
    s.position = float3(0.0f);
    s.wi = float3(0.0f, 0.0f, 1.0f);
    s.dist = 0.0f;
    s.radiance = float3(0.0f);
    s.pdf = 0.0f;
    s.flags = 0u;
    return s;
}

/// Direction in the cone around unit w with 1 - cos(theta) = om.
FUSE_RL_FN float3 rlConeDirection(float3 w, float om, float u2) {
    float cosT = 1.0f - om;
    float sinT = sqrt(max(0.0f, om * (2.0f - om)));
    float phi = kRlTwoPi * u2;
    float3 t;
    float3 b;
    rlBasis(w, t, b);
    return normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + w * cosT);
}

/// One direction towards L from p for the random numbers (u1, u2) in [0, 1)^2 (see SAMPLING above).
FUSE_RL_FN RlLightSample rlLightSample(RlLight L, float3 p, float u1, float u2) {
    RlLightSample s = rlLightSampleInvalid();
    if (L.kind == kRlKindSphere) {
        float3 d = L.position - p;
        float d2 = dot(d, d);
        float r2 = L.radius * L.radius;
        if (d2 <= r2) {
            // Inside the emitter: uniform sphere, nothing arrives (the surface emits outward).
            float z = 1.0f - 2.0f * u1;
            float rr = sqrt(max(0.0f, 1.0f - z * z));
            float phi = kRlTwoPi * u2;
            s.wi = float3(rr * cos(phi), rr * sin(phi), z);
            s.position = p;
            s.dist = 0.0f;
            s.pdf = kRlInvFourPi;
            s.flags = kRlSampleValid;
            return s;
        }
        float dist = sqrt(d2);
        float3 w = d * (1.0f / dist);
        float sin2Max = r2 / d2;
        float cosMax = sqrt(max(0.0f, 1.0f - sin2Max));
        float omc = sin2Max / (1.0f + cosMax);
        s.wi = rlConeDirection(w, u1 * omc, u2);
        float bproj = dot(s.wi, d);
        float3 q = d - s.wi * bproj;
        float th = bproj - sqrt(max(r2 - dot(q, q), 0.0f));
        s.dist = th;
        s.position = p + s.wi * th;
        s.pdf = 1.0f / (kRlTwoPi * omc);
        s.radiance = L.radiance * rlShapingAt(L, p);
        s.flags = kRlSampleValid;
        return s;
    }
    if (rlIsPlanar(L.kind)) {
        float a = 0.0f;
        float b = 0.0f;
        float3 x;
        if (L.kind == kRlKindTriangle) {
            float su = sqrt(u1);
            x = L.position + L.u * (su * (1.0f - u2)) + L.v * (su * u2);
        } else {
            if (L.kind == kRlKindRect) {
                a = u1 * 2.0f - 1.0f;
                b = u2 * 2.0f - 1.0f;
            } else {
                rlConcentric(u1, u2, a, b);
            }
            x = L.position + L.u * a + L.v * b;
        }
        float3 dv = x - p;
        float dist2 = dot(dv, dv);
        if (!(dist2 > 0.0f) || !(L.area > 0.0f)) {
            return s;
        }
        float dist = sqrt(dist2);
        float3 wi = dv * (1.0f / dist);
        float3 n = rlPlanarNormal(L);
        float cosL = abs(dot(n, wi));
        if (!(cosL > 0.0f)) {
            return s;
        }
        s.wi = wi;
        s.position = x;
        s.dist = dist;
        s.pdf = dist2 / (cosL * L.area);
        bool front = -dot(n, wi) > 0.0f;
        if (front || (L.flags & kRlFlagTwoSided) != 0u) {
            s.radiance = L.radiance * rlShapingAt(L, p);
        }
        s.flags = kRlSampleValid;
        return s;
    }
    if (L.kind == kRlKindCylinder) {
        float h = length(L.u);
        if (!(h > 0.0f)) {
            return s;
        }
        float3 A = L.u * (1.0f / h);
        float3 t;
        float3 b;
        rlBasis(A, t, b);
        float phi = kRlTwoPi * u2;
        float3 x = L.position + L.u * (u1 * 2.0f - 1.0f) + (t * cos(phi) + b * sin(phi)) * L.radius;
        float3 dv = x - p;
        float dist2 = dot(dv, dv);
        if (!(dist2 > 0.0f)) {
            return s;
        }
        float3 wi = dv * (1.0f / sqrt(dist2));
        RlCylinderHits hits = rlCylinderHits(L, p, wi);
        if (hits.tNear < 0.0f || !(hits.pdf > 0.0f)) {
            return s;
        }
        s.wi = wi;
        s.dist = hits.tNear;
        s.position = p + wi * hits.tNear;
        s.pdf = hits.pdf;
        if (dot(hits.nNear, wi) < 0.0f) {
            s.radiance = L.radiance * rlShapingAt(L, p);
        }
        s.flags = kRlSampleValid;
        return s;
    }
    if (L.kind == kRlKindDistant) {
        float3 w = -L.u;
        s.dist = kRlFarDistance;
        if (!(L.area > 0.0f)) {
            s.wi = w;
            s.position = p + w * kRlFarDistance;
            s.pdf = 1.0f;
            s.radiance = L.radiance;
            s.flags = kRlSampleValid | kRlSampleDelta;
            return s;
        }
        s.wi = rlConeDirection(w, u1 * rlDistantOneMinusCos(L), u2);
        s.position = p + s.wi * kRlFarDistance;
        s.pdf = 1.0f / L.area;
        s.radiance = L.radiance * (1.0f / L.area);
        s.flags = kRlSampleValid;
        return s;
    }
    return s;
}
