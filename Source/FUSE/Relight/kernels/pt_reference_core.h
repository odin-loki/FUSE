// FUSE Relight RL-5.1: the path-tracing core (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.1 "Primary: ray-query G-buffer +
// PSR", §5.2, §5.3, §5.8 row `pt_reference`). FUSE's own code, written from the papers cited below; no NVIDIA shader
// source was used (the Remix concepts it follows - primary / secondary surface replacement, ray portals, legacy alpha
// test and blending - are described from the MIT-licensed host code and public documentation only).
//
// SINGLE SOURCE. This file is compiled three ways, after the RL-4.3 BSDF core (bsdf_core.h), the RL-4.4 light core
// (light_core.h) and the path tracer's records (pt_reference_types.h), with the dialect macros and scene accessors set
// by the includer (the accessors are declared between pt_reference_types.h and this file):
//   C++   kernels/pt_reference_cpp.hpp        the CPU reference path tracer (scene: the WP-6.0 CPU reference BVH,
//                                             RtReferenceScene; lights: RelightLightSet's CPU sampler);
//   Slang render/pathtrace/shaders/rl_pt.slang (ray query on the WP-6.0 TLAS; lights: rl_lights.slang);
//   GLSL  render/pathtrace/shaders/rl_pt.glsl  (the fallback for glslangValidator-only hosts).
// Common subset of the three (as bsdf_core.h / light_core.h): HLSL-style type names, no `const` locals, no lerp / mix /
// saturate, f-suffixed literals, every struct field assigned before use, no GLSL / HLSL keywords as identifiers.
//
// Required macros: PT_FN, PT_CONST, PT_OUT(T), PT_INOUT(T), PT_CTX_PARAM / PT_CTX_ARG (the scene context: C++
// `const PtCpuContext& ctx,` / `ctx,`, shaders empty), PT_LUT_ARG (the BSDF albedo table argument: FUSE_BSDF_LUT_ARG
// in shaders, `ctx.lut,` in C++), PT_L3(v) / PT_B3(v) (vector conversion between the BSDF and light dialect types in
// C++; identity in shaders), PT_PARAM_WORDS(name) (float4[kPtParamWords]), PT_MATERIAL_WORDS(name)
// (float4[kPtMaterialWords]).
// Required accessors (declared by the includer before this file):
//   PtRawHit ptRawTrace(PT_CTX_PARAM float3 o, float3 d, float tmin, float tmax, uint cullMask);
//                                        closest triangle hit of the scene (every instance opaque to traversal)
//   float4 ptInstanceWord(PT_CTX_PARAM uint slot, uint k);     k < kPtInstanceWords
//   float4 ptTriangleWord(PT_CTX_PARAM uint triangle, uint k); k < kPtTriangleWords
//   float4 ptMaterialWord(PT_CTX_PARAM uint material, uint k); k < kPtMaterialWords
//   float4 ptPortalWord(PT_CTX_PARAM uint portal, uint k);     k < kPtPortalWords
//   float  ptLightMapValue(PT_CTX_PARAM uint index);          emissive ordinal -> light-set index (-1: none)
//   float4 ptTextureSample(PT_CTX_PARAM uint texture, uint smp, float u, float v);  (LOD 0)
//   RlLight ptLoadLight(PT_CTX_PARAM uint light);
//   PtLightPick ptSampleLightSet(PT_CTX_PARAM float3 p, float3 n, float u0, float u1, float u2);
//   float ptLightSetPdf(PT_CTX_PARAM float3 p, float3 n, uint light, float3 wi);
//   uint ptRestirDiVertex(PT_CTX_PARAM PtParams P, uint px, uint py, PtRawHit h, float3 d, PT_INOUT(float3) direct);
//                                        RL-5.2 ReSTIR DI hook (render/pathtrace/restir_di*): with kPtFlagDiRecord,
//                                        called at the G-buffer vertex (hit h, incoming direction d), returns 2 and
//                                        the path stops (the surface pass); with kPtFlagRestirDi, called at every
//                                        primary-chain vertex of sample sampleBase: 1 = `direct` is ReSTIR's estimate
//                                        of this vertex's direct light (NEE is skipped, and light-set emitters seen
//                                        by the continuation from it get MIS weight 0 unless it scattered by a dirac
//                                        lobe), 0 = not ReSTIR's vertex (ordinary NEE).
//   uint ptRestirGiVertex(PT_CTX_PARAM PtParams P, uint px, uint py, PtRawHit h, float3 d, uint bounce,
//                         PT_INOUT(float3) indirect);
//                                        RL-5.3 ReSTIR GI hook (render/pathtrace/restir_gi*): with kPtFlagGiRecord,
//                                        called at the G-buffer vertex (vertex index `bounce`), returns 2 and the path
//                                        stops; with kPtFlagRestirGi, called at every primary-chain vertex of sample
//                                        sampleBase: 1 = `indirect` is ReSTIR GI's estimate of everything the
//                                        continuation from this vertex brings except the light-set emission of its
//                                        first segment: NEE (or ReSTIR DI) runs as usual, the continuation collects
//                                        that first segment's light-set emission (MIS as usual) and the path stops.
//   uint ptRadianceCacheVertex(PT_CTX_PARAM PtParams P, uint px, uint py, uint bounce, uint vflags, PtSurface S,
//                              float3 n, float3 d, float tHit, float prevPdf, float3 thr, float3 acc,
//                              PT_INOUT(float3) cached);
//   void ptRadianceCacheEnd(PT_CTX_PARAM PtParams P, uint px, uint py, float3 acc);
//                                        RL-5.4 radiance cache hooks (render/pathtrace/radiance_cache*, bodies in
//                                        kernels/radiance_cache_path.h), called only with kPtFlagRadianceCache or
//                                        kPtFlagRcTrain: at every scattering vertex before its NEE (vflags: primary
//                                        chain 1, previous scatter dirac 2, ReSTIR-owned 4; thr / acc: the path
//                                        throughput and radiance so far), 1 = the path ends with thr x `cached` (never
//                                        on the primary chain); End once per path after the loop (training records).
//
// THE ESTIMATOR (unidirectional path tracing, one path per sample; ptRenderSample):
//   * primary rays from a pinhole camera (the D3D view / projection's eye and field of view), jittered in the pixel
//     (kPtFlagJitter) with counter-based random numbers: ptRandom(pixel, frame, sample, dimension) is the PCG hash of
//     [Jarzynski and Olano 2020, "Hash Functions for GPU Rendering", JCGT 9(3)], identical in the three dialects;
//   * next-event estimation at every vertex with a non-dirac lobe: one light of the RL-4.4 light set (WP-7.1 light-tree
//     selection x the light's own sample), shadow ray, RL-4.3 bsdfEval; combined with BSDF sampling by multiple
//     importance sampling with the power heuristic [Veach and Guibas 1995, "Optimally Combining Sampling Techniques
//     for Monte Carlo Rendering", SIGGRAPH; Veach 1997, PhD thesis ch. 9]: BSDF-sampled rays that reach an emitter of
//     the light set (an emissive triangle through the scene, an analytic light by ptAnalyticHit) are weighted against
//     the light-set density ptLightSetPdf at the previous vertex (same position and normal as that vertex's NEE);
//     after a dirac scatter (glass, the opacity pass-through, a portal) the weight is 1;
//   * Russian roulette from bounce rrStart on, survival = min(max component of the throughput, 0.95)
//     [Arvo and Kirk 1990, "Particle Transport and Image Synthesis", SIGGRAPH];
//   * the sky (uniform radiance) and distant lights with a non-zero angle are seen by rays that leave the scene.
//   Every light of the set is reachable by NEE except delta ones, which only NEE reaches; emitters outside the set
//   (unlit / sky surfaces, the uniform sky) are reached by BSDF sampling only (weight 1). The estimator is unbiased
//   for the scene up to maxBounces (the same truncation on CPU and GPU).
//
// LEGACY ALPHA (Remix legacy material semantics; FUSE implementation): the surface alpha = texture alpha x (vertex
// COLOR0 alpha when kPtMatVertexColor) x the material's base alpha;
//   alpha test (kPtMatAlphaTest)  a hit whose alpha fails `alpha <op> reference` (VkCompareOp) does not exist: the
//                                 traversal is FORCE_OPAQUE (WP-6.0), so ptTraceScene re-traces from the rejected hit
//                                 (tmin = t x (1 + 1e-5) + rayEps) up to maxAlphaSkips times - shadow rays alike;
//   alpha blend (kPtMatAlphaBlend) the alpha is the BSDF opacity: the RL-4.3 opaque model then passes 1 - alpha
//                                 through as a dirac lobe (stochastic layer resolution, energy exact); an unlit
//                                 blended layer (kPtMatUnlit) adds alpha x its colour and passes 1 - alpha on
//                                 deterministically (the D3D SRCALPHA / INVSRCALPHA result). Shadow rays
//                                 treat blended surfaces as occluders (the light behind them arrives through the
//                                 pass-through lobe with MIS weight 1: unbiased).
//
// PRIMARY SURFACE REPLACEMENT (PSR, kPtFlagPsr): the G-buffer the denoiser sees (normal, depth, motion, albedo) is
// written at the first vertex of the primary path that is not a mirror-like interface: while the primary chain
// crosses a portal, a dirac glass lobe (translucent model without a diffuse layer) or the specular lobe of a mirror
// (opaque, perceptual roughness <= psrMirrorRoughness), up to psrMaxBounces times, the chain's throughput becomes
// part of the G-buffer albedo and the replaced surface is the one after it (mirror images keep their own normals and
// the view depth of the unfolded path; their motion is 0 - virtual motion is RL-5.5's). Radiance is unchanged by PSR (it only moves the
// G-buffer), so PSR on / off converge to the same image.
//
// RAY PORTALS (kBsdfModelPortal with a portal index): a ray entering portal i leaves portal i's partner: origin and
// direction go through the portal's 3x4 world matrix (ptPortalWord), the path continues as after a dirac scatter.
// Shadow rays and the light tree do not look through portals (lights behind a portal arrive through BSDF paths).
//
// DEMODULATION (the WP-6.4 denoiser's inputs; exact: remodulated = radiance): radiance = emissive + diffuse x albedoD
// + specular x albedoS, where `emissive` is what the primary chain collects before the G-buffer vertex (emission, NEE
// at PSR vertices), `diffuse` / `specular` everything after it: NEE at the G-buffer vertex split by the albedo
// luminance ratio, the rest by the lobe sampled there (diffuse / diffuse transmission / hair residual -> diffuse,
// everything else -> specular); albedoD / albedoS = max(chain throughput x the vertex's diffuse / specular albedo,
// kPtMinAlbedo) per channel.
//
// PACKED RECORDS (float4 words; integer fields stored as exact floats below 2^24):
//   params    kPtParamWords   see ptParamsUnpack
//   instance  kPtInstanceWords per GPU-scene instance slot: rows 0..2 of objectToWorld (3x4, column vectors),
//             w3 = (first triangle in the triangle table, flags, light-map base, 0)
//   triangle  kPtTriangleWords per BLAS primitive (MTRI order of the mesh; object space): w0..w2 = (p_i, uv
//             component), w3..w5 = (n_i, uv component) (uv0 = (w0.w, w1.w), uv1 = (w2.w, w3.w), uv2 = (w4.w,
//             w5.w)), w6 = (material, emissive ordinal or -1, 0, 0), w7..w9 = COLOR0 of vertex 0..2 (rgb linear - the scene
//             compiler decodes display-space vertex colours -, a as authored)
//   material  kPtMaterialWords: w0..w10 = the packed BsdfMaterial (bsdfMaterialPack), w11 = (texture handle low /
//             high 16 bits, sampler handle low / high 16 bits; the texture is display space: its rgb is decoded
//             with pow 2.2 before multiplying the base colour), w12 = (flags, alpha reference [0, 1], alpha
//             VkCompareOp, portal index or -1)
//   portal    kPtPortalWords per portal: rows 0..2 of the world -> world teleport matrix (3x4)

