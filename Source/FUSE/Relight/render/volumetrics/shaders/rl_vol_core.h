// FUSE Relight RL-5.6: volumetrics (froxel participating media lit by the RL-4.4 light set) and the particle composite
// (ray-traced billboards of the RL-3.6 particle system), one single-source core for the CPU reference and the GPU
// kernels (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.7, §5.8 rows `froxel_*`, `particle_sim`). FUSE's own code written from
// the papers cited below; no NVIDIA shader source was used. Include after rl_vol_types.h and the accessors (see there).
// Common subset of C++ / Slang / GLSL as light_core.h (HLSL type names, no `const` locals, no lerp / mix / saturate,
// f-suffixed literals, every struct field assigned before use).
//
// GRID. gx x gy columns over the screen (column (x, y) covers screen fraction [x / gx, (x + 1) / gx) x ...; screen y
// down), gz depth slices along the camera forward: slice k spans view depth [b(k), b(k + 1)] with b(0) = 0 and
// b(k) = nearZ x (farZ / nearZ)^(k / gz) for k >= 1 (the WP-8.1 / clustered exponential distribution with the first
// slice extended to the eye). A continuous slice coordinate c in [0, gz] maps to depth by volDepthAt (linear inside
// slice 0, exponential after), and back by volCoordAt. Froxel index = (y x gx + x) x gz + z.
//
// INJECT (per froxel, per frame). A jittered point p of the froxel (counter-based random numbers: the PCG hash of
// [Jarzynski and Olano 2020, JCGT 9(3)]); extinction sigma_t(p) = density x exp(-falloff x max(0, p.y - baseHeight))
// (exponential height fog), scattering sigma_s = albedo x sigma_t. In-scattered radiance towards the camera
//   L_s(p) = ambient + lightScale x integral over the sphere of Li(p, wi) x V(p, wi) x phase(wi . v) x volumetricScale
// with the Henyey-Greenstein phase function [Henyey and Greenstein 1941] (v = the view ray direction, wi towards the
// light; g > 0 scatters forward, towards a camera looking at the light). The light integral is estimated by resampled
// importance sampling [Talbot et al. 2005, "Importance Resampling for Global Illumination"; the streaming reservoir
// of Bitterli et al. 2020, "Spatiotemporal reservoir resampling"]: `candidates` samples of the RL-4.4 light set (the
// WP-7.1 light tree's selection x the light's own sample, with a zero normal: the tree treats the point as a volume)
// with target p^ = luminance(Li) x phase x volumetricScale (unshadowed), resampled in primary sample space (the
// candidates' random numbers u; target p^(y(u)) / pdf(y(u)), uniform source density), one sample kept with
// W = sum(w) / (M p^_pss(u)); the estimate is f(y) / pdf(y) x W with the shadow ray only for y (visibility reuse).
// Unbiased without reuse. With kVolFlagReuse the previous frame's reservoir at the reprojected froxel is merged into
// the stream by replaying its random numbers at the current point (the primary-sample-space shift of [Lin et al.
// 2022, "Generalized Resampled Importance Sampling"], Jacobian 1; M capped at mCap x candidates): the "ported Remix
// concept" of reservoir reuse for volumetrics; biased (no MIS between the domains, occlusion is not part of the
// target), bounded by the M cap. Reservoir words: (u0, u1, u2, light or -1), (W, M, p^_pss(u), 0). Stores
// current = (sigma_s x L_s, sigma_t) and the reservoir.
//
// TEMPORAL. history = prev + (current - prev) x alpha, prev = the previous history trilinearly sampled at the froxel
// centre reprojected through the previous camera (kVolFlagReproject; clamp to edge, current alone when the centre
// was outside the previous volume) or the same froxel. Without kVolFlagHistory the current values are copied.
//
// INTEGRATE (per column, front to back). Step length ds = slice thickness x |ray| (the column centre's view ray per
// unit depth); energy-conserving step [Hillaire 2015, "Physically Based and Unified Volumetric Rendering in
// Frostbite"]: S_int = S (1 - e^-x) / sigma with x = sigma ds (a 7-term series below x = 0.1), accum += T S_int,
// T *= e^-x; stored at the slice's far boundary.
//
// APPLY (per pixel). The pixel's view depth (0: sky = farZ) -> the integrated (in-scatter, transmittance) at that depth
// (exact between the slice boundaries for the slice's constant medium, volColumnFog; each of the four nearest
// columns rescaled to the pixel's ray length, volColumnFogFor, then bilinear), and the
// particle layers in front of the opaque surface: every quad of every particle system (4 vertices in strip order, two
// triangles, Moller-Trumbore [Moller and Trumbore 1997]) hit by the pixel's centre ray before the opaque depth, the
// kVolMaxLayers nearest kept sorted by distance. The composite runs far to near, which is the D3D painter's result
// of back-to-front sorted particles with the system's blend state, with the medium between consecutive layers
// (segment [z_i, z_j]: T = T(z_j) / T(z_i), S = (S(z_j) - S(z_i)) / T(z_i)):
//   C = colour; for each layer from the farthest: C = C x T_seg + S_seg; C = blend(C, layer); finally the segment from
//   the eye. Particles are unlit (vertex colour x the optional procedural disc alpha).

