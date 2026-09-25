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
// Ported from dxvk-remix src/dxvk/shaders/rtx/utility/brdf.slangh@0867d3c (GGX D / Smith G1 / height-correlated
// visibility, Schlick and TIR-Schlick Fresnel, Hammon diffuse, Beer-Lambert and thin-wall geometric series, the
// Hanrahan single-scattering diffuse transmission, Henyey-Greenstein, perceptual roughness and anisotropy mapping),
// src/dxvk/shaders/rtx/utility/sampling.slangh@0867d3c (cosine hemisphere, spherical-cap VNDF sampling),
// src/dxvk/shaders/rtx/concept/surface_material/opaque_surface_material_interaction.slangh@0867d3c (lobe set:
// specular / diffuse / opacity pass-through / thin-opaque diffuse transmission),
// translucent_surface_material_interaction.slangh@0867d3c (dirac reflection / transmission, thin walls, diffuse layer)
// and ray_portal_surface_material_interaction.slangh@0867d3c (portals scatter nothing; the traversal teleports).
//
// FUSE Relight RL-4.3: the Relight material / BSDF model (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.2, §5.8).
//
// SINGLE SOURCE. This file is compiled three ways, with the dialect macros set by the includer:
//   C++   kernels/bsdf_cpp.hpp      the CPU reference (compute_kernel bodies in kernels/bsdf_kernels.hpp);
//   Slang shaders/material/bsdf.slang;
//   GLSL  shaders/material/bsdf.glsl (the fallback, for glslangValidator-only hosts).
// It is written in the common subset of the three (HLSL-style type names; GLSL maps float3 -> vec3 by #define):
// no `const` locals, no lerp / mix / saturate / frac / atan / asin (bsdfAtan2 / bsdfAsin), f-suffixed literals, every
// struct field assigned before use, no GLSL keywords (sample, input, output, filter...) as identifiers.
// Required macros: FUSE_BSDF_FN (function prefix), FUSE_BSDF_CONST (global constant), FUSE_BSDF_OUT(T),
// FUSE_BSDF_LUT_PARAM / FUSE_BSDF_LUT_ARG (the albedo table: `const float* lut,` / `lut,`
// in C++, empty in shaders), FUSE_BSDF_LUT_READ(i), FUSE_BSDF_WORDS_PARAM(name) (the packed material).
//
// CONVENTIONS. Directions are unit vectors in the local frame, both pointing away from the surface: `wo` towards
// the viewer (the path's previous vertex), `wi` towards the light. Surfaces: z = shading normal, x = tangent (the
// anisotropy axis). Hair: x = fiber direction (Chiang / pbrt fiber frame; the ray-fiber offset h is a per-hit
// input, BsdfMaterial.hairH). bsdfEval returns the PROJECTED value f(wo, wi) |cos theta_i|; bsdfPdf the solid-angle
// density of the non-dirac part of bsdfSample; bsdfSample returns weight = eval / pdf (dirac lobes: the lobe's
// throughput over its selection probability, pdf = that probability, kBsdfSampleDelta set). Transmission weights
// carry no eta^2 radiance scaling (upstream convention: throughput, not radiance, is transported).
//
// MODELS AND PAPERS (FUSE reimplementations where upstream has none; the divergences from upstream are listed in
// render/material/include/fuse/relight/render/material/material_bsdf.hpp):
//   Opaque       GGX (Trowbridge-Reitz) with the height-correlated Smith masking-shadowing [Heitz 2014, "Understanding
//                the Masking-Shadowing Function in Microfacet-Based BRDFs", JCGT 3(2)], anisotropy after [Kulla and
//                Conty 2017, "Revisiting Physically Based Shading at Imageworks", SIGGRAPH course], visible-normal
//                sampling with the spherical cap [Dupuy and Benyoub 2023, "Sampling Visible GGX Normals with
//                Spherical Caps", HPG], multiple-scattering energy compensation f_ms and the albedo-scaled diffuse
//                layer [Kulla and Conty 2017] (the reciprocal form: E(mu_o) and E(mu_i) both appear), diffuse Lambert,
//                Burley [Burley 2012, "Physically-Based Shading at Disney"] or Hammon [Hammon 2017, "PBR Diffuse
//                Lighting for GGX+Smith Microsurfaces", GDC] (upstream's), thin-film iridescence [Belcour and Barla
//                2017, "A Practical Extension to Microfacet Theory for the Modeling of Varying Iridescence", ACM TOG
//                36(4)] (Airy reflectance as a Fourier series in the phase, integrated against their Gaussian fit of
//                the XYZ sensitivities).
//   Translucent  Dirac dielectric reflection / transmission with Schlick Fresnel and TIR [Schlick 1994], thin walls
//                with the internal-reflection geometric series and Beer-Lambert absorption [Kulla and Conty 2017;
//                Burley 2015, "Extending the Disney BRDF to a BSDF with Integrated Subsurface Scattering"], an
//                optional Lambertian diffuse layer on the outer face.
//   Portal       No scattering (upstream: the ray portal is resolved by the traversal).
//   Hair         [Chiang, Bitterli, Tappan and Burley 2016, "A Practical and Controllable Hair and Fur Model for
//                Production Path Tracing", CGF 35(2)] with the longitudinal lobes of [d'Eon, Francois, Hill, Letteri
//                and Aubry 2011, "An Energy-Conserving Hair Reflectance Model", CGF 30(4)], the trimmed-logistic
//                azimuthal lobes and the p <= 2 + residual attenuation, cuticle tilt alpha.
//   SSS          Normalized diffusion [Christensen and Burley 2015, "Approximate Reflectance Profiles for Efficient
//                Subsurface Scattering", Pixar TM 15-04], radius sampling by the analytic CDF inversion of
//                [Golubev 2018, "Sampling Burley's Normalized Diffusion Profiles"] (disk sampling), single scattering
//                with Henyey-Greenstein [Henyey and Greenstein 1941] and exponential free flight; the thin-opaque
//                first-order transmission of [Hanrahan and Krueger 1993, "Reflection from Layered Surfaces due to
//                Subsurface Scattering", SIGGRAPH] (upstream's evalHanrahanSingleScatteringDiffuseTransmission).

// ---- constants --------------------------------------------------------------------------------------------------

FUSE_BSDF_CONST float kBsdfPi = 3.14159265358979f;
FUSE_BSDF_CONST float kBsdfTwoPi = 6.28318530717959f;
FUSE_BSDF_CONST float kBsdfInvPi = 0.318309886183791f;
FUSE_BSDF_CONST float kBsdfEpsilon = 8e-5f;         ///< upstream materialEpsilon
FUSE_BSDF_CONST float kBsdfMinAlpha = 1e-3f;        ///< GGX alpha floor (upstream: 1e-4 plus a dirac switch below 1e-3)
FUSE_BSDF_CONST float kBsdfDielectricF0 = 0.04f;    ///< upstream materialBaseReflectivityDielectric
FUSE_BSDF_CONST float kBsdfThinFilmIor = 1.5f;      ///< upstream materialIoRThinFilmLayer
FUSE_BSDF_CONST float kBsdfLobeFloor = 1e-3f;       ///< minimum selection weight of a present lobe

// BsdfMaterial.model
FUSE_BSDF_CONST uint kBsdfModelOpaque = 0u;
FUSE_BSDF_CONST uint kBsdfModelTranslucent = 1u;
FUSE_BSDF_CONST uint kBsdfModelPortal = 2u;
FUSE_BSDF_CONST uint kBsdfModelHair = 3u;
// BsdfMaterial.flags
FUSE_BSDF_CONST uint kBsdfFlagThinFilm = 1u;        ///< opaque: iridescent layer of thinFilmThickness nm
FUSE_BSDF_CONST uint kBsdfFlagThinWalled = 2u;      ///< translucent: thin shell of thickness `mediumDistance`
FUSE_BSDF_CONST uint kBsdfFlagDiffuseLayer = 4u;    ///< translucent: Lambertian layer (layerColor, layerOpacity)
FUSE_BSDF_CONST uint kBsdfFlagSssThin = 8u;         ///< opaque: thin-opaque single-scattering transmission lobe
FUSE_BSDF_CONST uint kBsdfFlagSssDiffusion = 16u;   ///< opaque: diffuse lobe is the entry of Burley diffusion
// BsdfMaterial.diffuseModel
FUSE_BSDF_CONST uint kBsdfDiffuseLambert = 0u;
FUSE_BSDF_CONST uint kBsdfDiffuseBurley = 1u;
FUSE_BSDF_CONST uint kBsdfDiffuseHammon = 2u;
// BsdfSample.flags: bits 0..7 properties, bits 8..15 the lobe.
FUSE_BSDF_CONST uint kBsdfSampleValid = 1u;
FUSE_BSDF_CONST uint kBsdfSampleDelta = 2u;
FUSE_BSDF_CONST uint kBsdfSampleTransmission = 4u;
FUSE_BSDF_CONST uint kBsdfSampleSss = 8u;           ///< the integrator performs the diffusion-profile probe
FUSE_BSDF_CONST uint kBsdfLobeShift = 8u;
FUSE_BSDF_CONST uint kBsdfLobeSpecular = 1u;
FUSE_BSDF_CONST uint kBsdfLobeDiffuse = 2u;         ///< cosine lobe: diffuse + multiple-scattering specular
FUSE_BSDF_CONST uint kBsdfLobeDiffuseTransmission = 3u;
FUSE_BSDF_CONST uint kBsdfLobeOpacity = 4u;         ///< dirac pass-through (opacity < 1)
FUSE_BSDF_CONST uint kBsdfLobeDeltaReflection = 5u;
FUSE_BSDF_CONST uint kBsdfLobeDeltaTransmission = 6u;
FUSE_BSDF_CONST uint kBsdfLobeHair0 = 8u;           ///< hair: 8 + p (R, TT, TRT, residual)