// ---- small helpers ----------------------------------------------------------------------------------------------

PT_FN float ptMax3(float3 c) { return max(c.x, max(c.y, c.z)); }
PT_FN float ptLum(float3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
PT_FN float3 ptSafeNormalize(float3 v, float3 fallback) {
    float l2 = dot(v, v);
    if (!(l2 > 1e-30f)) {
        return fallback;
    }
    return v * (1.0f / sqrt(l2));
}
PT_FN float3 ptMax3v(float3 a, float b) { return float3(max(a.x, b), max(a.y, b), max(a.z, b)); }
PT_FN float3 ptDiv3(float3 a, float3 b) { return float3(a.x / b.x, a.y / b.y, a.z / b.z); }

/// PCG hash [Jarzynski and Olano 2020].
PT_FN uint ptHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
/// Per-sample seed of pixel (x, y), frame seed and sample index.
PT_FN uint ptSeed(uint x, uint y, uint frameSeed, uint sampleIndex) {
    return ptHash(x ^ ptHash(y ^ ptHash(frameSeed ^ ptHash(sampleIndex))));
}
/// Uniform in [0, 1) for dimension `dim` of a sample.
PT_FN float ptRandom(uint seed, uint dim) {
    return float(ptHash(seed ^ ptHash(dim + 0x9E3779B9u)) >> 8u) * (1.0f / 16777216.0f);
}

/// Power heuristic (beta = 2) of `a` against `b`.
PT_FN float ptPowerHeuristic(float a, float b) {
    float a2 = a * a;
    float b2 = b * b;
    if (!(a2 + b2 > 0.0f)) {
        return 0.0f;
    }
    return a2 / (a2 + b2);
}

PT_FN float ptMisWeight(uint flags, float pdfThis, float pdfOther) {
    if ((flags & kPtFlagMis) != 0u) {
        return ptPowerHeuristic(pdfThis, pdfOther);
    }
    return pdfOther > 0.0f ? 0.5f : 1.0f;
}

/// Offset along the geometric normal, to the side of `dir` (scale-aware epsilon).
PT_FN float3 ptOffset(float3 p, float3 ng, float3 dir, float eps) {
    float s = dot(dir, ng) >= 0.0f ? 1.0f : -1.0f;
    float scale = eps * (1.0f + max(abs(p.x), max(abs(p.y), abs(p.z))));
    return p + ng * (s * scale);
}

/// Local frame around n (Duff et al. 2017, as light_core.h rlBasis).
PT_FN void ptBasis(float3 w, PT_OUT(float3) t, PT_OUT(float3) b) {
    float s = w.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (s + w.z);
    float bb = w.x * w.y * a;
    t = float3(1.0f + s * w.x * w.x * a, s * bb, -s * w.x);
    b = float3(bb, s + w.y * w.y * a, -w.y);
}

PT_FN bool ptCompare(uint op, float a, float ref) {
    // VkCompareOp: NEVER, LESS, EQUAL, LESS_OR_EQUAL, GREATER, NOT_EQUAL, GREATER_OR_EQUAL, ALWAYS
    if (op == 0u) {
        return false;
    }
    if (op == 1u) {
        return a < ref;
    }
    if (op == 2u) {
        return abs(a - ref) <= (0.5f / 255.0f);
    }
    if (op == 3u) {
        return a <= ref + (0.5f / 255.0f);
    }
    if (op == 4u) {
        return a > ref + (0.5f / 255.0f);
    }
    if (op == 5u) {
        return abs(a - ref) > (0.5f / 255.0f);
    }
    if (op == 6u) {
        return a >= ref - (0.5f / 255.0f);
    }
    return true;
}

// ---- unpacking ----------------------------------------------------------------------------------------------------

PT_FN PtParams ptParamsUnpack(PT_PARAM_WORDS(w)) {
    PtParams P;
    P.camOrigin = float3(w[0].x, w[0].y, w[0].z);
    P.width = uint(w[0].w);
    P.camRight = float3(w[1].x, w[1].y, w[1].z);
    P.height = uint(w[1].w);
    P.camUp = float3(w[2].x, w[2].y, w[2].z);
    P.frameSeed = uint(w[2].w);
    P.camForward = float3(w[3].x, w[3].y, w[3].z);
    P.maxBounces = uint(w[3].w);
    P.prevOrigin = float3(w[4].x, w[4].y, w[4].z);
    P.rrStart = uint(w[4].w);
    P.prevRight = float3(w[5].x, w[5].y, w[5].z);
    P.flags = uint(w[5].w);
    P.prevUp = float3(w[6].x, w[6].y, w[6].z);
    P.lightCount = uint(w[6].w);
    P.prevForward = float3(w[7].x, w[7].y, w[7].z);
    P.analyticCount = uint(w[7].w);
    P.sky = float3(w[8].x, w[8].y, w[8].z);
    P.psrMaxBounces = uint(w[8].w);
    P.rayEps = w[9].x;
    P.psrMirrorRoughness = w[9].y;
    P.sampleBase = uint(w[9].z);
    P.samplesPerPixel = uint(w[9].w);
    P.maxAlphaSkips = uint(w[10].x);
    P.portalCount = uint(w[10].y);
    P.rcTableLo = uint(w[10].z);
    P.rcTableHi = uint(w[10].w);
    return P;
}

PT_FN uint ptHandle(float lo, float hi) { return uint(lo) | (uint(hi) << 16u); }

PT_FN BsdfMaterial ptLoadBsdf(PT_CTX_PARAM uint material) {
    float4 w[11];
    for (uint k = 0u; k < 11u; ++k) {
        w[k] = ptMaterialWord(PT_CTX_ARG material, k);
    }
    return bsdfMaterialUnpack(w);
}

/// Object -> world of a point / vector through the instance rows.
PT_FN float3 ptXformPoint(float4 r0, float4 r1, float4 r2, float3 p) {
    return float3(r0.x * p.x + r0.y * p.y + r0.z * p.z + r0.w, r1.x * p.x + r1.y * p.y + r1.z * p.z + r1.w,
                  r2.x * p.x + r2.y * p.y + r2.z * p.z + r2.w);
}
PT_FN float3 ptXformVector(float4 r0, float4 r1, float4 r2, float3 v) {
    return float3(r0.x * v.x + r0.y * v.y + r0.z * v.z, r1.x * v.x + r1.y * v.y + r1.z * v.z,
                  r2.x * v.x + r2.y * v.y + r2.z * v.z);
}
/// Normal transform: cofactor matrix of the upper 3x3 (the inverse transpose up to a positive scale when det > 0;
/// the sign of det flips it back for mirrored instances).
PT_FN float3 ptXformNormal(float4 r0, float4 r1, float4 r2, float3 n) {
    float3 a = float3(r0.x, r0.y, r0.z);
    float3 b = float3(r1.x, r1.y, r1.z);
    float3 c = float3(r2.x, r2.y, r2.z);
    // columns of the cofactor matrix: rows of M are a, b, c; cof(M) rows = b x c, c x a, a x b.
    float3 k0 = cross(b, c);
    float3 k1 = cross(c, a);
    float3 k2 = cross(a, b);
    float det = dot(a, k0);
    float3 r = float3(k0.x * n.x + k1.x * n.y + k2.x * n.z, k0.y * n.x + k1.y * n.y + k2.y * n.z,
                      k0.z * n.x + k1.z * n.y + k2.z * n.z);
    return det < 0.0f ? -r : r;
}

// ---- surfaces -----------------------------------------------------------------------------------------------------

/// The surface at a raw hit: world position / normals, the material with the legacy texture / vertex colour / alpha
/// applied, emission and the emissive light index.
PT_FN PtSurface ptLoadSurface(PT_CTX_PARAM PtRawHit h) {
    PtSurface S;
    float4 r0 = ptInstanceWord(PT_CTX_ARG h.instance, 0u);
    float4 r1 = ptInstanceWord(PT_CTX_ARG h.instance, 1u);
    float4 r2 = ptInstanceWord(PT_CTX_ARG h.instance, 2u);
    float4 r3 = ptInstanceWord(PT_CTX_ARG h.instance, 3u);
    uint tri = uint(r3.x) + h.primitive;
    float4 w0 = ptTriangleWord(PT_CTX_ARG tri, 0u);
    float4 w1 = ptTriangleWord(PT_CTX_ARG tri, 1u);
    float4 w2 = ptTriangleWord(PT_CTX_ARG tri, 2u);
    float4 w3 = ptTriangleWord(PT_CTX_ARG tri, 3u);
    float4 w4 = ptTriangleWord(PT_CTX_ARG tri, 4u);
    float4 w5 = ptTriangleWord(PT_CTX_ARG tri, 5u);
    float4 w6 = ptTriangleWord(PT_CTX_ARG tri, 6u);
    float4 c0 = ptTriangleWord(PT_CTX_ARG tri, 7u);
    float4 c1 = ptTriangleWord(PT_CTX_ARG tri, 8u);
    float4 c2 = ptTriangleWord(PT_CTX_ARG tri, 9u);
    float b1 = h.u;
    float b2 = h.v;
    float b0 = 1.0f - b1 - b2;
    float3 p0 = float3(w0.x, w0.y, w0.z);
    float3 p1 = float3(w1.x, w1.y, w1.z);
    float3 p2 = float3(w2.x, w2.y, w2.z);
    float3 pObj = p0 * b0 + p1 * b1 + p2 * b2;
    S.position = ptXformPoint(r0, r1, r2, pObj);
    float3 ngObj = cross(p1 - p0, p2 - p0);
    S.geoNormal = ptSafeNormalize(ptXformNormal(r0, r1, r2, ngObj), float3(0.0f, 0.0f, 1.0f));
    float3 nObj = float3(w3.x, w3.y, w3.z) * b0 + float3(w4.x, w4.y, w4.z) * b1 + float3(w5.x, w5.y, w5.z) * b2;
    float3 ns = ptXformNormal(r0, r1, r2, nObj);
    S.normal = dot(ns, ns) > 1e-20f ? ptSafeNormalize(ns, S.geoNormal) : S.geoNormal;
    // The shading normal stays in the geometric normal's hemisphere (flipped vertex normals, degenerate data).
    if (dot(S.normal, S.geoNormal) < 0.0f) {
        S.normal = -S.normal;
    }
    float tu = w0.w * b0 + w2.w * b1 + w4.w * b2;
    float tv = w1.w * b0 + w3.w * b1 + w5.w * b2;
    float4 vc = float4(c0.x * b0 + c1.x * b1 + c2.x * b2, c0.y * b0 + c1.y * b1 + c2.y * b2,
                       c0.z * b0 + c1.z * b1 + c2.z * b2, c0.w * b0 + c1.w * b1 + c2.w * b2);
    S.material = uint(w6.x);
    S.m = ptLoadBsdf(PT_CTX_ARG S.material);
    float4 x11 = ptMaterialWord(PT_CTX_ARG S.material, 11u);
    float4 x12 = ptMaterialWord(PT_CTX_ARG S.material, 12u);
    S.flags = uint(x12.x);
    float alpha = S.m.opacity;
    float3 base = S.m.albedo;
    if ((S.flags & kPtMatTextured) != 0u) {
        float4 t = ptTextureSample(PT_CTX_ARG ptHandle(x11.x, x11.y), ptHandle(x11.z, x11.w), tu, tv);
        // Legacy textures hold display-space colour (upstream gammaToLinear: pow 2.2).
        base = base * float3(pow(max(t.x, 0.0f), 2.2f), pow(max(t.y, 0.0f), 2.2f), pow(max(t.z, 0.0f), 2.2f));
        alpha = alpha * t.w;
    }
    if ((S.flags & kPtMatVertexColor) != 0u) {
        base = base * float3(vc.x, vc.y, vc.z);
        alpha = alpha * vc.w;
    }
    S.m.albedo = base;
    S.alpha = alpha;
    S.m.opacity = (S.flags & kPtMatAlphaBlend) != 0u ? alpha : 1.0f;
    S.emission = S.m.emission;
    if ((S.flags & kPtMatUnlit) != 0u) {
        S.emission = base;
    }
    S.light = kPtInvalid;
    if (w6.y >= 0.0f && r3.z >= 0.0f) {
        float l = ptLightMapValue(PT_CTX_ARG uint(r3.z) + uint(w6.y));
        if (l >= 0.0f) {
            S.light = uint(l);
        }
    }
    S.portal = kPtInvalid;
    if (S.m.model == kBsdfModelPortal && x12.w >= 0.0f) {
        S.portal = uint(x12.w);
    }
    return S;
}

/// Alpha test of the hit (true: the hit exists).
PT_FN bool ptAlphaAccept(PT_CTX_PARAM PtRawHit h) {
    float4 r3 = ptInstanceWord(PT_CTX_ARG h.instance, 3u);
    uint tri = uint(r3.x) + h.primitive;
    float4 w6 = ptTriangleWord(PT_CTX_ARG tri, 6u);
    uint material = uint(w6.x);
    float4 x12 = ptMaterialWord(PT_CTX_ARG material, 12u);
    uint flags = uint(x12.x);
    if ((flags & kPtMatAlphaTest) == 0u) {
        return true;
    }
    PtSurface S = ptLoadSurface(PT_CTX_ARG h);
    return ptCompare(uint(x12.z), S.alpha, x12.y);
}

/// Closest hit that passes the alpha test (see LEGACY ALPHA).
PT_FN PtRawHit ptTraceScene(PT_CTX_PARAM PtParams P, float3 o, float3 d, float tmin, float tmax, uint mask) {
    PtRawHit h = ptRawTrace(PT_CTX_ARG o, d, tmin, tmax, mask);
    uint skips = 0u;
    while (h.hit && skips < P.maxAlphaSkips && !ptAlphaAccept(PT_CTX_ARG h)) {
        float next = h.t * (1.0f + 1e-5f) + P.rayEps;
        ++skips;
        h = ptRawTrace(PT_CTX_ARG o, d, next, tmax, mask);
    }
    if (h.hit && skips >= P.maxAlphaSkips && !ptAlphaAccept(PT_CTX_ARG h)) {
        h.hit = false;
    }
    return h;
}

// ---- analytic lights seen by BSDF rays ------------------------------------------------------------------------------

/// Distance along the unit ray (o, d) to analytic light L where rlLightEval sees it (kPtFar: not hit / distant).
PT_FN float ptAnalyticDistance(RlLight L, float3 o, float3 d) {
    if (L.kind == kRlKindSphere) {
        float3 oc = o - PT_B3(L.position);
        float b = dot(oc, d);
        float c = dot(oc, oc) - L.radius * L.radius;
        float disc = b * b - c;
        if (!(disc >= 0.0f)) {
            return kPtFar;
        }
        float s = sqrt(disc);
        float t0 = -b - s;
        if (t0 > 0.0f) {
            return t0;
        }
        float t1 = -b + s;
        return t1 > 0.0f ? t1 : kPtFar;
    }
    if (L.kind == kRlKindRect || L.kind == kRlKindDisk || L.kind == kRlKindTriangle) {
        float t = rlPlanarHit(L, PT_L3(o), PT_L3(d));
        return t > 0.0f ? t : kPtFar;
    }
    if (L.kind == kRlKindCylinder) {
        RlCylinderHits ch = rlCylinderHits(L, PT_L3(o), PT_L3(d));
        return ch.tNear > 0.0f ? ch.tNear : kPtFar;
    }
    return kPtFar;
}

/// Emission of the analytic lights along a BSDF-sampled ray up to tHit (MIS against the light set from the previous
/// vertex (prevP, prevN) unless `prevDelta`). Distant lights only when the ray left the scene.
PT_FN float3 ptAnalyticEmission(PT_CTX_PARAM PtParams P, float3 o, float3 d, float tHit, bool escaped, bool prevDelta,
                                float prevPdf, float3 prevP, float3 prevN) {
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    for (uint i = 0u; i < P.analyticCount; ++i) {
        RlLight L = ptLoadLight(PT_CTX_ARG i);
        bool distant = L.kind == kRlKindDistant;
        if (distant ? !escaped : false) {
            continue;
        }
        if (!distant && ptAnalyticDistance(L, o, d) >= tHit) {
            continue;
        }
        float3 le = PT_B3(rlLightEval(L, PT_L3(o), PT_L3(d)));
        if (!(ptMax3(le) > 0.0f)) {
            continue;
        }
        float w = 1.0f;
        if (!prevDelta && (P.flags & kPtFlagNee) != 0u) {
            w = ptMisWeight(P.flags, prevPdf, ptLightSetPdf(PT_CTX_ARG prevP, prevN, i, d));
        }
        sum = sum + le * w;
    }
    return sum;
}

/// Shadow ray: true when nothing (that passes the alpha test) lies in (tmin, tmax).
PT_FN bool ptVisible(PT_CTX_PARAM PtParams P, float3 o, float3 d, float tmax) {
    PtRawHit h = ptTraceScene(PT_CTX_ARG P, o, d, 0.0f, tmax, kPtMaskShadow);
    return !h.hit;
}

// ---- material classification --------------------------------------------------------------------------------------

/// The light-tree normal of a vertex: 0 (no culling) when light can arrive from both sides.
PT_FN float3 ptTreeNormal(PtSurface S, float3 nFacing) {
    if (S.m.model != kBsdfModelOpaque || (S.m.flags & kBsdfFlagSssThin) != 0u) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    return nFacing;
}

PT_FN void ptAlbedos(PtSurface S, PT_OUT(float3) aD, PT_OUT(float3) aS) {
    if (S.m.model == kBsdfModelOpaque) {
        float o = S.m.opacity;
        aD = S.m.albedo * ((1.0f - S.m.metallic) * o);
        float3 f0 = float3(kBsdfDielectricF0, kBsdfDielectricF0, kBsdfDielectricF0);
        aS = (f0 + (S.m.albedo - f0) * S.m.metallic) * o + float3(1.0f - o, 1.0f - o, 1.0f - o);
    } else if (S.m.model == kBsdfModelTranslucent) {
        float l = (S.m.flags & kBsdfFlagDiffuseLayer) != 0u ? S.m.layerOpacity : 0.0f;
        aD = S.m.layerColor * l;
        aS = S.m.transmittance * (1.0f - l);
    } else {
        aD = float3(0.0f, 0.0f, 0.0f);
        aS = float3(1.0f, 1.0f, 1.0f);
    }
}

PT_FN bool ptIsDiffuseLobe(uint sampleFlags) {
    uint lobe = sampleFlags >> kBsdfLobeShift;
    return lobe == kBsdfLobeDiffuse || lobe == kBsdfLobeDiffuseTransmission || lobe == kBsdfLobeHair0 + 3u;
}

// ---- the path --------------------------------------------------------------------------------------------------------

PT_FN float3 ptPrimaryDirection(PtParams P, float fx, float fy) {
    float x = (fx / float(P.width)) * 2.0f - 1.0f;
    float y = 1.0f - (fy / float(P.height)) * 2.0f;
    return normalize(P.camForward + P.camRight * x + P.camUp * y);
}

/// UV (x right, y down, [0, 1]) of world point `p` in the camera (origin, right, up, forward); false behind it.
PT_FN bool ptProject(float3 origin, float3 right, float3 up, float3 forward, float3 p, PT_OUT(float) ux,
                     PT_OUT(float) uy) {
    float3 v = p - origin;
    float z = dot(v, forward);
    ux = 0.0f;
    uy = 0.0f;
    if (!(z > 1e-6f)) {
        return false;
    }
    float x = dot(v, right) / (z * dot(right, right));
    float y = dot(v, up) / (z * dot(up, up));
    ux = x * 0.5f + 0.5f;
    uy = 0.5f - y * 0.5f;
    return true;
}

/// One path through pixel (px, py), sample index `sampleIndex` (see THE ESTIMATOR).
PT_FN PtSample ptRenderSample(PT_CTX_PARAM PtParams P, uint px, uint py, uint sampleIndex) {
    PtSample R;
    R.radiance = float3(0.0f, 0.0f, 0.0f);
    R.emissive = float3(0.0f, 0.0f, 0.0f);
    R.diffuse = float3(0.0f, 0.0f, 0.0f);
    R.specular = float3(0.0f, 0.0f, 0.0f);
    R.albedoD = float3(1.0f, 1.0f, 1.0f);
    R.albedoS = float3(1.0f, 1.0f, 1.0f);
    R.normal = float3(0.0f, 0.0f, 0.0f);
    R.roughness = 1.0f;
    R.depth = 0.0f;
    R.hitDist = 0.0f;
    R.motionX = 0.0f;
    R.motionY = 0.0f;
    R.instance = kPtInvalid;
    R.psr = 0u;
    R.flags = 0u;

    uint seed = ptSeed(px, py, P.frameSeed, sampleIndex);
    float jx = 0.5f;
    float jy = 0.5f;
    if ((P.flags & kPtFlagJitter) != 0u) {
        jx = ptRandom(seed, 0u);
        jy = ptRandom(seed, 1u);
    }
    float3 o = P.camOrigin;
    float3 d = ptPrimaryDirection(P, float(px) + jx, float(py) + jy);
    float depthScale = dot(d, P.camForward);
    float3 thr = float3(1.0f, 1.0f, 1.0f);
    // Channel of what the path collects: 0 = emissive (primary chain), 1 = diffuse, 2 = specular.
    uint channel = 0u;
    bool chain = true;      // still on the primary chain (G-buffer not written)
    bool prevDelta = true;  // the camera is a dirac "scatter"
    float prevPdf = 0.0f;
    float3 prevP = o;
    float3 prevN = float3(0.0f, 0.0f, 0.0f);
    float pathLength = 0.0f; // unfolded distance along the primary chain
    float3 gPos = o;
    bool needHitDist = false;
    float3 chainThr = float3(1.0f, 1.0f, 1.0f);
    bool nee = (P.flags & kPtFlagNee) != 0u;
    bool bsdfLights = (P.flags & kPtFlagBsdfLights) != 0u;
    bool diReplaced = false; // RL-5.2: this vertex's direct light is ReSTIR DI's (ptRestirDiVertex)
    bool prevRestir = false; // ... the previous vertex's
    bool giReplaced = false; // RL-5.3: this vertex's indirect light is ReSTIR GI's (ptRestirGiVertex)
    bool giCut = false;      // ... the previous vertex's: this segment collects light-set emission only, then stops
    float3 giIndirect = float3(0.0f, 0.0f, 0.0f);

    for (uint bounce = 0u; bounce < P.maxBounces + 1u; ++bounce) {
        uint dim = 2u + bounce * kPtDimsPerBounce;
        PtRawHit h = ptTraceScene(PT_CTX_ARG P, o, d, 0.0f, kPtFar, kPtMaskVisible);
        float tHit = h.hit ? h.t : kPtFar;
        if (needHitDist) {
            R.hitDist = h.hit ? h.t : 0.0f;
            needHitDist = false;
        }
        // Emitters along the ray: analytic lights (and distant ones / the sky when it escapes).
        float3 le = float3(0.0f, 0.0f, 0.0f);
        if (prevDelta || (bsdfLights && !prevRestir)) {
            le = ptAnalyticEmission(PT_CTX_ARG P, o, d, tHit, !h.hit, prevDelta, prevPdf, prevP, prevN);
        }
        if (!h.hit && !giCut) {
            le = le + P.sky;
        }
        float3 contrib = thr * le;
        if (!h.hit) {
            if (channel == 0u) {
                R.emissive = R.emissive + contrib;
            } else if (channel == 1u) {
                R.diffuse = R.diffuse + contrib;
            } else {
                R.specular = R.specular + contrib;
            }
            R.radiance = R.radiance + contrib;
            break;
        }
        PtSurface S = ptLoadSurface(PT_CTX_ARG h);
        if (chain) {
            pathLength = pathLength + h.t;
        }
        // Surface emission (emissive triangles: MIS against the light set; unlit / non-light emitters: weight 1).
        if (ptMax3(S.emission) > 0.0f && (!giCut || S.light != kPtInvalid)) {
            float w = 1.0f;
            if (S.light != kPtInvalid) {
                if (!prevDelta) {
                    if (!bsdfLights || prevRestir) {
                        w = 0.0f;
                    } else if (nee) {
                        w = ptMisWeight(P.flags, prevPdf, ptLightSetPdf(PT_CTX_ARG prevP, prevN, S.light, d));
                    }
                }
            }
            // Blended unlit layers emit alpha x colour; the rest passes through (below).
            float coverage = (S.flags & (kPtMatUnlit | kPtMatAlphaBlend)) == (kPtMatUnlit | kPtMatAlphaBlend)
                                 ? S.alpha
                                 : 1.0f;
            contrib = contrib + thr * S.emission * (w * coverage);
        }
        if (channel == 0u) {
            R.emissive = R.emissive + contrib;
        } else if (channel == 1u) {
            R.diffuse = R.diffuse + contrib;
        } else {
            R.specular = R.specular + contrib;
        }
        R.radiance = R.radiance + contrib;
        if (giCut) {
            break; // RL-5.3: the rest of this path is ReSTIR GI's
        }
        // A blended unlit layer: deterministic pass-through of 1 - alpha (no scattering; counts as a vertex).
        if ((S.flags & (kPtMatUnlit | kPtMatAlphaBlend)) == (kPtMatUnlit | kPtMatAlphaBlend) && S.alpha < 1.0f &&
            bounce < P.maxBounces) {
            thr = thr * (1.0f - max(S.alpha, 0.0f));
            o = ptOffset(S.position, S.geoNormal, d, P.rayEps);
            continue;
        }
        if ((S.flags & kPtMatUnlit) != 0u || bounce == P.maxBounces) {
            if (chain) {
                // The G-buffer is the unlit surface / the last vertex.
                R.normal = dot(S.geoNormal, d) > 0.0f ? -S.geoNormal : S.geoNormal;
                R.depth = pathLength * depthScale;
                R.instance = h.instance;
                R.flags = R.flags | kPtSampleHit;
                chain = false;
            }
            break;
        }
        // Ray portals.
        if (S.portal != kPtInvalid) {
            if (S.portal < P.portalCount) {
                float4 m0 = ptPortalWord(PT_CTX_ARG S.portal, 0u);
                float4 m1 = ptPortalWord(PT_CTX_ARG S.portal, 1u);
                float4 m2 = ptPortalWord(PT_CTX_ARG S.portal, 2u);
                float3 exitP = ptXformPoint(m0, m1, m2, S.position);
                float3 exitD = ptSafeNormalize(ptXformVector(m0, m1, m2, d), d);
                float3 exitN = ptSafeNormalize(ptXformVector(m0, m1, m2, S.geoNormal), S.geoNormal);
                o = ptOffset(exitP, exitN, exitD, P.rayEps);
                d = exitD;
                prevDelta = true;
                if (chain) {
                    R.psr = R.psr + 1u;
                }
                continue;
            }
            break;
        }
        // Shading frame: opaque surfaces are two-sided (the frame faces the incoming ray); translucent / hair keep
        // the authored side (inside / outside).
        float3 n = S.normal;
        float3 ng = S.geoNormal;
        if (S.m.model == kBsdfModelOpaque && dot(ng, d) > 0.0f) {
            n = -n;
            ng = -ng;
        }
        float3 tx;
        float3 ty;
        ptBasis(n, tx, ty);
        float3 woW = -d;
        float3 wo = float3(dot(woW, tx), dot(woW, ty), dot(woW, n));
        float3 treeN = ptTreeNormal(S, n);

        // Next-event estimation (or ReSTIR DI's estimate at the G-buffer vertex, RL-5.2).
        float3 direct = float3(0.0f, 0.0f, 0.0f);
        diReplaced = false;
        if (chain && (P.flags & kPtFlagRestirDi) != 0u && sampleIndex == P.sampleBase) {
            diReplaced = ptRestirDiVertex(PT_CTX_ARG P, px, py, h, d, direct) == 1u;
        }
        giReplaced = false;
        if (chain && (P.flags & kPtFlagRestirGi) != 0u && sampleIndex == P.sampleBase) {
            giReplaced = ptRestirGiVertex(PT_CTX_ARG P, px, py, h, d, bounce, giIndirect) == 1u;
        }
        if (!giReplaced) {
            giIndirect = float3(0.0f, 0.0f, 0.0f);
        }
        // RL-5.4 radiance cache: record the vertex (training) or end the path with the cached radiance.
        if ((P.flags & (kPtFlagRadianceCache | kPtFlagRcTrain)) != 0u) {
            float3 cached = float3(0.0f, 0.0f, 0.0f);
            uint vflags = (chain ? 1u : 0u) | (prevDelta ? 2u : 0u) | (diReplaced || giReplaced ? 4u : 0u);
            if (ptRadianceCacheVertex(PT_CTX_ARG P, px, py, bounce, vflags, S, n, d, h.t, prevPdf, thr, R.radiance,
                                      cached) == 1u) {
                float3 ccon = thr * cached;
                if (channel == 1u) {
                    R.diffuse = R.diffuse + ccon;
                } else if (channel == 2u) {
                    R.specular = R.specular + ccon;
                } else {
                    R.emissive = R.emissive + ccon;
                }
                R.radiance = R.radiance + ccon;
                break;
            }
        }
        if (nee && !diReplaced && P.lightCount > 0u) {
            PtLightPick lp = ptSampleLightSet(PT_CTX_ARG S.position, treeN, ptRandom(seed, dim + 0u),
                                              ptRandom(seed, dim + 1u), ptRandom(seed, dim + 2u));
            if (lp.light != kPtInvalid && lp.pdf > 0.0f && ptMax3(lp.radiance) > 0.0f) {
                float3 wiL = float3(dot(lp.wi, tx), dot(lp.wi, ty), dot(lp.wi, n));
                float3 f = bsdfEval(PT_LUT_ARG S.m, wo, wiL);
                bool geomOk = true;
                if (S.m.model == kBsdfModelOpaque && (S.m.flags & kBsdfFlagSssThin) == 0u) {
                    geomOk = dot(lp.wi, ng) > 0.0f && wiL.z > 0.0f;
                }
                if (geomOk && ptMax3(f) > 0.0f) {
                    float3 so = ptOffset(S.position, ng, lp.wi, P.rayEps);
                    // The shadow ray aims from the offset origin at the sampled point (at grazing angles the offset
                    // moves the ray's crossing of the emitter's plane by offset / cos: aiming along wi would hit the
                    // emitter itself before its own sample).
                    float3 sd = lp.wi;
                    float tmax = kPtFar;
                    if (lp.dist < kPtFar * 0.5f) {
                        float3 toL = S.position + lp.wi * lp.dist - so;
                        float len = sqrt(dot(toL, toL));
                        sd = len > 0.0f ? toL * (1.0f / len) : lp.wi;
                        tmax = len * (1.0f - 1e-4f) - P.rayEps;
                    }
                    if (tmax > 0.0f && ptVisible(PT_CTX_ARG P, so, sd, tmax)) {
                        float w = 1.0f;
                        if (!lp.delta && bsdfLights) {
                            w = ptMisWeight(P.flags, lp.pdf, bsdfPdf(PT_LUT_ARG S.m, wo, wiL));
                        }
                        direct = f * lp.radiance * (w / lp.pdf);
                    }
                }
            }
        }

        // BSDF sample (the continuation).
        float4 u4 = float4(ptRandom(seed, dim + 3u), ptRandom(seed, dim + 4u), ptRandom(seed, dim + 5u),
                           ptRandom(seed, dim + 6u));
        BsdfSample bs = bsdfSample(PT_LUT_ARG S.m, wo, u4);
        bool valid = (bs.flags & kBsdfSampleValid) != 0u;
        bool delta = (bs.flags & kBsdfSampleDelta) != 0u;
        uint lobe = bs.flags >> kBsdfLobeShift;

        // PSR: does the primary chain continue through this vertex?
        bool psrHere = false;
        if (chain && (P.flags & kPtFlagPsr) != 0u && R.psr < P.psrMaxBounces && valid) {
            bool glass = S.m.model == kBsdfModelTranslucent && (S.m.flags & kBsdfFlagDiffuseLayer) == 0u && delta;
            bool mirror = S.m.model == kBsdfModelOpaque && S.m.roughness <= P.psrMirrorRoughness &&
                          lobe == kBsdfLobeSpecular;
            psrHere = glass || mirror;
        }
        if (chain && !psrHere) {
            // This vertex is the G-buffer vertex.
            if ((P.flags & kPtFlagDiRecord) != 0u && ptRestirDiVertex(PT_CTX_ARG P, px, py, h, d, direct) == 2u) {
                break;
            }
            if ((P.flags & kPtFlagGiRecord) != 0u &&
                ptRestirGiVertex(PT_CTX_ARG P, px, py, h, d, bounce, giIndirect) == 2u) {
                break;
            }
            float3 aD;
            float3 aS;
            ptAlbedos(S, aD, aS);
            R.albedoD = ptMax3v(chainThr * aD, kPtMinAlbedo);
            R.albedoS = ptMax3v(chainThr * aS, kPtMinAlbedo);
            R.normal = n;
            R.roughness = S.m.roughness;
            R.depth = pathLength * depthScale;
            R.instance = h.instance;
            R.flags = R.flags | kPtSampleHit;
            gPos = S.position;
            float lumD = ptLum(aD);
            float lumS = ptLum(aS);
            float fs = lumD + lumS > 0.0f ? lumS / (lumD + lumS) : 0.0f;
            float3 dcon = thr * (direct + giIndirect);
            R.diffuse = R.diffuse + dcon * (1.0f - fs);
            R.specular = R.specular + dcon * fs;
            R.radiance = R.radiance + dcon;
            chain = false;
            channel = valid && ptIsDiffuseLobe(bs.flags) ? 1u : 2u;
            if (channel == 2u) {
                R.flags = R.flags | kPtSampleSpecularHit;
            }
            needHitDist = true;
        } else {
            float3 dcon = thr * (direct + giIndirect);
            if (channel == 0u) {
                R.emissive = R.emissive + dcon;
            } else if (channel == 1u) {
                R.diffuse = R.diffuse + dcon;
            } else {
                R.specular = R.specular + dcon;
            }
            R.radiance = R.radiance + dcon;
        }
        if (!valid || !(ptMax3(bs.weight) > 0.0f)) {
            break;
        }
        float3 wiW = tx * bs.wi.x + ty * bs.wi.y + n * bs.wi.z;
        // Reflection lobes must leave on the viewer's side of the geometric surface (shading-normal leaks).
        if (S.m.model == kBsdfModelOpaque && (S.m.flags & kBsdfFlagSssThin) == 0u && lobe != kBsdfLobeOpacity &&
            dot(wiW, ng) <= 0.0f) {
            break;
        }
        thr = thr * bs.weight;
        if (psrHere) {
            chainThr = chainThr * bs.weight;
            R.psr = R.psr + 1u;
        }
        prevDelta = delta;
        prevRestir = diReplaced;
        giCut = giReplaced;
        prevPdf = bs.pdf;
        prevP = S.position;
        prevN = treeN;
        o = ptOffset(S.position, ng, wiW, P.rayEps);
        d = wiW;
        if ((P.flags & kPtFlagRussianRoulette) != 0u && bounce + 1u >= P.rrStart) {
            float q = min(ptMax3(thr), 0.95f);
            if (!(ptRandom(seed, dim + 7u) < q)) {
                break;
            }
            thr = thr * (1.0f / q);
        }
    }
    if ((P.flags & (kPtFlagRadianceCache | kPtFlagRcTrain)) != 0u) {
        ptRadianceCacheEnd(PT_CTX_ARG P, px, py, R.radiance); // RL-5.4 (training records; resets the hook's state)
    }
    // Demodulation (exact: remodulated = radiance) and motion of the G-buffer point.
    R.diffuse = ptDiv3(R.diffuse, R.albedoD);
    R.specular = ptDiv3(R.specular, R.albedoS);
    if ((R.flags & kPtSampleHit) != 0u) {
        float cx = 0.0f;
        float cy = 0.0f;
        float qx = 0.0f;
        float qy = 0.0f;
        if (ptProject(P.camOrigin, P.camRight, P.camUp, P.camForward, gPos, cx, cy) &&
            ptProject(P.prevOrigin, P.prevRight, P.prevUp, P.prevForward, gPos, qx, qy) && R.psr == 0u) {
            R.motionX = qx - cx;
            R.motionY = qy - cy;
        }
    }
    return R;
}