VOL_CONST float kVolPi = 3.14159265358979f;
VOL_CONST float kVolInv4Pi = 0.0795774715459477f;
VOL_CONST float kVolFar = 1e30f;
VOL_CONST float kVolSeriesDepth = 0.1f;
VOL_CONST uint kVolNone = 0xFFFFFFFFu;

// ---- random numbers -----------------------------------------------------------------------------------------------

VOL_FN uint volHash(uint x) {
    uint state = x * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

/// Uniform in [0, 1) for (a, b, dimension).
VOL_FN float volRandom(uint a, uint b, uint dim) {
    return float(volHash(a ^ volHash(b ^ volHash(dim + 0x9E3779B9u))) >> 8u) * (1.0f / 16777216.0f);
}

// ---- small helpers ------------------------------------------------------------------------------------------------

VOL_FN float volLuminance(float3 c) { return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f; }

VOL_FN float volPhaseHG(float g, float cosTheta) {
    float g2 = g * g;
    float d = max(1.0f + g2 - 2.0f * g * cosTheta, 1e-6f);
    return kVolInv4Pi * (1.0f - g2) / (d * sqrt(d));
}

VOL_FN float volExtinction(VolParams P, float3 p) {
    return P.density * exp(-P.falloff * max(0.0f, p.y - P.baseHeight));
}

VOL_FN uint volFroxelIndex(VolParams P, uint x, uint y, uint z) { return (y * P.gx + x) * P.gz + z; }

/// Depth of the continuous slice coordinate c (see GRID).
VOL_FN float volDepthAt(VolParams P, float c) {
    float b1 = P.nearZ * pow(P.farZ / P.nearZ, 1.0f / float(P.gz));
    if (c <= 1.0f) {
        return max(c, 0.0f) * b1;
    }
    return P.nearZ * pow(P.farZ / P.nearZ, c / float(P.gz));
}

/// Slice coordinate of a view depth (inverse of volDepthAt), clamped to [0, gz].
VOL_FN float volCoordAt(VolParams P, float depth) {
    float b1 = P.nearZ * pow(P.farZ / P.nearZ, 1.0f / float(P.gz));
    if (depth <= b1) {
        return max(depth, 0.0f) / b1;
    }
    float c = float(P.gz) * log(depth / P.nearZ) / log(P.farZ / P.nearZ);
    return min(max(c, 1.0f), float(P.gz));
}

/// View ray per unit depth through screen fraction (sx, sy) (y down).
VOL_FN float3 volRay(VolParams P, float sx, float sy) {
    return P.camFwd + P.camRight * (sx * 2.0f - 1.0f) + P.camUp * (1.0f - sy * 2.0f);
}

/// Continuous froxel coordinates (x, y in column units with centres at integers, z in slice coordinate units with
/// centres at k + 0.5) of world point p in the PREVIOUS camera; false outside its volume.
VOL_FN bool volPrevCoords(VolParams P, float3 p, VOL_OUT(float) fx, VOL_OUT(float) fy, VOL_OUT(float) fz) {
    fx = 0.0f;
    fy = 0.0f;
    fz = 0.0f;
    float3 rel = p - P.prevPos;
    float zf = dot(rel, P.prevFwd);
    if (!(zf > 0.0f) || zf > P.farZ) {
        return false;
    }
    float nx = dot(rel, P.prevRight) / (zf * dot(P.prevRight, P.prevRight));
    float ny = dot(rel, P.prevUp) / (zf * dot(P.prevUp, P.prevUp));
    float sx = (nx + 1.0f) * 0.5f;
    float sy = (1.0f - ny) * 0.5f;
    if (!(sx >= 0.0f && sx <= 1.0f && sy >= 0.0f && sy <= 1.0f)) {
        return false;
    }
    fx = sx * float(P.gx) - 0.5f;
    fy = sy * float(P.gy) - 0.5f;
    fz = volCoordAt(P, zf) - 0.5f;
    return true;
}

VOL_FN uint volClampIndex(float f, uint n) {
    float c = min(max(floor(f + 0.5f), 0.0f), float(n - 1u));
    return uint(c);
}

// ---- inject -------------------------------------------------------------------------------------------------------

/// Light-set sample of the random numbers u at p: its unshadowed contribution f (radiance x phase x volumetricScale),
/// the RIS target p^ = luminance(f) and the light-set density; false when there is no sample.
VOL_FN bool volLightSample(VOL_CTX_PARAM VolParams P, float3 p, float3 v, float u0, float u1, float u2,
                           VOL_OUT(VolLightPick) s, VOL_OUT(float3) f, VOL_OUT(float) target) {
    s = volSampleLight(VOL_CTX_ARG p, u0, u1, u2);
    f = float3(0.0f, 0.0f, 0.0f);
    target = 0.0f;
    if (s.light >= P.lightCount || !(s.pdf > 0.0f) || (s.flags & kRlSampleValid) == 0u) {
        return false;
    }
    RlLight L = volLoadLight(VOL_CTX_ARG s.light);
    f = s.radiance * (volPhaseHG(P.g, dot(s.wi, v)) * L.volumetricScale);
    target = volLuminance(f);
    return target > 0.0f;
}

VOL_FN void volInject(VOL_CTX_PARAM VolParams P, uint x, uint y, uint z) {
    uint i = volFroxelIndex(P, x, y, z);
    float jx = volRandom(i, P.frame, 0u);
    float jy = volRandom(i, P.frame, 1u);
    float jz = volRandom(i, P.frame, 2u);
    float3 ray = volRay(P, (float(x) + jx) / float(P.gx), (float(y) + jy) / float(P.gy));
    float depth = volDepthAt(P, float(z) + jz);
    float3 p = P.camPos + ray * depth;
    float3 v = normalize(ray);
    float sigmaT = volExtinction(P, p);
    float3 ls = P.ambient;

    // Reservoir in primary sample space: the selected sample's random numbers u, its contribution f / pdf, target.
    float3 yU = float3(0.0f, 0.0f, 0.0f);
    bool ySel = false;
    VolLightPick yS;
    yS.light = kVolNone;
    yS.pdf = 0.0f;
    yS.wi = float3(0.0f, 0.0f, 1.0f);
    yS.dist = 0.0f;
    yS.radiance = float3(0.0f, 0.0f, 0.0f);
    yS.flags = 0u;
    float3 yF = float3(0.0f, 0.0f, 0.0f);
    float yHat = 0.0f; // target in primary sample space: p^ / pdf
    float wSum = 0.0f;
    float m = 0.0f;
    if ((P.flags & kVolFlagLights) != 0u && P.lightCount > 0u && sigmaT > 0.0f) {
        uint n = min(P.candidates, kVolMaxCandidates);
        for (uint k = 0u; k < n; ++k) {
            float u0 = volRandom(i, P.frame, 3u + k * 4u);
            float u1 = volRandom(i, P.frame, 4u + k * 4u);
            float u2 = volRandom(i, P.frame, 5u + k * 4u);
            m += 1.0f;
            VolLightPick s;
            float3 f;
            float target;
            if (!volLightSample(VOL_CTX_ARG P, p, v, u0, u1, u2, s, f, target)) {
                continue;
            }
            float w = target / s.pdf;
            wSum += w;
            if (volRandom(i, P.frame, 6u + k * 4u) * wSum < w) {
                ySel = true;
                yU = float3(u0, u1, u2);
                yS = s;
                yF = f;
                yHat = w;
            }
        }
        if ((P.flags & kVolFlagReuse) != 0u && (P.flags & kVolFlagHistory) != 0u) {
            float fx;
            float fy;
            float fz;
            if (volPrevCoords(P, p, fx, fy, fz)) {
                uint pi = volFroxelIndex(P, volClampIndex(fx, P.gx), volClampIndex(fy, P.gy), volClampIndex(fz, P.gz));
                float4 r0 = volLoad(VOL_CTX_ARG kVolBufResPrev, pi * 2u);
                float4 r1 = volLoad(VOL_CTX_ARG kVolBufResPrev, pi * 2u + 1u);
                float mp = min(r1.y, P.mCap * float(max(n, 1u)));
                if (r0.w >= 0.0f && r1.x > 0.0f && mp > 0.0f) {
                    // Replay the previous sample's random numbers here (primary-sample-space shift, Jacobian 1).
                    VolLightPick s;
                    float3 f;
                    float target;
                    m += mp;
                    if (volLightSample(VOL_CTX_ARG P, p, v, r0.x, r0.y, r0.z, s, f, target)) {
                        float hat = target / s.pdf;
                        float w = hat * r1.x * mp;
                        wSum += w;
                        if (volRandom(i, P.frame, 3u + kVolMaxCandidates * 4u) * wSum < w) {
                            ySel = true;
                            yU = float3(r0.x, r0.y, r0.z);
                            yS = s;
                            yF = f;
                            yHat = hat;
                        }
                    }
                }
            }
        }
    }
    float W = 0.0f; // contribution weight in primary sample space: wSum / (M p^_pss(u))
    if (ySel && yHat > 0.0f && m > 0.0f) {
        W = wSum / (m * yHat);
        float3 f = yF * (1.0f / yS.pdf);
        if ((P.flags & kVolFlagShadows) != 0u) {
            float tmax = yS.dist >= kVolFar ? kVolFar : yS.dist * (1.0f - 1e-4f) - P.rayEps;
            if (tmax > P.rayEps && volOccluded(VOL_CTX_ARG p, yS.wi, P.rayEps, tmax)) {
                f = float3(0.0f, 0.0f, 0.0f);
            }
        }
        ls = ls + f * (W * P.lightScale);
    }
    volStore(VOL_CTX_ARG kVolBufResCur, i * 2u, float4(yU.x, yU.y, yU.z, ySel ? float(yS.light) : -1.0f));
    volStore(VOL_CTX_ARG kVolBufResCur, i * 2u + 1u, float4(W, m, yHat, 0.0f));
    float3 scatter = P.albedo * ls * sigmaT;
    volStore(VOL_CTX_ARG kVolBufCurrent, i, float4(scatter.x, scatter.y, scatter.z, sigmaT));
}

// ---- temporal -----------------------------------------------------------------------------------------------------

VOL_FN float4 volLerp4(float4 a, float4 b, float t) {
    return float4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

VOL_FN float4 volHistoryTrilinear(VOL_CTX_PARAM VolParams P, float fx, float fy, float fz) {
    fx = min(max(fx, 0.0f), float(P.gx - 1u));
    fy = min(max(fy, 0.0f), float(P.gy - 1u));
    fz = min(max(fz, 0.0f), float(P.gz - 1u));
    uint x0 = uint(floor(fx));
    uint y0 = uint(floor(fy));
    uint z0 = uint(floor(fz));
    uint x1 = min(x0 + 1u, P.gx - 1u);
    uint y1 = min(y0 + 1u, P.gy - 1u);
    uint z1 = min(z0 + 1u, P.gz - 1u);
    float tx = fx - float(x0);
    float ty = fy - float(y0);
    float tz = fz - float(z0);
    float4 a = volLerp4(volLoad(VOL_CTX_ARG kVolBufHistPrev, volFroxelIndex(P, x0, y0, z0)),
                        volLoad(VOL_CTX_ARG kVolBufHistPrev, volFroxelIndex(P, x1, y0, z0)), tx);
    float4 b = volLerp4(volLoad(VOL_CTX_ARG kVolBufHistPrev, volFroxelIndex(P, x0, y1, z0)),
                        volLoad(VOL_CTX_ARG kVolBufHistPrev, volFroxelIndex(P, x1, y1, z0)), tx);
    float4 c = volLerp4(volLoad(VOL_CTX_ARG kVolBufHistPrev, volFroxelIndex(P, x0, y0, z1)),
                        volLoad(VOL_CTX_ARG kVolBufHistPrev, volFroxelIndex(P, x1, y0, z1)), tx);
    float4 d = volLerp4(volLoad(VOL_CTX_ARG kVolBufHistPrev, volFroxelIndex(P, x0, y1, z1)),
                        volLoad(VOL_CTX_ARG kVolBufHistPrev, volFroxelIndex(P, x1, y1, z1)), tx);
    return volLerp4(volLerp4(a, b, ty), volLerp4(c, d, ty), tz);
}

VOL_FN void volTemporal(VOL_CTX_PARAM VolParams P, uint x, uint y, uint z) {
    uint i = volFroxelIndex(P, x, y, z);
    float4 cur = volLoad(VOL_CTX_ARG kVolBufCurrent, i);
    float4 outv = cur;
    if ((P.flags & kVolFlagHistory) != 0u) {
        if ((P.flags & kVolFlagReproject) != 0u) {
            float3 ray = volRay(P, (float(x) + 0.5f) / float(P.gx), (float(y) + 0.5f) / float(P.gy));
            float3 pc = P.camPos + ray * volDepthAt(P, float(z) + 0.5f);
            float fx;
            float fy;
            float fz;
            if (volPrevCoords(P, pc, fx, fy, fz)) {
                outv = volLerp4(volHistoryTrilinear(VOL_CTX_ARG P, fx, fy, fz), cur, P.alpha);
            }
        } else {
            outv = volLerp4(volLoad(VOL_CTX_ARG kVolBufHistPrev, i), cur, P.alpha);
        }
    }
    volStore(VOL_CTX_ARG kVolBufHistCur, i, outv);
}

// ---- integrate ----------------------------------------------------------------------------------------------------

/// (1 - e^-x) / x.
VOL_FN float volStepFactor(float x) {
    if (x < kVolSeriesDepth) {
        return 1.0f - x * (0.5f - x * (1.0f / 6.0f - x * (1.0f / 24.0f - x * (1.0f / 120.0f - x * (1.0f / 720.0f - x * (1.0f / 5040.0f))))));
    }
    return (1.0f - exp(-x)) / x;
}

VOL_FN void volIntegrate(VOL_CTX_PARAM VolParams P, uint x, uint y) {
    float3 ray = volRay(P, (float(x) + 0.5f) / float(P.gx), (float(y) + 0.5f) / float(P.gy));
    float rayLen = length(ray);
    float3 s = float3(0.0f, 0.0f, 0.0f);
    float t = 1.0f;
    float lower = 0.0f;
    for (uint z = 0u; z < P.gz; ++z) {
        uint i = volFroxelIndex(P, x, y, z);
        float4 h = volLoad(VOL_CTX_ARG kVolBufHistCur, i);
        float upper = volDepthAt(P, float(z + 1u));
        float ds = (upper - lower) * rayLen;
        float od = max(h.w, 0.0f) * ds;
        float3 sInt = float3(h.x, h.y, h.z) * (ds * volStepFactor(od));
        s = s + sInt * t;
        t = t * exp(-od);
        volStore(VOL_CTX_ARG kVolBufIntegrated, i, float4(s.x, s.y, s.z, t));
        lower = upper;
    }
}

// ---- apply --------------------------------------------------------------------------------------------------------

/// (in-scattering, transmittance) of column (x, y) at view depth `depth` (slice coordinate c): exact inside the slice
/// for its constant medium - T(f) = T0 (T1 / T0)^f and S(f) = S0 + (S1 - S0) (1 - T(f) / T0) / (1 - T1 / T0), with f the
/// depth fraction - and linear where the slice is (nearly) transparent.
VOL_FN float4 volColumnFog(VOL_CTX_PARAM VolParams P, uint x, uint y, float c, float depth) {
    uint k = min(uint(floor(c)), P.gz - 1u);
    float d0 = volDepthAt(P, float(k));
    float d1 = volDepthAt(P, float(k + 1u));
    float f = d1 > d0 ? min(max((depth - d0) / (d1 - d0), 0.0f), 1.0f) : 1.0f;
    float4 lo = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (k > 0u) {
        lo = volLoad(VOL_CTX_ARG kVolBufIntegrated, volFroxelIndex(P, x, y, k - 1u));
    }
    float4 hi = volLoad(VOL_CTX_ARG kVolBufIntegrated, volFroxelIndex(P, x, y, k));
    float ratio = lo.w > 0.0f ? hi.w / lo.w : 1.0f;
    if (!(ratio < 0.9999f) || !(ratio > 0.0f)) {
        return volLerp4(lo, hi, f);
    }
    float tf = pow(ratio, f);
    float w = (1.0f - tf) / (1.0f - ratio);
    return float4(lo.x + (hi.x - lo.x) * w, lo.y + (hi.y - lo.y) * w, lo.z + (hi.z - lo.z) * w, lo.w * tf);
}

/// Column (x, y)'s (in-scattering, transmittance) at view depth `depth`, rescaled from the column centre's view ray to
/// a ray `rayLen` long per unit depth (exact for a medium homogeneous along the ray: T' = T^k, S' = S (1 - T') / (1 - T),
/// k = rayLen / |column ray|).
VOL_FN float4 volColumnFogFor(VOL_CTX_PARAM VolParams P, uint x, uint y, float c, float depth, float rayLen) {
    float4 f = volColumnFog(VOL_CTX_ARG P, x, y, c, depth);
    float colLen = length(volRay(P, (float(x) + 0.5f) / float(P.gx), (float(y) + 0.5f) / float(P.gy)));
    float k = rayLen / colLen;
    if (!(f.w < 0.9999f)) {
        return float4(f.x * k, f.y * k, f.z * k, 1.0f - (1.0f - f.w) * k);
    }
    float t = pow(max(f.w, 0.0f), k);
    float w = (1.0f - t) / (1.0f - f.w);
    return float4(f.x * w, f.y * w, f.z * w, t);
}

/// (in-scattering, transmittance) from the eye to view depth `depth` at screen fraction (sx, sy).
VOL_FN float4 volFogAt(VOL_CTX_PARAM VolParams P, float sx, float sy, float depth) {
    float dz = min(depth, P.farZ);
    float c = volCoordAt(P, dz);
    float rayLen = length(volRay(P, sx, sy));
    float fx = min(max(sx * float(P.gx) - 0.5f, 0.0f), float(P.gx - 1u));
    float fy = min(max(sy * float(P.gy) - 0.5f, 0.0f), float(P.gy - 1u));
    uint x0 = uint(floor(fx));
    uint y0 = uint(floor(fy));
    uint x1 = min(x0 + 1u, P.gx - 1u);
    uint y1 = min(y0 + 1u, P.gy - 1u);
    float tx = fx - float(x0);
    float ty = fy - float(y0);
    float4 a = volLerp4(volColumnFogFor(VOL_CTX_ARG P, x0, y0, c, dz, rayLen),
                        volColumnFogFor(VOL_CTX_ARG P, x1, y0, c, dz, rayLen), tx);
    float4 b = volLerp4(volColumnFogFor(VOL_CTX_ARG P, x0, y1, c, dz, rayLen),
                        volColumnFogFor(VOL_CTX_ARG P, x1, y1, c, dz, rayLen), tx);
    return volLerp4(a, b, ty);
}

/// Ray / triangle (Moller-Trumbore); t in (0, tmax), barycentrics (b1, b2). `sumSlack` / `b2Slack` widen the b1 + b2 <= 1
/// and b2 >= 0 edges: the quad's two triangles overlap by that much on their shared diagonal, so no ray slips between
/// them (the first triangle that reports a hit wins; the quad is counted once).
VOL_FN bool volHitTriangle(float3 o, float3 d, float3 a, float3 b, float3 c, float tmax, float sumSlack, float b2Slack,
                           VOL_OUT(float) t, VOL_OUT(float) b1, VOL_OUT(float) b2) {
    t = 0.0f;
    b1 = 0.0f;
    b2 = 0.0f;
    float3 e1 = b - a;
    float3 e2 = c - a;
    float3 pv = cross(d, e2);
    float det = dot(e1, pv);
    if (abs(det) < 1e-12f) {
        return false;
    }
    float inv = 1.0f / det;
    float3 tv = o - a;
    b1 = dot(tv, pv) * inv;
    if (b1 < 0.0f || b1 > 1.0f) {
        return false;
    }
    float3 qv = cross(tv, e1);
    b2 = dot(d, qv) * inv;
    if (b2 < -b2Slack || b1 + b2 > 1.0f + sumSlack) {
        return false;
    }
    t = dot(e2, qv) * inv;
    return t > 0.0f && t < tmax;
}

VOL_FN float3 volBlend(float3 dst, float4 src, uint mode) {
    float3 rgb = float3(src.x, src.y, src.z);
    if (mode == kVolBlendAdditive) {
        return dst + rgb * src.w;
    }
    if (mode == kVolBlendPremultiplied) {
        return rgb + dst * (1.0f - src.w);
    }
    if (mode == kVolBlendMultiply) {
        return dst * rgb;
    }
    return rgb * src.w + dst * (1.0f - src.w);
}

VOL_FN void volApply(VOL_CTX_PARAM VolParams P, uint px, uint py) {
    uint i = py * P.width + px;
    float4 color = volLoad(VOL_CTX_ARG kVolBufColorIn, i);
    float depth = volLoadDepth(VOL_CTX_ARG i);
    float sx = (float(px) + 0.5f) / float(P.width);
    float sy = (float(py) + 0.5f) / float(P.height);
    float3 ray = volRay(P, sx, sy);
    float3 dir = normalize(ray);
    float depthScale = dot(dir, P.camFwd);
    float bgDepth = depth > 0.0f ? depth : P.farZ;
    bool fog = (P.flags & kVolFlagFog) != 0u;

    // Particle layers (nearest kVolMaxLayers, sorted near to far).
    float layerT[16];
    float4 layerC[16];
    uint layerMode[16];
    uint layers = 0u;
    for (uint k = 0u; k < 16u; ++k) {
        layerT[k] = 0.0f;
        layerC[k] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        layerMode[k] = 0u;
    }
    if ((P.flags & kVolFlagParticles) != 0u) {
        float tOpaque = depth > 0.0f ? depth / depthScale : kVolFar;
        for (uint sys = 0u; sys < min(P.systemCount, kVolMaxSystems); ++sys) {
            uint4 rec = volLoadSystem(VOL_CTX_ARG sys);
            for (uint q = 0u; q < rec.y; ++q) {
                uint base = (rec.x + q) * 4u;
                VolVertex v0 = volLoadVertex(VOL_CTX_ARG base);
                VolVertex v1 = volLoadVertex(VOL_CTX_ARG base + 1u);
                VolVertex v2 = volLoadVertex(VOL_CTX_ARG base + 2u);
                VolVertex v3 = volLoadVertex(VOL_CTX_ARG base + 3u);
                float t;
                float b1;
                float b2;
                float lu = 0.0f;
                float lv = 0.0f;
                float4 c = float4(0.0f, 0.0f, 0.0f, 0.0f);
                bool hit = false;
                // Strip order: (v0, v1, v2) and (v2, v1, v3); quad-local corners (0,0) (1,0) (0,1) (1,1).
                if (volHitTriangle(P.camPos, dir, v0.position, v1.position, v2.position, tOpaque, 1e-5f, 0.0f, t, b1, b2)) {
                    hit = true;
                    float b0 = 1.0f - b1 - b2;
                    lu = b1;
                    lv = b2;
                    c = float4(v0.color.x * b0 + v1.color.x * b1 + v2.color.x * b2,
                               v0.color.y * b0 + v1.color.y * b1 + v2.color.y * b2,
                               v0.color.z * b0 + v1.color.z * b1 + v2.color.z * b2,
                               v0.color.w * b0 + v1.color.w * b1 + v2.color.w * b2);
                } else if (volHitTriangle(P.camPos, dir, v2.position, v1.position, v3.position, tOpaque, 0.0f, 1e-5f, t, b1, b2)) {
                    hit = true;
                    float b0 = 1.0f - b1 - b2;
                    lu = b1 + b2;
                    lv = b0 + b2;
                    c = float4(v2.color.x * b0 + v1.color.x * b1 + v3.color.x * b2,
                               v2.color.y * b0 + v1.color.y * b1 + v3.color.y * b2,
                               v2.color.z * b0 + v1.color.z * b1 + v3.color.z * b2,
                               v2.color.w * b0 + v1.color.w * b1 + v3.color.w * b2);
                }
                if (!hit) {
                    continue;
                }
                if ((rec.w & kVolSystemSoftDisc) != 0u) {
                    float du = lu * 2.0f - 1.0f;
                    float dv = lv * 2.0f - 1.0f;
                    c.w = c.w * max(0.0f, 1.0f - (du * du + dv * dv));
                }
                // Insert sorted by t (ties: the earlier quad stays nearer).
                if (layers == kVolMaxLayers && !(t < layerT[kVolMaxLayers - 1u])) {
                    continue;
                }
                uint at = layers < kVolMaxLayers ? layers : kVolMaxLayers - 1u;
                while (at > 0u && t < layerT[at - 1u]) {
                    layerT[at] = layerT[at - 1u];
                    layerC[at] = layerC[at - 1u];
                    layerMode[at] = layerMode[at - 1u];
                    at = at - 1u;
                }
                layerT[at] = t;
                layerC[at] = c;
                layerMode[at] = rec.z;
                layers = min(layers + 1u, kVolMaxLayers);
            }
        }
    }

    float3 col = float3(color.x, color.y, color.z);
    float4 prev = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (fog) {
        prev = volFogAt(VOL_CTX_ARG P, sx, sy, bgDepth);
    }
    for (uint n = 0u; n < layers; ++n) {
        uint k = layers - 1u - n;
        if (fog) {
            float4 f = volFogAt(VOL_CTX_ARG P, sx, sy, layerT[k] * depthScale);
            float tSeg = f.w > 0.0f ? min(prev.w / f.w, 1.0f) : 0.0f;
            float invT = f.w > 0.0f ? 1.0f / f.w : 0.0f;
            float3 sSeg = float3(max(prev.x - f.x, 0.0f), max(prev.y - f.y, 0.0f), max(prev.z - f.z, 0.0f)) * invT;
            col = col * tSeg + sSeg;
            prev = f;
        }
        col = volBlend(col, layerC[k], layerMode[k]);
    }
    col = col * prev.w + float3(prev.x, prev.y, prev.z);
    volStore(VOL_CTX_ARG kVolBufColorOut, i, float4(col.x, col.y, col.z, color.w));
}