// Albedo table (bsdf_albedo_lut kernel): kBsdfLutSize^2 A, then B (rows = perceptual roughness 0..1, columns =
// mu 0..1), then kBsdfLutSize per-row Aavg, then Bavg: E_F0(mu) = F0 A + B for Schlick Fresnel, E = A + B,
// X_avg = 2 int X mu dmu of the piecewise-linear columns (exact, so the albedo scaling integrates exactly).
FUSE_BSDF_CONST int kBsdfLutSize = 32;
FUSE_BSDF_CONST int kBsdfLutA = 0;
FUSE_BSDF_CONST int kBsdfLutB = 1024;
FUSE_BSDF_CONST int kBsdfLutAavg = 2048;
FUSE_BSDF_CONST int kBsdfLutBavg = 2080;
FUSE_BSDF_CONST int kBsdfLutWords = 2112;

// Packed material: kBsdfMaterialWords float4 (bsdfMaterialPack in C++, bsdfMaterialUnpack everywhere).
FUSE_BSDF_CONST int kBsdfMaterialWords = 11;

// ---- types ------------------------------------------------------------------------------------------------------

/// The per-hit BSDF inputs (texture-resolved material + per-hit fiber offset). All colours linear.
struct BsdfMaterial {
    uint model;                   ///< kBsdfModel*
    uint flags;                   ///< kBsdfFlag*
    uint diffuseModel;            ///< kBsdfDiffuse*
    float opacity;                ///< opaque: 1 - dirac pass-through weight
    float3 albedo;                ///< opaque: base colour (metal F0 when metallic); hair: unused (sigmaA)
    float roughness;              ///< perceptual (alpha = roughness^2); hair: beta_m source when hairBetaM < 0
    float anisotropy;             ///< [-1, 1]: alpha_x = alpha (1 + a), alpha_y = alpha (1 - a)
    float metallic;               ///< [0, 1]
    float ior;                    ///< translucent / hair refractive index
    float thinFilmThickness;      ///< nm (kBsdfFlagThinFilm)
    float3 transmittance;         ///< translucent: transmittance colour at `mediumDistance` (Beer-Lambert)
    float mediumDistance;         ///< translucent: measurement distance; thin walls: the wall thickness
    float3 layerColor;            ///< translucent diffuse layer colour
    float layerOpacity;           ///< translucent diffuse layer coverage [0, 1]
    float3 sssTransmittance;      ///< subsurface transmittance colour (attenuation at sssMeasurementDistance)
    float sssMeasurementDistance;
    float3 sssSingleScatterAlbedo;
    float sssAnisotropy;          ///< Henyey-Greenstein g
    float3 sssRadius;             ///< diffusion mean free path per channel (already scaled)
    float hairBetaM;              ///< longitudinal roughness
    float hairBetaN;              ///< azimuthal roughness
    float hairAlpha;              ///< cuticle scale tilt (radians)
    float hairH;                  ///< per-hit ray offset across the fiber [-1, 1]
    float3 hairSigmaA;            ///< absorption per fiber radius
    float3 emission;              ///< emitted radiance (not part of eval; carried for the integrator)
};

struct BsdfSample {
    float3 wi;
    float3 weight;                ///< eval / pdf (dirac: throughput / selection probability)
    float pdf;                    ///< solid-angle pdf (dirac: the lobe's selection probability)
    uint flags;                   ///< kBsdfSample* | lobe << kBsdfLobeShift
};

// ---- small helpers ----------------------------------------------------------------------------------------------

FUSE_BSDF_FN float bsdfSaturate(float x) { return clamp(x, 0.0f, 1.0f); }
FUSE_BSDF_FN float bsdfSqr(float x) { return x * x; }
FUSE_BSDF_FN float bsdfPow5(float x) {
    float x2 = x * x;
    return x2 * x2 * x;
}
FUSE_BSDF_FN float bsdfSafeSqrt(float x) { return sqrt(max(x, 0.0f)); }
FUSE_BSDF_FN float bsdfMix(float a, float b, float t) { return a + (b - a) * t; }
FUSE_BSDF_FN float3 bsdfMix3(float3 a, float3 b, float t) { return a + (b - a) * t; }
FUSE_BSDF_FN float bsdfLum(float3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
FUSE_BSDF_FN float3 bsdfExp3(float3 v) { return float3(exp(v.x), exp(v.y), exp(v.z)); }
FUSE_BSDF_FN float bsdfSinh(float x) { return 0.5f * (exp(x) - exp(-x)); }

/// Portable atan2: Vulkan leaves atan / asin precision loose (Lavapipe's are ~1e-4 rad off), so the angles the hair
/// lobes depend on use only +, *, / and sqrt (correctly rounded everywhere): reduction to |t| <= 1, then to
/// |t| <= tan(pi/12) with atan(t) = pi/6 + atan((sqrt(3) t - 1) / (sqrt(3) + t)), then the odd series to t^11
/// (truncation < 3e-9).
FUSE_BSDF_FN float bsdfAtanUnit(float t) {
    float a = abs(t);
    float offset = 0.0f;
    if (a > 0.267949192f) {
        a = (1.73205081f * a - 1.0f) / (1.73205081f + a);
        offset = 0.523598776f;
    }
    float a2 = a * a;
    float series = a * (1.0f + a2 * (-0.333333333f + a2 * (0.2f + a2 * (-0.142857143f + a2 * (0.111111111f +
                                                                                           a2 * -0.0909090909f)))));
    float r = offset + series;
    return t < 0.0f ? -r : r;
}
FUSE_BSDF_FN float bsdfAtan2(float y, float x) {
    float ax = abs(x);
    float ay = abs(y);
    if (ax == 0.0f && ay == 0.0f) {
        return 0.0f;
    }
    float r = ay <= ax ? bsdfAtanUnit(ay / ax) : 1.57079633f - bsdfAtanUnit(ax / ay);
    if (x < 0.0f) {
        r = kBsdfPi - r;
    }
    return y < 0.0f ? -r : r;
}
/// asin(x) = atan2(x, sqrt(1 - x^2)), clamped (see bsdfAtan2).
FUSE_BSDF_FN float bsdfAsin(float x) {
    float c = clamp(x, -1.0f, 1.0f);
    return bsdfAtan2(c, sqrt(max(0.0f, 1.0f - c * c)));
}
/// Reduces an angle to [-pi, pi) with an exactly rounded floor, so cos / sin see the same small argument everywhere.
FUSE_BSDF_FN float bsdfWrapAngle(float a) { return a - kBsdfTwoPi * floor((a + kBsdfPi) / kBsdfTwoPi); }

FUSE_BSDF_FN BsdfSample bsdfSampleInvalid() {
    BsdfSample s;
    s.wi = float3(0.0f, 0.0f, 0.0f);
    s.weight = float3(0.0f, 0.0f, 0.0f);
    s.pdf = 0.0f;
    s.flags = 0u;
    return s;
}

FUSE_BSDF_FN BsdfSample bsdfSampleMake(float3 wi, float3 weight, float pdf, uint flags, uint lobe) {
    BsdfSample s;
    s.wi = wi;
    s.weight = weight;
    s.pdf = pdf;
    s.flags = flags | kBsdfSampleValid | (lobe << kBsdfLobeShift);
    return s;
}

/// Mirror of `wo` about the unit vector n (both on the same side).
FUSE_BSDF_FN float3 bsdfReflect(float3 wo, float3 n) { return n * (2.0f * dot(wo, n)) - wo; }

/// Refraction of `wo` (on the side of n) through the interface with relative index eta = eta_wo / eta_other.
/// False on total internal reflection.
FUSE_BSDF_FN bool bsdfRefract(float3 wo, float3 n, float eta, FUSE_BSDF_OUT(float3) wt) {
    float cosi = dot(n, wo);
    float sin2t = eta * eta * max(0.0f, 1.0f - cosi * cosi);
    if (sin2t >= 1.0f) {
        wt = float3(0.0f, 0.0f, 0.0f);
        return false;
    }
    float cost = sqrt(1.0f - sin2t);
    wt = wo * (-eta) + n * (eta * cosi - cost);
    return true;
}

/// Cosine-weighted hemisphere around +z (upstream calcCosineHemisphereDirectionSample); pdf = z / pi.
FUSE_BSDF_FN float3 bsdfCosineSample(float u1, float u2) {
    float phi = kBsdfTwoPi * u1;
    float cosTheta = sqrt(u2);
    float sinTheta = sqrt(max(0.0f, 1.0f - u2));
    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

// ---- albedo table -----------------------------------------------------------------------------------------------

/// Bilinear fetch of the table at (mu, perceptual roughness), clamped to [0, 1]^2.
FUSE_BSDF_FN float bsdfLutFetch(FUSE_BSDF_LUT_PARAM int offset, float mu, float rho) {
    float fx = clamp(mu, 0.0f, 1.0f) * float(kBsdfLutSize - 1);
    float fy = clamp(rho, 0.0f, 1.0f) * float(kBsdfLutSize - 1);
    int x0 = int(floor(fx));
    int y0 = int(floor(fy));
    x0 = x0 > kBsdfLutSize - 2 ? kBsdfLutSize - 2 : x0;
    y0 = y0 > kBsdfLutSize - 2 ? kBsdfLutSize - 2 : y0;
    float tx = fx - float(x0);
    float ty = fy - float(y0);
    int i00 = offset + y0 * kBsdfLutSize + x0;
    float v00 = FUSE_BSDF_LUT_READ(i00);
    float v10 = FUSE_BSDF_LUT_READ(i00 + 1);
    float v01 = FUSE_BSDF_LUT_READ(i00 + kBsdfLutSize);
    float v11 = FUSE_BSDF_LUT_READ(i00 + kBsdfLutSize + 1);
    return bsdfMix(bsdfMix(v00, v10, tx), bsdfMix(v01, v11, tx), ty);
}

/// Linear fetch of a per-row average at perceptual roughness rho.
FUSE_BSDF_FN float bsdfLutAvg(FUSE_BSDF_LUT_PARAM int offset, float rho) {
    float fy = clamp(rho, 0.0f, 1.0f) * float(kBsdfLutSize - 1);
    int y0 = int(floor(fy));
    y0 = y0 > kBsdfLutSize - 2 ? kBsdfLutSize - 2 : y0;
    float ty = fy - float(y0);
    return bsdfMix(FUSE_BSDF_LUT_READ(offset + y0), FUSE_BSDF_LUT_READ(offset + y0 + 1), ty);
}

// ---- GGX microfacet distribution --------------------------------------------------------------------------------

/// Upstream calcRoughness: alpha = perceptual^2, anisotropic split, floored (FUSE: also capped at 1).
FUSE_BSDF_FN void bsdfAlphas(float roughness, float anisotropy, FUSE_BSDF_OUT(float) ax, FUSE_BSDF_OUT(float) ay) {
    float a = bsdfSqr(clamp(roughness, 0.0f, 1.0f));
    float an = clamp(anisotropy, -1.0f, 1.0f);
    ax = clamp(a * (1.0f + an), kBsdfMinAlpha, 1.0f);
    ay = clamp(a * (1.0f - an), kBsdfMinAlpha, 1.0f);
}

/// Table row (perceptual roughness) for direction w: the projected roughness alpha(phi_w) = sqrt(ax^2 cos^2 phi +
/// ay^2 sin^2 phi), with which the anisotropic Smith masking equals the isotropic one [Heitz 2014, sec. 5.3], so the
/// isotropic albedo E(mu, alpha(phi)) stands in for the anisotropic lobe (exact when ax == ay).
FUSE_BSDF_FN float bsdfLutRoughnessDir(float ax, float ay, float3 w) {
    float s2 = w.x * w.x + w.y * w.y;
    if (s2 < 1e-12f) {
        return sqrt(0.5f * (ax + ay));
    }
    return sqrt(sqrt((ax * ax * w.x * w.x + ay * ay * w.y * w.y) / s2));
}

/// Azimuthal average of a per-row table average over alpha(phi) (8-point midpoint rule on [0, pi/2]; the integrand is
/// smooth and periodic): the E_avg that keeps the albedo scaling exact with direction-dependent rows.
FUSE_BSDF_FN float bsdfLutAvgAniso(FUSE_BSDF_LUT_PARAM int offset, float ax, float ay) {
    float sum = 0.0f;
    for (int k = 0; k < 8; ++k) {
        float phi = (float(k) + 0.5f) * (kBsdfPi / 16.0f);
        float c = cos(phi);
        float sn = sin(phi);
        sum = sum + bsdfLutAvg(FUSE_BSDF_LUT_ARG offset, sqrt(sqrt(ax * ax * c * c + ay * ay * sn * sn)));
    }
    return sum * 0.125f;
}

/// Anisotropic GGX normal distribution D(h) (h in the upper hemisphere).
FUSE_BSDF_FN float bsdfGgxD(float3 h, float ax, float ay) {
    float hx = h.x / ax;
    float hy = h.y / ay;
    float d = hx * hx + hy * hy + h.z * h.z;
    return 1.0f / (kBsdfPi * ax * ay * d * d);
}

/// sqrt(ax^2 wx^2 + ay^2 wy^2 + wz^2): the Smith Lambda root.
FUSE_BSDF_FN float bsdfGgxRoot(float3 w, float ax, float ay) {
    return sqrt(bsdfSqr(ax * w.x) + bsdfSqr(ay * w.y) + w.z * w.z);
}

/// Smith G1 (upstream evalGGXShadowing with k = 1) for w.z > 0.
FUSE_BSDF_FN float bsdfGgxG1(float3 w, float ax, float ay) { return 2.0f * w.z / (w.z + bsdfGgxRoot(w, ax, ay)); }

/// Height-correlated visibility G2 / (4 mu_o mu_i) (upstream evalHeightCorrelatedGGXVisibility).
FUSE_BSDF_FN float bsdfGgxVisibility(float3 wo, float3 wi, float ax, float ay) {
    float denom = wo.z * bsdfGgxRoot(wi, ax, ay) + wi.z * bsdfGgxRoot(wo, ax, ay);
    return 0.5f / max(denom, 1e-12f);
}

/// Visible-normal sample with the spherical cap (upstream calcGGXVisibleNormalDistributionSphericalCapSample, k = 1).
FUSE_BSDF_FN float3 bsdfGgxVndfSample(float3 wo, float ax, float ay, float u1, float u2) {
    float3 wh = normalize(float3(ax * wo.x, ay * wo.y, wo.z));
    float phi = kBsdfTwoPi * u1;
    float z = (1.0f - u2) * (1.0f + wh.z) - wh.z;
    float sinTheta = sqrt(clamp(1.0f - z * z, 0.0f, 1.0f));
    float3 hs = float3(sinTheta * cos(phi), sinTheta * sin(phi), z) + wh;
    return normalize(float3(ax * hs.x, ay * hs.y, max(hs.z, 0.0f)));
}

/// Solid-angle pdf of the reflected direction of a VNDF-sampled normal: G1(wo) D(h) / (4 wo.z).
FUSE_BSDF_FN float bsdfGgxReflectionPdf(float3 wo, float3 wi, float ax, float ay) {
    if (wo.z <= 0.0f || wi.z <= 0.0f) {
        return 0.0f;
    }
    float3 h = normalize(wo + wi);
    if (dot(wo, h) <= 0.0f) {
        return 0.0f;
    }
    return bsdfGgxG1(wo, ax, ay) * bsdfGgxD(h, ax, ay) / (4.0f * wo.z);
}

// ---- Fresnel ----------------------------------------------------------------------------------------------------

FUSE_BSDF_FN float bsdfSchlick(float f0, float c) { return f0 + (1.0f - f0) * bsdfPow5(1.0f - bsdfSaturate(c)); }
FUSE_BSDF_FN float3 bsdfSchlick3(float3 f0, float c) {
    return f0 + (float3(1.0f, 1.0f, 1.0f) - f0) * bsdfPow5(1.0f - bsdfSaturate(c));
}

/// Upstream evalTranslucentSchlickFresnelTIR: from the denser side (relIor > 1) the refracted cosine is used; 1 on TIR.
FUSE_BSDF_FN float bsdfSchlickTir(float f0, float relIor, float c) {
    float cc = c;
    if (relIor > 1.0f) {
        float sin2 = relIor * relIor * (1.0f - c * c);
        if (sin2 > 1.0f) {
            return 1.0f;
        }
        cc = sqrt(1.0f - sin2);
    }
    return bsdfSchlick(f0, cc);
}

/// Unpolarized dielectric Fresnel reflectance (exact) for incidence cosine cosi (> 0 = outside) and index eta.
FUSE_BSDF_FN float bsdfFresnelDielectric(float cosi, float eta) {
    float ci = clamp(cosi, -1.0f, 1.0f);
    float e = eta;
    if (ci < 0.0f) {
        e = 1.0f / eta;
        ci = -ci;
    }
    float sin2t = (1.0f - ci * ci) / (e * e);
    if (sin2t >= 1.0f) {
        return 1.0f;
    }
    float ct = sqrt(1.0f - sin2t);
    float rs = (ci - e * ct) / (ci + e * ct);
    float rp = (e * ci - ct) / (e * ci + ct);
    return 0.5f * (rs * rs + rp * rp);
}

/// Upstream iorToBaseReflectivity / baseReflectivityToIoR (sqrt(F0) capped at 0.99).
FUSE_BSDF_FN float bsdfIorToF0(float ior) { return bsdfSqr((ior - 1.0f) / (ior + 1.0f)); }
FUSE_BSDF_FN float bsdfF0ToIor(float f0) {
    float s = min(sqrt(max(f0, 0.0f)), 0.99f);
    return (1.0f + s) / (1.0f - s);
}

// ---- thin film (Belcour and Barla 2017) -------------------------------------------------------------------------

/// Spectral integral of cos(2 pi OPD / lambda) against the CIE XYZ matching functions (their Gaussian fit), in
/// linear Rec.709, normalized so that OPD = 0 gives (1, 1, 1). The phase is range-reduced exactly (cycles minus
/// floor) so every backend evaluates cos() on [0, 2 pi).
FUSE_BSDF_FN float bsdfThinFilmCos(float cycles) {
    return cos(kBsdfTwoPi * (cycles - floor(cycles)));
}
FUSE_BSDF_FN float3 bsdfThinFilmSensitivity(float opdNm) {
    float opd = opdNm * 1e-9f;                    // metres
    float phase = kBsdfTwoPi * opd;
    float p2 = phase * phase;
    float x = 8.4659000e-01f * bsdfThinFilmCos(1.6810e+06f * opd) * exp(-4.3278e+09f * p2) +
              1.5386831e-01f * bsdfThinFilmCos(2.2399e+06f * opd) * exp(-4.5282e+09f * p2);
    float y = 1.0002219e+00f * bsdfThinFilmCos(1.7953e+06f * opd) * exp(-9.3046e+09f * p2);
    float z = 1.0011225e+00f * bsdfThinFilmCos(2.2084e+06f * opd) * exp(-6.6121e+09f * p2);
    float r = 3.2404542f * x - 1.5371385f * y - 0.4985314f * z;
    float g = -0.9692660f * x + 1.8760108f * y + 0.0415560f * z;
    float b = 0.0556434f * x - 0.2040259f * y + 1.0572252f * z;
    return float3(r / 1.2053688f, g / 0.94831950f, b / 0.91000965f);
}

/// Airy reflectance of one polarization as C0 + sum_m C_m cos(m delta) with signed real amplitudes a = r12, b = r23
/// (|1 + x e^{i delta}|^-2 expanded as a Poisson kernel, x = a b), integrated spectrally term by term (m <= 3).
FUSE_BSDF_FN float3 bsdfThinFilmAiry(float a, float b, float3 s1, float3 s2, float3 s3) {
    float x = a * b;
    float k = (1.0f - a * a) * (1.0f - b * b) / max(1.0f - x * x, 1e-6f);
    float c1 = 2.0f * k * x;          // -2 k (-x)^1
    float c2 = -2.0f * k * x * x;     // -2 k (-x)^2
    float c3 = 2.0f * k * x * x * x;  // -2 k (-x)^3
    return float3(1.0f - k, 1.0f - k, 1.0f - k) + s1 * c1 + s2 * c2 + s3 * c3;
}

/// Reflectance of a film (index filmIor, thickness nm) over a dielectric base (baseIor) seen from air at cosine c.
FUSE_BSDF_FN float3 bsdfThinFilmFresnel(float c, float filmIor, float baseIor, float thicknessNm) {
    float cos1 = clamp(c, 0.0f, 1.0f);
    float sin1sq = 1.0f - cos1 * cos1;
    float cos2 = sqrt(max(0.0f, 1.0f - sin1sq / (filmIor * filmIor)));
    float sin3sq = sin1sq / (baseIor * baseIor);
    float cos3 = sqrt(max(0.0f, 1.0f - sin3sq));
    float r12s = (cos1 - filmIor * cos2) / (cos1 + filmIor * cos2);
    float r23s = (filmIor * cos2 - baseIor * cos3) / max(filmIor * cos2 + baseIor * cos3, 1e-6f);
    float r12p = (filmIor * cos1 - cos2) / (filmIor * cos1 + cos2);
    float r23p = (baseIor * cos2 - filmIor * cos3) / max(baseIor * cos2 + filmIor * cos3, 1e-6f);
    float opd = 2.0f * filmIor * max(thicknessNm, 0.0f) * cos2;
    float3 s1 = bsdfThinFilmSensitivity(opd);
    float3 s2 = bsdfThinFilmSensitivity(2.0f * opd);
    float3 s3 = bsdfThinFilmSensitivity(3.0f * opd);
    float3 r = (bsdfThinFilmAiry(r12s, r23s, s1, s2, s3) + bsdfThinFilmAiry(r12p, r23p, s1, s2, s3)) * 0.5f;
    return float3(bsdfSaturate(r.x), bsdfSaturate(r.y), bsdfSaturate(r.z));
}

// ---- diffuse models ---------------------------------------------------------------------------------------------

/// Burley 2012 retro-reflective factor (x 1/pi), with the perceptual roughness and cos theta_d = wi . h.
FUSE_BSDF_FN float bsdfBurleyFactor(float roughness, float muo, float mui, float cosD) {
    float fd90 = 0.5f + 2.0f * roughness * cosD * cosD;
    return (1.0f + (fd90 - 1.0f) * bsdfPow5(1.0f - mui)) * (1.0f + (fd90 - 1.0f) * bsdfPow5(1.0f - muo)) * kBsdfInvPi;
}

/// Upstream evalHammonDiffuse (includes its own Fresnel-like terms and multiple scattering; albedo applied).
FUSE_BSDF_FN float3 bsdfHammon(float3 albedo, float alpha, float ldotv, float nol, float nov, float noh) {
    float facing = 0.5f + 0.5f * ldotv;
    float rough = facing * (0.9f - 0.4f * facing) * ((0.5f + noh) / max(noh, kBsdfEpsilon));
    float smoothTerm = 1.05f * (1.0f - bsdfPow5(1.0f - bsdfSaturate(nol))) * (1.0f - bsdfPow5(1.0f - bsdfSaturate(nov)));
    float single = bsdfMix(smoothTerm, rough, alpha) * kBsdfInvPi;
    float multi = 0.1159f * alpha;
    return albedo * (float3(single, single, single) + albedo * multi);
}

// ---- subsurface: normalized diffusion, single scattering, thin transmission -------------------------------------

/// Christensen-Burley scale s(A) for the mean-free-path parameterization (searchlight fit).
FUSE_BSDF_FN float bsdfBurleyScale(float albedo) {
    float t = abs(albedo - 0.8f);
    return 1.85f - albedo + 7.0f * t * t * t;
}
/// Profile R(r) / A for shape d: (e^{-r/d} + e^{-r/3d}) / (8 pi d r); 2 pi int R r dr = 1.
FUSE_BSDF_FN float bsdfBurleyProfile(float r, float d) {
    float rr = max(r, 1e-7f);
    return (exp(-rr / d) + exp(-rr / (3.0f * d))) / (8.0f * kBsdfPi * d * rr);
}
/// Radial pdf (including 2 pi r): (e^{-r/d} + e^{-r/3d}) / (4 d).
FUSE_BSDF_FN float bsdfBurleyRadiusPdf(float r, float d) {
    return (exp(-r / d) + exp(-r / (3.0f * d))) / (4.0f * d);
}
/// Golubev 2018: 1 - CDF(r) = e^{-r/d}/4 + 3 e^{-r/3d}/4 = w; with x = e^{-r/3d}, x^3 + 3x - 4w = 0, whose real root
/// is c - 1/c with c = cbrt(2w + sqrt(4w^2 + 1)) (Cardano, the two cube roots multiply to 1).
FUSE_BSDF_FN float bsdfBurleyRadiusSample(float u, float d) {
    float w = 1.0f - clamp(u, 0.0f, 0.99999994f);
    float g = 2.0f * w + sqrt(4.0f * w * w + 1.0f);
    float c = pow(g, 1.0f / 3.0f);
    float x = c - 1.0f / c;
    return -3.0f * d * log(max(x, 1e-30f));
}
/// Disk sample for the probe rays: a channel uniformly, its radius, a uniform angle. Returns (x, y, pdf) with the
/// planar pdf averaged over the three channels' profiles (one-sample MIS over channels).
FUSE_BSDF_FN float3 bsdfSssDiskSample(float u1, float u2, float u3, float3 d) {
    float dc = u1 < (1.0f / 3.0f) ? d.x : (u1 < (2.0f / 3.0f) ? d.y : d.z);
    float r = bsdfBurleyRadiusSample(u2, dc);
    float phi = kBsdfTwoPi * u3;
    float pdfR = (bsdfBurleyRadiusPdf(r, d.x) + bsdfBurleyRadiusPdf(r, d.y) + bsdfBurleyRadiusPdf(r, d.z)) / 3.0f;
    return float3(r * cos(phi), r * sin(phi), pdfR / (kBsdfTwoPi * max(r, 1e-7f)));
}

/// Henyey-Greenstein phase for cosine c between the propagation directions (upstream, with the division floored).
FUSE_BSDF_FN float bsdfHgEval(float g, float c) {
    float denom = 1.0f + g * g - 2.0f * g * c;
    return (1.0f - g * g) / (4.0f * kBsdfPi * max(denom * sqrt(max(denom, 0.0f)), 1e-12f));
}
/// Samples the cosine to the propagation direction (inversion of the HG CDF).
FUSE_BSDF_FN float bsdfHgSampleCos(float g, float u) {
    if (abs(g) < 1e-3f) {
        return 1.0f - 2.0f * u;
    }
    float t = (1.0f - g * g) / (1.0f - g + 2.0f * g * u);
    return clamp((1.0f + g * g - t * t) / (2.0f * g), -1.0f, 1.0f);
}
/// Exponential free flight: distance and its pdf sigma e^{-sigma t}.
FUSE_BSDF_FN float bsdfFreeFlightSample(float u, float sigma) { return -log(max(1.0f - u, 1e-30f)) / sigma; }
FUSE_BSDF_FN float bsdfFreeFlightPdf(float t, float sigma) { return sigma * exp(-sigma * t); }

/// Upstream transmittanceToAttenuationCoefficient (transmittance floored so log() stays finite on every backend).
FUSE_BSDF_FN float3 bsdfAttenuationCoefficient(float3 transmittance, float measured) {
    float3 t = max(transmittance, float3(1e-6f, 1e-6f, 1e-6f));
    return float3(-log(t.x), -log(t.y), -log(t.z)) / max(measured, 1e-6f);
}
/// Beer-Lambert attenuation after `travelled` inside a translucent material.
FUSE_BSDF_FN float3 bsdfMediumAttenuation(BsdfMaterial m, float travelled) {
    return bsdfExp3(bsdfAttenuationCoefficient(m.transmittance, m.mediumDistance) * (-travelled));
}

/// Upstream evalHanrahanSingleScatteringDiffuseTransmission (unprojected): muo = n.v, mut = -n.l, c = -v.l.
FUSE_BSDF_FN float3 bsdfHanrahanTransmission(float f0, float3 sigma, float measurementDistance, float3 ssAlbedo,
                                             float g, float muo, float mut, float c) {
    if (muo < kBsdfEpsilon || mut < kBsdfEpsilon) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    float fo = bsdfSchlick(f0, muo);
    float fi = bsdfSchlick(f0, mut);
    float dist = measurementDistance * measurementDistance;
    float3 first = bsdfExp3(sigma * (-dist / muo));
    float3 second = bsdfExp3(sigma * (-dist / mut));
    float phase = bsdfHgEval(g, c);
    float diff = muo - mut;
    if (abs(diff) >= kBsdfEpsilon) {
        float3 d = first - second;
        return ssAlbedo * ((1.0f - fo) * (1.0f - fi) / abs(diff) * phase) * float3(abs(d.x), abs(d.y), abs(d.z));
    }
    return ssAlbedo * ((1.0f - fo) * (1.0f - fi) * phase / (mut * mut)) * (sigma * dist) * second;
}

// ---- opaque ------------------------------------------------------------------------------------------------------

/// Per-wo terms of the opaque model.
struct BsdfOpaqueCtx {
    float ax;
    float ay;
    float rho;          ///< table row of wo
    float eo;           ///< E(mu_o) = A + B
    float ao;           ///< A(mu_o)
    float bo;           ///< B(mu_o)
    float eavg;
    float aavg;
    float bavg;
    float fmsD;         ///< Kulla-Conty F_ms for the dielectric F0
    float3 fmsM;        ///< ... for the metal F0 = albedo
    float especDo;      ///< compensated dielectric specular albedo at mu_o
    float especDavg;    ///< ... hemispherical average
    float metallic;
    float opacity;
};

/// Kulla-Conty Fresnel factor of the multiple-scattering lobe: F_avg^2 E_avg / (1 - F_avg (1 - E_avg)).
FUSE_BSDF_FN float bsdfFms(float f0, float eavg) {
    float favg = f0 + (1.0f - f0) / 21.0f;
    return favg * favg * eavg / max(1.0f - favg * (1.0f - eavg), 1e-6f);
}

FUSE_BSDF_FN BsdfOpaqueCtx bsdfOpaqueContext(FUSE_BSDF_LUT_PARAM BsdfMaterial m, float3 wo) {
    BsdfOpaqueCtx c;
    float ax;
    float ay;
    bsdfAlphas(m.roughness, m.anisotropy, ax, ay);
    c.ax = ax;
    c.ay = ay;
    c.rho = bsdfLutRoughnessDir(ax, ay, wo);
    c.ao = bsdfLutFetch(FUSE_BSDF_LUT_ARG kBsdfLutA, wo.z, c.rho);
    c.bo = bsdfLutFetch(FUSE_BSDF_LUT_ARG kBsdfLutB, wo.z, c.rho);
    c.eo = c.ao + c.bo;
    if (ax == ay) {
        c.aavg = bsdfLutAvg(FUSE_BSDF_LUT_ARG kBsdfLutAavg, sqrt(ax));
        c.bavg = bsdfLutAvg(FUSE_BSDF_LUT_ARG kBsdfLutBavg, sqrt(ax));
    } else {
        c.aavg = bsdfLutAvgAniso(FUSE_BSDF_LUT_ARG kBsdfLutAavg, ax, ay);
        c.bavg = bsdfLutAvgAniso(FUSE_BSDF_LUT_ARG kBsdfLutBavg, ax, ay);
    }
    c.eavg = c.aavg + c.bavg;
    c.fmsD = bsdfFms(kBsdfDielectricF0, c.eavg);
    c.fmsM = float3(bsdfFms(m.albedo.x, c.eavg), bsdfFms(m.albedo.y, c.eavg), bsdfFms(m.albedo.z, c.eavg));
    c.especDo = kBsdfDielectricF0 * c.ao + c.bo + c.fmsD * (1.0f - c.eo);
    c.especDavg = kBsdfDielectricF0 * c.aavg + c.bavg + c.fmsD * (1.0f - c.eavg);
    c.metallic = bsdfSaturate(m.metallic);
    c.opacity = bsdfSaturate(m.opacity);
    return c;
}

/// Projected opaque BSDF (all non-dirac lobes).
/// Thin-opaque subsurface sheets (leaves) are two-sided: seen from below, the frame is mirrored (z -> -z).
FUSE_BSDF_FN float3 bsdfOpaqueSide(BsdfMaterial m, float3 wo) {
    bool twoSided = (m.flags & kBsdfFlagSssThin) != 0u && wo.z < 0.0f;
    return float3(1.0f, 1.0f, twoSided ? -1.0f : 1.0f);
}

FUSE_BSDF_FN float3 bsdfOpaqueEvalUpper(FUSE_BSDF_LUT_PARAM BsdfMaterial m, float3 wo, float3 wi) {
    float3 zero = float3(0.0f, 0.0f, 0.0f);
    if (wo.z <= 0.0f) {
        return zero;
    }
    BsdfOpaqueCtx c = bsdfOpaqueContext(FUSE_BSDF_LUT_ARG m, wo);
    float dielectric = 1.0f - c.metallic;
    if (wi.z < 0.0f) {
        if ((m.flags & kBsdfFlagSssThin) == 0u) {
            return zero;
        }
        float3 sigma = bsdfAttenuationCoefficient(m.sssTransmittance, max(m.sssMeasurementDistance, 1e-6f));
        float3 t = bsdfHanrahanTransmission(kBsdfDielectricF0, sigma, m.sssMeasurementDistance, m.sssSingleScatterAlbedo,
                                            m.sssAnisotropy, wo.z, -wi.z, dot(wo, wi) * -1.0f);
        return t * (c.opacity * dielectric * (-wi.z));
    }
    if (wi.z == 0.0f) {
        return zero;
    }
    float3 h = normalize(wo + wi);
    float voh = dot(wo, h);
    // Single-scattering specular: F D V mu_i.
    float3 spec = zero;
    if (voh > 0.0f) {
        float dv = bsdfGgxD(h, c.ax, c.ay) * bsdfGgxVisibility(wo, wi, c.ax, c.ay) * wi.z;
        float3 fD;
        float3 fM;
        if ((m.flags & kBsdfFlagThinFilm) != 0u && m.thinFilmThickness > 0.0f) {
            fD = bsdfThinFilmFresnel(voh, kBsdfThinFilmIor, bsdfF0ToIor(kBsdfDielectricF0), m.thinFilmThickness);
            fM = bsdfThinFilmFresnel(voh, kBsdfThinFilmIor, bsdfF0ToIor(bsdfLum(m.albedo)), m.thinFilmThickness);
        } else {
            float fd = bsdfSchlick(kBsdfDielectricF0, voh);
            fD = float3(fd, fd, fd);
            fM = bsdfSchlick3(m.albedo, voh);
        }
        spec = (fD * dielectric + fM * c.metallic) * dv;
    }
    // Multiple-scattering specular (Kulla-Conty): F_ms (1 - E(mu_o)) (1 - E(mu_i)) / (pi (1 - E_avg)) mu_i.
    float rhoI = bsdfLutRoughnessDir(c.ax, c.ay, wi);
    float ai = bsdfLutFetch(FUSE_BSDF_LUT_ARG kBsdfLutA, wi.z, rhoI);
    float bi = bsdfLutFetch(FUSE_BSDF_LUT_ARG kBsdfLutB, wi.z, rhoI);
    float ei = ai + bi;
    float msShape = (1.0f - c.eo) * (1.0f - ei) / (kBsdfPi * max(1.0f - c.eavg, 1e-6f)) * wi.z;
    float3 fms = float3(c.fmsD, c.fmsD, c.fmsD) * dielectric + c.fmsM * c.metallic;
    float3 ms = fms * msShape;
    // Diffuse under the dielectric coat, albedo-scaled: (1 - Es(mu_o)) (1 - Es(mu_i)) / (1 - Es_avg).
    float3 diffuse = zero;
    if (dielectric > 0.0f) {
        if (m.diffuseModel == kBsdfDiffuseHammon) {
            diffuse = bsdfHammon(m.albedo, c.ax * 0.5f + c.ay * 0.5f, dot(wi, wo), wi.z, wo.z, h.z) * (wi.z * dielectric);
        } else {
            float especDi = kBsdfDielectricF0 * ai + bi + c.fmsD * (1.0f - ei);
            float shape = (1.0f - c.especDo) * (1.0f - especDi) / max(1.0f - c.especDavg, 1e-6f);
            float lobe = kBsdfInvPi;
            if (m.diffuseModel == kBsdfDiffuseBurley) {
                lobe = bsdfBurleyFactor(m.roughness, wo.z, wi.z, dot(wi, h));
            }
            diffuse = m.albedo * (lobe * shape * wi.z * dielectric);
        }
    }
    return (spec + ms + diffuse) * c.opacity;
}

/// Projected opaque BSDF (all non-dirac lobes).
FUSE_BSDF_FN float3 bsdfOpaqueEval(FUSE_BSDF_LUT_PARAM BsdfMaterial m, float3 wo, float3 wi) {
    float3 side = bsdfOpaqueSide(m, wo);
    return bsdfOpaqueEvalUpper(FUSE_BSDF_LUT_ARG m, wo * side, wi * side);
}

/// Lobe selection probabilities at wo: specular (VNDF), cosine (diffuse + multiple scattering), thin transmission,
/// opacity pass-through. They sum to 1 when opacity or any lobe is present.
FUSE_BSDF_FN void bsdfOpaqueLobeProbabilities(BsdfMaterial m, BsdfOpaqueCtx c, FUSE_BSDF_OUT(float) pSpec,
                                              FUSE_BSDF_OUT(float) pCos, FUSE_BSDF_OUT(float) pTrans,
                                              FUSE_BSDF_OUT(float) pDelta) {
    float dielectric = 1.0f - c.metallic;
    float3 albedoM = m.albedo * c.ao + float3(c.bo, c.bo, c.bo);
    float wSpec = dielectric * (kBsdfDielectricF0 * c.ao + c.bo) + c.metallic * bsdfLum(albedoM);
    float wCos = (dielectric * c.fmsD + c.metallic * bsdfLum(c.fmsM)) * (1.0f - c.eo) +
                 dielectric * bsdfLum(m.albedo) * (1.0f - c.especDo);
    float wTrans = (m.flags & kBsdfFlagSssThin) != 0u ? dielectric * 0.5f * bsdfLum(m.sssSingleScatterAlbedo) : 0.0f;
    wSpec = c.opacity * max(wSpec, kBsdfLobeFloor);
    wCos = c.opacity * max(wCos, kBsdfLobeFloor);
    wTrans = (m.flags & kBsdfFlagSssThin) != 0u ? c.opacity * max(wTrans, kBsdfLobeFloor) : 0.0f;
    float wDelta = 1.0f - c.opacity;
    float sum = wSpec + wCos + wTrans + wDelta;
    pSpec = wSpec / sum;
    pCos = wCos / sum;
    pTrans = wTrans / sum;
    pDelta = wDelta / sum;
}

FUSE_BSDF_FN float bsdfOpaquePdfUpper(FUSE_BSDF_LUT_PARAM BsdfMaterial m, float3 wo, float3 wi) {
    if (wo.z <= 0.0f) {
        return 0.0f;
    }
    BsdfOpaqueCtx c = bsdfOpaqueContext(FUSE_BSDF_LUT_ARG m, wo);
    float pSpec;
    float pCos;
    float pTrans;
    float pDelta;
    bsdfOpaqueLobeProbabilities(m, c, pSpec, pCos, pTrans, pDelta);
    if (wi.z > 0.0f) {
        return pSpec * bsdfGgxReflectionPdf(wo, wi, c.ax, c.ay) + pCos * wi.z * kBsdfInvPi;
    }
    return pTrans * max(-wi.z, 0.0f) * kBsdfInvPi;
}

FUSE_BSDF_FN float bsdfOpaquePdf(FUSE_BSDF_LUT_PARAM BsdfMaterial m, float3 wo, float3 wi) {
    float3 side = bsdfOpaqueSide(m, wo);
    return bsdfOpaquePdfUpper(FUSE_BSDF_LUT_ARG m, wo * side, wi * side);
}

FUSE_BSDF_FN BsdfSample bsdfOpaqueSampleUpper(FUSE_BSDF_LUT_PARAM BsdfMaterial m, float3 wo, float4 u) {
    if (wo.z <= 0.0f) {
        return bsdfSampleInvalid();
    }
    BsdfOpaqueCtx c = bsdfOpaqueContext(FUSE_BSDF_LUT_ARG m, wo);
    float pSpec;
    float pCos;
    float pTrans;
    float pDelta;
    bsdfOpaqueLobeProbabilities(m, c, pSpec, pCos, pTrans, pDelta);
    float3 wi;
    uint lobe;
    uint flags = 0u;
    if (u.x < pDelta) {
        // Opacity pass-through (upstream opacity transmission lobe): throughput 1 - opacity.
        float t = (1.0f - c.opacity) / pDelta;
        return bsdfSampleMake(wo * -1.0f, float3(t, t, t), pDelta, kBsdfSampleDelta | kBsdfSampleTransmission,
                              kBsdfLobeOpacity);
    }
    if (u.x < pDelta + pSpec) {
        float3 h = bsdfGgxVndfSample(wo, c.ax, c.ay, u.y, u.z);
        wi = bsdfReflect(wo, h);
        lobe = kBsdfLobeSpecular;
    } else if (u.x < pDelta + pSpec + pCos) {
        wi = bsdfCosineSample(u.y, u.z);
        lobe = kBsdfLobeDiffuse;
        if ((m.flags & kBsdfFlagSssDiffusion) != 0u) {
            flags = kBsdfSampleSss;
        }
    } else {
        wi = bsdfCosineSample(u.y, u.z) * -1.0f;
        lobe = kBsdfLobeDiffuseTransmission;
        flags = kBsdfSampleTransmission;
    }
    if (wi.z == 0.0f || (lobe != kBsdfLobeDiffuseTransmission && wi.z < 0.0f)) {
        return bsdfSampleInvalid();
    }
    float pdf = bsdfOpaquePdfUpper(FUSE_BSDF_LUT_ARG m, wo, wi);
    if (!(pdf > 0.0f)) {
        return bsdfSampleInvalid();
    }
    return bsdfSampleMake(wi, bsdfOpaqueEvalUpper(FUSE_BSDF_LUT_ARG m, wo, wi) / pdf, pdf, flags, lobe);
}

FUSE_BSDF_FN BsdfSample bsdfOpaqueSample(FUSE_BSDF_LUT_PARAM BsdfMaterial m, float3 wo, float4 u) {
    float3 side = bsdfOpaqueSide(m, wo);
    BsdfSample s = bsdfOpaqueSampleUpper(FUSE_BSDF_LUT_ARG m, wo * side, u);
    s.wi = s.wi * side;
    return s;
}

// ---- translucent -------------------------------------------------------------------------------------------------

/// Dirac lobe weights of the translucent model at wo (already flipped to the upper side for thin walls).
/// wR / wT: reflection and transmission throughputs (thin walls: per-channel series; scalar otherwise).
FUSE_BSDF_FN void bsdfTranslucentLobes(BsdfMaterial m, float3 wo, bool inside, FUSE_BSDF_OUT(float3) wR,
                                       FUSE_BSDF_OUT(float3) wT, FUSE_BSDF_OUT(float) layer) {
    float ior = max(m.ior, 1.0f);
    float f0 = bsdfIorToF0(ior);
    float muo = abs(wo.z);
    bool thin = (m.flags & kBsdfFlagThinWalled) != 0u;
    float d = (m.flags & kBsdfFlagDiffuseLayer) != 0u ? bsdfSaturate(m.layerOpacity) : 0.0f;
    layer = inside ? 0.0f : d;
    if (thin) {
        float cost = sqrt(max(0.0f, 1.0f - (1.0f - muo * muo) / (ior * ior)));
        float fOut = bsdfSchlick(f0, muo);
        float fIn = bsdfSchlick(f0, cost);
        float3 sigma = bsdfAttenuationCoefficient(m.transmittance, 1.0f);
        float3 att = bsdfExp3(sigma * (-max(m.mediumDistance, 0.0f) / max(cost, kBsdfEpsilon)));
        float3 series = float3(1.0f / (1.0f - bsdfSqr(att.x * fIn)), 1.0f / (1.0f - bsdfSqr(att.y * fIn)),
                               1.0f / (1.0f - bsdfSqr(att.z * fIn)));
        float3 r = float3(fOut, fOut, fOut) + att * att * series * (fIn * (1.0f - fOut) * (1.0f - fIn));
        float3 t = att * series * ((1.0f - fOut) * (1.0f - fIn));
        if (fIn >= 1.0f || cost <= 0.0f) {
            r = float3(fOut, fOut, fOut);
            t = float3(0.0f, 0.0f, 0.0f);
        }
        wR = r * (1.0f - d);
        wT = t * ((1.0f - d) * (1.0f - d));
        return;
    }
    float f = inside ? bsdfSchlickTir(f0, ior, muo) : bsdfSchlick(f0, muo);
    wR = float3(f, f, f) * (inside ? 1.0f : 1.0f - d);
    wT = float3(1.0f - f, 1.0f - f, 1.0f - f) * (1.0f - d);
}

FUSE_BSDF_FN float bsdfTranslucentLayerWeight(BsdfMaterial m, float layer) {
    return layer > 0.0f ? max(layer * bsdfLum(m.layerColor), kBsdfLobeFloor) : 0.0f;
}

FUSE_BSDF_FN float3 bsdfTranslucentEval(BsdfMaterial m, float3 wo, float3 wi) {
    // Only the diffuse layer is non-dirac: Lambert d c / pi on the layer (outer) side.
    bool thin = (m.flags & kBsdfFlagThinWalled) != 0u;
    float side = wo.z >= 0.0f ? 1.0f : -1.0f;
    if ((m.flags & kBsdfFlagDiffuseLayer) == 0u || (!thin && side < 0.0f) || wi.z * side <= 0.0f) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    return m.layerColor * (bsdfSaturate(m.layerOpacity) * kBsdfInvPi * abs(wi.z));
}

FUSE_BSDF_FN float bsdfTranslucentPdf(BsdfMaterial m, float3 wo, float3 wi) {
    bool thin = (m.flags & kBsdfFlagThinWalled) != 0u;
    float side = wo.z >= 0.0f ? 1.0f : -1.0f;
    bool inside = !thin && side < 0.0f;
    float3 flip = float3(1.0f, 1.0f, side);
    float3 wR;
    float3 wT;
    float layer;
    bsdfTranslucentLobes(m, wo * flip, inside, wR, wT, layer);
    float wD = bsdfTranslucentLayerWeight(m, layer);
    float sum = bsdfLum(wR) + bsdfLum(wT) + wD;
    if (!(sum > 0.0f) || wD <= 0.0f || wi.z * side <= 0.0f) {
        return 0.0f;
    }
    return wD / sum * abs(wi.z) * kBsdfInvPi;
}

FUSE_BSDF_FN BsdfSample bsdfTranslucentSample(BsdfMaterial m, float3 wo, float4 u) {
    bool thin = (m.flags & kBsdfFlagThinWalled) != 0u;
    float side = wo.z >= 0.0f ? 1.0f : -1.0f;
    bool inside = !thin && side < 0.0f;
    float3 flip = float3(1.0f, 1.0f, side);
    float3 w = wo * flip;           // on the +z side
    float3 wR;
    float3 wT;
    float layer;
    bsdfTranslucentLobes(m, w, inside, wR, wT, layer);
    float pR = bsdfLum(wR);
    float pT = bsdfLum(wT);
    float pD = bsdfTranslucentLayerWeight(m, layer);
    float sum = pR + pT + pD;
    if (!(sum > 0.0f)) {
        return bsdfSampleInvalid();
    }
    pR = pR / sum;
    pT = pT / sum;
    pD = pD / sum;
    if (u.x < pR) {
        float3 wi = float3(-w.x, -w.y, w.z) * flip;
        return bsdfSampleMake(wi, wR / pR, pR, kBsdfSampleDelta, kBsdfLobeDeltaReflection);
    }
    if (u.x < pR + pT) {
        float3 wi;
        if (thin) {
            wi = wo * -1.0f;
        } else {
            float3 wt;
            float eta = inside ? max(m.ior, 1.0f) : 1.0f / max(m.ior, 1.0f);
            if (!bsdfRefract(w, float3(0.0f, 0.0f, 1.0f), eta, wt)) {
                return bsdfSampleInvalid();
            }
            wi = wt * flip;
        }
        return bsdfSampleMake(wi, wT / pT, pT, kBsdfSampleDelta | kBsdfSampleTransmission, kBsdfLobeDeltaTransmission);
    }
    float3 wl = bsdfCosineSample(u.y, u.z) * flip;
    float pdf = bsdfTranslucentPdf(m, wo, wl);
    if (!(pdf > 0.0f)) {
        return bsdfSampleInvalid();
    }
    return bsdfSampleMake(wl, bsdfTranslucentEval(m, wo, wl) / pdf, pdf, 0u, kBsdfLobeDiffuse);
}

// ---- hair (Chiang et al. 2016, d'Eon et al. 2011) ------------------------------------------------------------

/// Modified Bessel function I0 by its power series sum_k (x^2/4)^k / (k!)^2, each term from the previous one (no
/// overflow), 26 terms: converged to f32 for x <= 12 (FUSE: the 10-term form of pbrt truncates I0(12) by ~2%, which
/// makes the evaluated M_p disagree with its exact sampling). Its logarithm switches to the asymptotic expansion
/// e^x / sqrt(2 pi x) (1 + 1/(8x) + 9/(128x^2) + 225/(3072x^3)) above 12 (relative error < 1e-5).
FUSE_BSDF_FN float bsdfHairI0(float x) {
    float q = 0.25f * x * x;
    float term = 1.0f;
    float val = 1.0f;
    for (int k = 1; k < 26; ++k) {
        term = term * q / float(k * k);
        val = val + term;
    }
    return val;
}
FUSE_BSDF_FN float bsdfHairLogI0(float x) {
    if (x > 12.0f) {
        float r = 1.0f / x;
        return x - 0.5f * log(kBsdfTwoPi * x) + log(1.0f + r * (0.125f + r * (0.0703125f + r * 0.0732421875f)));
    }
    return log(bsdfHairI0(x));
}

/// Longitudinal scattering M_p (normalized: int M_p cos theta_i d theta_i = 1).
FUSE_BSDF_FN float bsdfHairMp(float cosThetaI, float cosThetaO, float sinThetaI, float sinThetaO, float v) {
    float a = cosThetaI * cosThetaO / v;
    float b = sinThetaI * sinThetaO / v;
    if (v <= 0.1f) {
        return exp(bsdfHairLogI0(a) - b - 1.0f / v + 0.6931f + log(1.0f / (2.0f * v)));
    }
    return exp(-b) * bsdfHairI0(a) / (bsdfSinh(1.0f / v) * 2.0f * v);
}

FUSE_BSDF_FN float bsdfHairLogistic(float x, float s) {
    float e = exp(-abs(x) / s);
    return e / (s * bsdfSqr(1.0f + e));
}
FUSE_BSDF_FN float bsdfHairLogisticCdf(float x, float s) { return 1.0f / (1.0f + exp(-x / s)); }
FUSE_BSDF_FN float bsdfHairTrimmedLogistic(float x, float s) {
    return bsdfHairLogistic(x, s) / (bsdfHairLogisticCdf(kBsdfPi, s) - bsdfHairLogisticCdf(-kBsdfPi, s));
}
FUSE_BSDF_FN float bsdfHairSampleTrimmedLogistic(float u, float s) {
    float lo = bsdfHairLogisticCdf(-kBsdfPi, s);
    float k = bsdfHairLogisticCdf(kBsdfPi, s) - lo;
    float x = -s * log(1.0f / (u * k + lo) - 1.0f);
    return clamp(x, -kBsdfPi, kBsdfPi);
}
FUSE_BSDF_FN float bsdfHairPhi(int p, float gammaO, float gammaT) {
    return 2.0f * float(p) * gammaT - 2.0f * gammaO + float(p) * kBsdfPi;
}
/// Azimuthal lobe N_p: trimmed logistic around Phi(p), the difference wrapped to [-pi, pi).
FUSE_BSDF_FN float bsdfHairNp(float phi, int p, float s, float gammaO, float gammaT) {
    float dphi = phi - bsdfHairPhi(p, gammaO, gammaT);
    return bsdfHairTrimmedLogistic(bsdfWrapAngle(dphi), s);
}

/// Per-(wo, material) hair terms.
struct BsdfHairCtx {
    float sinThetaO;
    float cosThetaO;
    float phiO;
    float gammaO;
    float gammaT;
    float s;
    float v0;
    float v1;
    float v2;
    float v3;
    float sin2kAlpha0;
    float sin2kAlpha1;
    float sin2kAlpha2;
    float cos2kAlpha0;
    float cos2kAlpha1;
    float cos2kAlpha2;
    float3 ap0;
    float3 ap1;
    float3 ap2;
    float3 ap3;
    float apPdf0;
    float apPdf1;
    float apPdf2;
    float apPdf3;
};

FUSE_BSDF_FN BsdfHairCtx bsdfHairContext(BsdfMaterial m, float3 wo) {
    BsdfHairCtx c;
    float eta = max(m.ior, 1.0001f);
    float h = clamp(m.hairH, -0.999f, 0.999f);
    float bm = clamp(m.hairBetaM, 0.02f, 1.0f);
    float bn = clamp(m.hairBetaN, 0.02f, 1.0f);
    c.sinThetaO = clamp(wo.x, -1.0f, 1.0f);
    c.cosThetaO = bsdfSafeSqrt(1.0f - c.sinThetaO * c.sinThetaO);
    c.phiO = bsdfAtan2(wo.z, wo.y);
    c.gammaO = bsdfAsin(h);
    // Longitudinal variances (Chiang eq. 7) and the logistic scale (eq. 8).
    float bm2 = bm * bm;
    float bm4 = bm2 * bm2;
    float bm20 = bm4 * bm4 * bm4 * bm4 * bm4;
    float v = bsdfSqr(0.726f * bm + 0.812f * bm2 + 3.7f * bm20);
    c.v0 = v;
    c.v1 = 0.25f * v;
    c.v2 = 4.0f * v;
    c.v3 = 4.0f * v;
    float bn2 = bn * bn;
    float bn4 = bn2 * bn2;
    float bn8 = bn4 * bn4;
    float bn22 = bn8 * bn8 * bn4 * bn2;
    c.s = 0.626657069f * (0.265f * bn + 1.194f * bn2 + 5.372f * bn22);
    // Cuticle tilt rotations sin / cos (2^k alpha).
    c.sin2kAlpha0 = sin(m.hairAlpha);
    c.cos2kAlpha0 = bsdfSafeSqrt(1.0f - c.sin2kAlpha0 * c.sin2kAlpha0);
    c.sin2kAlpha1 = 2.0f * c.cos2kAlpha0 * c.sin2kAlpha0;
    c.cos2kAlpha1 = bsdfSqr(c.cos2kAlpha0) - bsdfSqr(c.sin2kAlpha0);
    c.sin2kAlpha2 = 2.0f * c.cos2kAlpha1 * c.sin2kAlpha1;
    c.cos2kAlpha2 = bsdfSqr(c.cos2kAlpha1) - bsdfSqr(c.sin2kAlpha1);
    // Refracted ray inside the fiber and the attenuation A_p.
    float sinThetaT = c.sinThetaO / eta;
    float cosThetaT = bsdfSafeSqrt(1.0f - sinThetaT * sinThetaT);
    float etap = sqrt(max(eta * eta - c.sinThetaO * c.sinThetaO, 1e-8f)) / max(c.cosThetaO, 1e-6f);
    float sinGammaT = clamp(h / etap, -1.0f, 1.0f);
    float cosGammaT = bsdfSafeSqrt(1.0f - sinGammaT * sinGammaT);
    c.gammaT = bsdfAsin(sinGammaT);
    float3 tr = bsdfExp3(m.hairSigmaA * (-2.0f * cosGammaT / max(cosThetaT, 1e-6f)));
    float cosGammaO = bsdfSafeSqrt(1.0f - h * h);
    float f = bsdfFresnelDielectric(c.cosThetaO * cosGammaO, eta);
    c.ap0 = float3(f, f, f);
    c.ap1 = tr * bsdfSqr(1.0f - f);
    c.ap2 = c.ap1 * tr * f;
    c.ap3 = float3(c.ap2.x * f * tr.x / max(1.0f - tr.x * f, 1e-6f), c.ap2.y * f * tr.y / max(1.0f - tr.y * f, 1e-6f),
                   c.ap2.z * f * tr.z / max(1.0f - tr.z * f, 1e-6f));
    float l0 = bsdfLum(c.ap0);
    float l1 = bsdfLum(c.ap1);
    float l2 = bsdfLum(c.ap2);
    float l3 = bsdfLum(c.ap3);
    float sum = max(l0 + l1 + l2 + l3, 1e-12f);
    c.apPdf0 = l0 / sum;
    c.apPdf1 = l1 / sum;
    c.apPdf2 = l2 / sum;
    c.apPdf3 = l3 / sum;
    return c;
}

/// Rotates theta_o by the cuticle tilt of lobe p (p = 0: -2 alpha, 1: +alpha, 2: +4 alpha); returns (sin, cos).
FUSE_BSDF_FN void bsdfHairTilt(BsdfHairCtx c, int p, FUSE_BSDF_OUT(float) sinThetaOp, FUSE_BSDF_OUT(float) cosThetaOp) {
    if (p == 0) {
        sinThetaOp = c.sinThetaO * c.cos2kAlpha1 - c.cosThetaO * c.sin2kAlpha1;
        cosThetaOp = c.cosThetaO * c.cos2kAlpha1 + c.sinThetaO * c.sin2kAlpha1;
    } else if (p == 1) {
        sinThetaOp = c.sinThetaO * c.cos2kAlpha0 + c.cosThetaO * c.sin2kAlpha0;
        cosThetaOp = c.cosThetaO * c.cos2kAlpha0 - c.sinThetaO * c.sin2kAlpha0;
    } else {
        sinThetaOp = c.sinThetaO * c.cos2kAlpha2 + c.cosThetaO * c.sin2kAlpha2;
        cosThetaOp = c.cosThetaO * c.cos2kAlpha2 - c.sinThetaO * c.sin2kAlpha2;
    }
    cosThetaOp = abs(cosThetaOp);
}

FUSE_BSDF_FN float bsdfHairV(BsdfHairCtx c, int p) { return p == 0 ? c.v0 : (p == 1 ? c.v1 : (p == 2 ? c.v2 : c.v3)); }

/// Evaluates sum_p M_p A_p N_p (projected: the fiber BCSDF already carries its 1 / |cos theta_i|). With
/// `pdfOnly`, the A_p are replaced by their luminance selection probabilities and the result (x channel) is the pdf.
FUSE_BSDF_FN float3 bsdfHairTerms(BsdfHairCtx c, float3 wi, bool pdfOnly) {
    float sinThetaI = clamp(wi.x, -1.0f, 1.0f);
    float cosThetaI = bsdfSafeSqrt(1.0f - sinThetaI * sinThetaI);
    float phi = bsdfAtan2(wi.z, wi.y) - c.phiO;
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    for (int p = 0; p < 3; ++p) {
        float sinThetaOp;
        float cosThetaOp;
        bsdfHairTilt(c, p, sinThetaOp, cosThetaOp);
        float mn = bsdfHairMp(cosThetaI, cosThetaOp, sinThetaI, sinThetaOp, bsdfHairV(c, p)) *
                   bsdfHairNp(phi, p, c.s, c.gammaO, c.gammaT);
        float3 a = p == 0 ? c.ap0 : (p == 1 ? c.ap1 : c.ap2);
        float ap = p == 0 ? c.apPdf0 : (p == 1 ? c.apPdf1 : c.apPdf2);
        sum = sum + (pdfOnly ? float3(ap, ap, ap) : a) * mn;
    }
    float m3 = bsdfHairMp(cosThetaI, c.cosThetaO, sinThetaI, c.sinThetaO, c.v3) / kBsdfTwoPi;
    sum = sum + (pdfOnly ? float3(c.apPdf3, c.apPdf3, c.apPdf3) : c.ap3) * m3;
    return sum;
}

FUSE_BSDF_FN float3 bsdfHairEval(BsdfMaterial m, float3 wo, float3 wi) {
    return bsdfHairTerms(bsdfHairContext(m, wo), wi, false);
}
FUSE_BSDF_FN float bsdfHairPdf(BsdfMaterial m, float3 wo, float3 wi) {
    return bsdfHairTerms(bsdfHairContext(m, wo), wi, true).x;
}

FUSE_BSDF_FN BsdfSample bsdfHairSample(BsdfMaterial m, float3 wo, float4 u) {
    BsdfHairCtx c = bsdfHairContext(m, wo);
    int p = 3;
    if (u.x < c.apPdf0) {
        p = 0;
    } else if (u.x < c.apPdf0 + c.apPdf1) {
        p = 1;
    } else if (u.x < c.apPdf0 + c.apPdf1 + c.apPdf2) {
        p = 2;
    }
    float sinThetaOp = c.sinThetaO;
    float cosThetaOp = c.cosThetaO;
    if (p < 3) {
        bsdfHairTilt(c, p, sinThetaOp, cosThetaOp);
    }
    // Sample M_p (d'Eon: cos theta = 1 + v log(u + (1 - u) e^{-2/v})).
    float v = bsdfHairV(c, p);
    float u1 = max(u.y, 1e-5f);
    float cosTheta = 1.0f + v * log(u1 + (1.0f - u1) * exp(-2.0f / v));
    float sinTheta = bsdfSafeSqrt(1.0f - cosTheta * cosTheta);
    float cosPhi = cos(kBsdfTwoPi * u.z);
    float sinThetaI = clamp(-cosTheta * sinThetaOp + sinTheta * cosPhi * cosThetaOp, -1.0f, 1.0f);
    float cosThetaI = bsdfSafeSqrt(1.0f - sinThetaI * sinThetaI);
    // Sample N_p.
    float dphi = p < 3 ? bsdfHairPhi(p, c.gammaO, c.gammaT) + bsdfHairSampleTrimmedLogistic(u.w, c.s)
                       : kBsdfTwoPi * u.w;
    float phiI = bsdfWrapAngle(c.phiO + dphi);
    float3 wi = float3(sinThetaI, cosThetaI * cos(phiI), cosThetaI * sin(phiI));
    float pdf = bsdfHairTerms(c, wi, true).x;
    if (!(pdf > 0.0f)) {
        return bsdfSampleInvalid();
    }
    return bsdfSampleMake(wi, bsdfHairTerms(c, wi, false) / pdf, pdf, 0u, kBsdfLobeHair0 + uint(p));
}

/// Chiang 2016 eq. 9: absorption from a desired multiple-scattering colour and the azimuthal roughness.
FUSE_BSDF_FN float3 bsdfHairSigmaAFromColor(float3 color, float betaN) {
    float b = betaN;
    float b2 = b * b;
    float b3 = b2 * b;
    float den = 5.969f - 0.215f * b + 2.532f * b2 - 10.73f * b3 + 5.574f * b2 * b2 + 0.245f * b2 * b3;
    float3 c = max(color, float3(1e-4f, 1e-4f, 1e-4f));
    return float3(bsdfSqr(log(c.x) / den), bsdfSqr(log(c.y) / den), bsdfSqr(log(c.z) / den));
}

// ---- dispatch ----------------------------------------------------------------------------------------------------

/// Projected BSDF f(wo, wi) |cos theta_i| of every non-dirac lobe.
FUSE_BSDF_FN float3 bsdfEval(FUSE_BSDF_LUT_PARAM BsdfMaterial m, float3 wo, float3 wi) {
    if (m.model == kBsdfModelOpaque) {
        return bsdfOpaqueEval(FUSE_BSDF_LUT_ARG m, wo, wi);
    }
    if (m.model == kBsdfModelTranslucent) {
        return bsdfTranslucentEval(m, wo, wi);
    }
    if (m.model == kBsdfModelHair) {
        return bsdfHairEval(m, wo, wi);
    }
    return float3(0.0f, 0.0f, 0.0f);
}

/// Solid-angle pdf with which bsdfSample produces wi through a non-dirac lobe.
FUSE_BSDF_FN float bsdfPdf(FUSE_BSDF_LUT_PARAM BsdfMaterial m, float3 wo, float3 wi) {
    if (m.model == kBsdfModelOpaque) {
        return bsdfOpaquePdf(FUSE_BSDF_LUT_ARG m, wo, wi);
    }
    if (m.model == kBsdfModelTranslucent) {
        return bsdfTranslucentPdf(m, wo, wi);
    }
    if (m.model == kBsdfModelHair) {
        return bsdfHairPdf(m, wo, wi);
    }
    return 0.0f;
}

/// Samples wi from four uniforms (x: lobe; y, z: direction; w: hair azimuth).
FUSE_BSDF_FN BsdfSample bsdfSample(FUSE_BSDF_LUT_PARAM BsdfMaterial m, float3 wo, float4 u) {
    if (m.model == kBsdfModelOpaque) {
        return bsdfOpaqueSample(FUSE_BSDF_LUT_ARG m, wo, u);
    }
    if (m.model == kBsdfModelTranslucent) {
        return bsdfTranslucentSample(m, wo, u);
    }
    if (m.model == kBsdfModelHair) {
        return bsdfHairSample(m, wo, u);
    }
    return bsdfSampleInvalid();
}

// ---- packing -----------------------------------------------------------------------------------------------------

/// Inverse of bsdfMaterialPack (C++): kBsdfMaterialWords float4, integers stored as exact floats.
FUSE_BSDF_FN BsdfMaterial bsdfMaterialUnpack(FUSE_BSDF_WORDS_PARAM(w)) {
    BsdfMaterial m;
    m.model = uint(w[0].x);
    m.flags = uint(w[0].y);
    m.diffuseModel = uint(w[0].z);
    m.opacity = w[0].w;
    m.albedo = float3(w[1].x, w[1].y, w[1].z);
    m.roughness = w[1].w;
    m.anisotropy = w[2].x;
    m.metallic = w[2].y;
    m.ior = w[2].z;
    m.thinFilmThickness = w[2].w;
    m.transmittance = float3(w[3].x, w[3].y, w[3].z);
    m.mediumDistance = w[3].w;
    m.layerColor = float3(w[4].x, w[4].y, w[4].z);
    m.layerOpacity = w[4].w;
    m.sssTransmittance = float3(w[5].x, w[5].y, w[5].z);
    m.sssMeasurementDistance = w[5].w;
    m.sssSingleScatterAlbedo = float3(w[6].x, w[6].y, w[6].z);
    m.sssAnisotropy = w[6].w;
    m.sssRadius = float3(w[7].x, w[7].y, w[7].z);
    m.hairBetaM = w[7].w;
    m.hairBetaN = w[8].x;
    m.hairAlpha = w[8].y;
    m.hairH = w[8].z;
    m.hairSigmaA = float3(w[9].x, w[9].y, w[9].z);
    m.emission = float3(w[10].x, w[10].y, w[10].z);
    // w[8].w and w[10].w are padding.
    return m;
}
