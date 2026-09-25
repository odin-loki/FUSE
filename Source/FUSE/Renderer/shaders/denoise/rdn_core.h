// RL-5.5 radiance denoiser (the §5.5 in-tree default, extending WP-6.4): the single-source core of every pass.
// Included by the C++ dialect (include/fuse/renderer/denoise/rdn_kernel.hpp: the CPU reference), the Slang kernel
// (rdn.slang, primary) and the GLSL twin (rdn.comp); the dialect defines before including this file:
//   FUSE_RDN_FN        function prefix (C++ `inline`)
//   FUSE_RDN_IN(T)     read-only parameter (C++ `const T&`)
//   FUSE_RDN_INOUT(T)  in-out parameter (C++ `T&`, GLSL / Slang `inout T`)
//   float2 / float3 / float4 / uint / uint64_t with constructors, + - * / (scalar and component-wise), dot,
//   normalize, abs, sqrt, exp, floor, min, max
//   rdnLd4 / rdnSt4 / rdnLd2 / rdnLd1 / rdnLdU (uint64_t address, element index): f32x4 / f32x2 / f32 / u32 loads and
//   the f32x4 store. The GPU passes buffer device addresses; the CPU reference passes host pointers in the same
//   fields, so one RdnFrame record drives both.
//
// Algorithm (self-written from the public literature; no NRD code or headers):
//   [SVGF]   Schied et al., "Spatiotemporal Variance-Guided Filtering", HPG 2017 (temporal accumulation with
//            moments, variance estimate, variance-guided a-trous).
//   [A-SVGF] Schied, Peters, Dachsbacher, "Gradient Estimation for Real-Time Adaptive Temporal Filtering", HPG 2018
//            (temporal gradients from re-shaded previous samples drive the history length / alpha).
//   [TRMV]   Zeng, Liu, Yan, "Temporally Reliable Motion Vectors for Real-time Ray Tracing", EG 2021 (glossy
//            reflections reproject through the virtual image of the reflected point: hit-distance motion).
//   [RTG2]   Zhdan, "ReBLUR: A Hierarchical Recurrent Denoiser", Ray Tracing Gems II ch. 49 (pre-blur with
//            hit-distance-scaled radii, history fix of disocclusions from a coarser scale, anti-firefly).
//   [Salvi]  Salvi, "An Excursion in Temporal Supersampling", GDC 2016 (variance clipping of the history).
//
// Signals: demodulated diffuse and specular radiance (rgb + hit distance of the continuation in w; 0 = none), each
// with its own history, moments, gradients and filter; guide: world normal + perceptual roughness, linear view depth
// (0 = sky), UV motion (prevUV = uv + motionScale * motion), optional instance ids; camera: pinhole
// dir = normalize(forward + right x + up y), x = 2 u - 1, y = 1 - 2 v, right / up scaled by tan(fov / 2) (the Relight
// path tracer's model); world position of a pixel = origin + dir(pixel centre) * depth / dot(dir, forward).
//
// Passes (one frame; RadianceDenoiser records them on render graph v2, RdnReference runs them on the CPU):
//   0 prepare      guide (n, z) and aux (roughness, instance) into the state; spatial firefly clamp of the inputs
//                  (a sample brighter than fireflyRatio x the brightest of its 3 x 3 same-surface neighbours is scaled
//                  down to that bound; 0 = off)
//   1 preblur      8-tap disk blur with radius preblurRadius x hitFactor (x roughness for the specular channel),
//                  hitFactor = hd / (hd + hitDistScale z), hd = the neighbours' mean hit distance (never the pixel's own:
//                  selection bias); plane / normal (/ roughness) edge stopping; reconstructs the hit distance (weighted
//                  mean of the valid taps) [RTG2]
//   2 grad.prep    A-SVGF samples (dCur, dPrev, sCur, sPrev per 3 x 3 stratum, dPrev < 0 = invalid) ->
//                  (dDelta, dMax, sDelta, sMax) [A-SVGF]
//   3 grad.atrous  5 x 5 edge-aware a-trous on the stratum grid; lambda = min(1, gradientScale |delta| / max)
//   4 temporal     bilinear reprojection; a tap counts when inside, same instance (optional), normals agree and the
//                  previous surface point lies on the current tangent plane or at the current depth (disocclusion
//                  test), and when no bilinear tap does, the consistent taps of the 3 x 3 around the nearest pixel
//                  (jittered edges); diffuse: surface motion; specular: surface motion blended with the virtual motion of
//                  X + dir hitDist projected into the previous camera by 1 - smoothstep(virtRough0, virtRough1, r)
//                  [TRMV]; specular (and optionally diffuse) history clipped to the 3 x 3 mean +- k sigma of the
//                  pre-blurred input [Salvi]; temporal anti-firefly (a sample above m1 + fireflySigma sigma of the
//                  history once it holds >= 4 frames is clamped [RTG2]); accumulation alpha = max(alpha_min, 1 / len),
//                  A-SVGF: len' = max(1, (1 - lambda) len), alpha' = (1 - lambda) alpha + lambda
//   5 mip (x 3)    history-fix pyramid: level L texel = sum over its 2 x 2 children of (w rgb, w) per channel,
//                  w = min(len, fixMaxWeight), and (sum z, count, sum n, sum roughness) of the valid pixels
//   6 historyfix   pixels with len < fixFrames take the depth / normal (/ roughness) weighted bilinear mean of level
//                  min(3, fixFrames - len) (coarser while empty), blended by len / fixFrames [RTG2]
//   7 variance     len < varianceHistory: 7 x 7 bilateral moments, boosted by varianceBoost / len; else m2 - m1^2
//   8 atrous (x N) 5 x 5 B3 a-trous, step 2^i: plane-distance, normal (sigma_n; specular exponent x (1 - r)^2) and
//                  luminance (sigma_l sqrt(3 x 3 Gaussian of the variance)) weights, the specular lobe footprint
//                  exp(-|o|^2 / 2 R^2) with R = specRadiusMin + r specRadiusMax [SVGF + RTG2]; variance filtered
//                  with squared weights; iteration historyTap writes the colour history ([SVGF] 4.1)

// ---- flags, passes ----------------------------------------------------------------------------------------------
#define FUSE_RDN_FLAG_HISTORY 1u      // the previous frame's state is valid
#define FUSE_RDN_FLAG_GRADIENTS 2u    // A-SVGF gradient samples present
#define FUSE_RDN_FLAG_VIRTUAL 4u      // specular virtual (hit-distance) motion
#define FUSE_RDN_FLAG_HISTFIX 8u      // history fix of short histories
#define FUSE_RDN_FLAG_SPATIALVAR 16u  // spatial variance estimate while the history is short
#define FUSE_RDN_FLAG_INSTANCE 32u    // instance ids take part in the disocclusion test
#define FUSE_RDN_FLAG_PREBLUR 64u     // hit-distance pre-blur

#define FUSE_RDN_PASS_PREPARE 0u
#define FUSE_RDN_PASS_PREBLUR 1u
#define FUSE_RDN_PASS_GRAD_PREPARE 2u
#define FUSE_RDN_PASS_GRAD_ATROUS 3u
#define FUSE_RDN_PASS_TEMPORAL 4u
#define FUSE_RDN_PASS_MIP 5u
#define FUSE_RDN_PASS_HISTFIX 6u
#define FUSE_RDN_PASS_VARIANCE 7u
#define FUSE_RDN_PASS_ATROUS 8u
#define FUSE_RDN_PASS_COUNT 9u

#define FUSE_RDN_STRATUM 3u
#define FUSE_RDN_MIP_LEVELS 3u

// ---- the frame record (std430 / C natural layout: 8-byte addresses first, then 4-byte scalars) -----------------
struct RdnFrame {
    // inputs (per pixel unless noted)
    uint64_t inDiffuse;  // f32x4 demodulated diffuse rgb, hit distance
    uint64_t inSpecular; // f32x4 demodulated specular rgb, hit distance
    uint64_t inNormal;   // f32x4 unit world normal, perceptual roughness
    uint64_t inDepth;    // f32 linear view depth (0 = sky)
    uint64_t inMotion;   // f32x2 UV motion
    uint64_t inInstance; // u32 instance id (FUSE_RDN_FLAG_INSTANCE)
    uint64_t inGradient; // f32x4 per stratum (FUSE_RDN_FLAG_GRADIENTS)
    uint64_t guideCur;   // state f32x4 (n, z)
    uint64_t guidePrev;
    uint64_t auxCur;     // state f32x4 (roughness, instance, 0, 0)
    uint64_t auxPrev;
    uint64_t histDCur;   // state f32x4 (rgb, history length)
    uint64_t histDPrev;
    uint64_t histSCur;
    uint64_t histSPrev;
    uint64_t momCur;     // state f32x4 (m1 D, m2 D, m1 S, m2 S)
    uint64_t momPrev;
    uint64_t preD;       // work f32x4 (rgb, hit distance) after the firefly clamp
    uint64_t preS;
    uint64_t blurD;      // work f32x4 (rgb, reconstructed hit distance) after the pre-blur
    uint64_t blurS;
    uint64_t accD;       // work f32x4 (rgb, len) temporal result
    uint64_t accS;
    uint64_t mip1;       // work 4 x f32x4 per texel: D (w rgb, w), S (w rgb, w), (sum z, count, sum n.x, sum n.y),
                         // (sum n.z, sum roughness, 0, 0)
    uint64_t mip2;
    uint64_t mip3;
    uint64_t fixD;       // work f32x4 (rgb, len) after the history fix
    uint64_t fixS;
    uint64_t varD;       // work f32x4 (rgb, variance)
    uint64_t varS;
    uint64_t lambda;     // work f32x4 per stratum: the filtered gradient the temporal pass reads
    uint64_t reserved0;
    // camera (current, previous): origin, right x tan, up x tan, unit forward
    float camOx, camOy, camOz, camRx, camRy, camRz, camUx, camUy, camUz, camFx, camFy, camFz;
    float prvOx, prvOy, prvOz, prvRx, prvRy, prvRz, prvUx, prvUy, prvUz, prvFx, prvFy, prvFz;
    uint width;
    uint height;
    uint strataW;
    uint strataH;
    uint flags;
    uint atrousIterations;
    uint gradientIterations;
    uint historyTap;
    uint sigmaNormalD;     // integer exponent of max(0, n.n')
    uint sigmaNormalS;     // specular: x (1 - r)^2, at least 1
    uint reserved1;
    uint reserved2;
    float motionScale;     // prevUV = uv + motionScale x motion (-1: current - previous; +1: previous - current)
    float alphaD;          // minimum blend weight of a new sample
    float alphaS;
    float maxHistD;        // history length cap (frames)
    float maxHistS;
    float reprojNormal;    // minimum dot(n_prev, n) of a history tap
    float reprojDepth;     // relative plane / depth tolerance (x z / max(|n.v|, 0.05))
    float minReprojWeight; // minimum summed bilinear weight of the valid taps
    float virtRough0;      // specular virtual motion weight = 1 - smoothstep(virtRough0, virtRough1, roughness)
    float virtRough1;
    float clampSigmaD;     // history clipping k (0 = off)
    float clampSigmaS;
    float fireflyRatio;    // spatial firefly bound (0 = off)
    float fireflySigma;    // temporal anti-firefly k (0 = off)
    float preblurRadiusD;  // pixels
    float preblurRadiusS;  // pixels (x roughness)
    float hitDistScale;    // hitFactor = hd / (hd + hitDistScale z)
    float fixFrames;       // history fix below this length
    float fixMaxWeight;    // pyramid weight cap
    float varianceHistory;
    float varianceBoost;
    float sigmaPlane;      // a-trous plane distance / (sigmaPlane z)
    float sigmaLumD;
    float sigmaLumS;
    float specRadiusMin;   // specular lobe footprint (pixels)
    float specRadiusMax;
    float gradientScale;
    float gradientEpsilon;
    float depthEpsilon;
    float sigmaRoughness;  // specular taps weigh exp(-sigmaRoughness |r_p - r_q|) (materials of different gloss)
};

// ---- helpers ----------------------------------------------------------------------------------------------------
FUSE_RDN_FN float rdnLum3(float3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
FUSE_RDN_FN float rdnLum(float4 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
FUSE_RDN_FN float3 rdnXyz(float4 v) { return float3(v.x, v.y, v.z); }
FUSE_RDN_FN float rdnDot4(float4 a, float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
FUSE_RDN_FN float rdnClamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
FUSE_RDN_FN float rdnSmoothstep(float a, float b, float v) {
    if (!(b > a)) {
        return v < a ? 0.0f : 1.0f;
    }
    float t = rdnClamp01((v - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}
FUSE_RDN_FN float rdnAtrousTap(int d) {
    int a = d < 0 ? -d : d;
    return a == 0 ? 0.375f : (a == 1 ? 0.25f : 0.0625f);
}
FUSE_RDN_FN float rdnGaussTap(int d) { return d == 0 ? 0.5f : 0.25f; }
FUSE_RDN_FN float rdnPowInt(float b, uint e) {
    float r = 1.0f;
    float bb = b;
    uint k = e;
    while (k != 0u) {
        if ((k & 1u) != 0u) {
            r = r * bb;
        }
        bb = bb * bb;
        k = k >> 1u;
    }
    return r;
}
FUSE_RDN_FN float rdnNormalWeight(float4 p, float4 q, uint e) {
    float d = rdnDot4(p, q);
    return rdnPowInt(d > 0.0f ? d : 0.0f, e);
}
FUSE_RDN_FN uint rdnSpecNormalExp(FUSE_RDN_IN(RdnFrame) F, float rough) {
    float r = rdnClamp01(rough);
    float e = float(F.sigmaNormalS) * (1.0f - r) * (1.0f - r);
    uint k = uint(e);
    return k > 1u ? k : 1u;
}
FUSE_RDN_FN bool rdnInside(FUSE_RDN_IN(RdnFrame) F, int x, int y) {
    return x >= 0 && y >= 0 && x < int(F.width) && y < int(F.height);
}
FUSE_RDN_FN float3 rdnCamO(FUSE_RDN_IN(RdnFrame) F) { return float3(F.camOx, F.camOy, F.camOz); }
FUSE_RDN_FN float3 rdnCamF(FUSE_RDN_IN(RdnFrame) F) { return float3(F.camFx, F.camFy, F.camFz); }
FUSE_RDN_FN float3 rdnPrvO(FUSE_RDN_IN(RdnFrame) F) { return float3(F.prvOx, F.prvOy, F.prvOz); }
FUSE_RDN_FN float3 rdnPrvF(FUSE_RDN_IN(RdnFrame) F) { return float3(F.prvFx, F.prvFy, F.prvFz); }
/// Primary direction through (fx, fy) (pixels) of the current (prev = false) or previous camera.
FUSE_RDN_FN float3 rdnDir(FUSE_RDN_IN(RdnFrame) F, bool prev, float fx, float fy) {
    float x = (fx / float(F.width)) * 2.0f - 1.0f;
    float y = 1.0f - (fy / float(F.height)) * 2.0f;
    float3 d;
    if (prev) {
        d = float3(F.prvFx + F.prvRx * x + F.prvUx * y, F.prvFy + F.prvRy * x + F.prvUy * y,
                   F.prvFz + F.prvRz * x + F.prvUz * y);
    } else {
        d = float3(F.camFx + F.camRx * x + F.camUx * y, F.camFy + F.camRy * x + F.camUy * y,
                   F.camFz + F.camRz * x + F.camUz * y);
    }
    return normalize(d);
}
/// World position of pixel (x, y) at view depth z.
FUSE_RDN_FN float3 rdnWorld(FUSE_RDN_IN(RdnFrame) F, bool prev, uint x, uint y, float z) {
    float3 d = rdnDir(F, prev, float(x) + 0.5f, float(y) + 0.5f);
    float3 o = prev ? rdnPrvO(F) : rdnCamO(F);
    float3 f = prev ? rdnPrvF(F) : rdnCamF(F);
    float t = z / dot(d, f);
    return o + d * t;
}
/// UV of world point p in the previous camera: (u, v, ok, 0).
FUSE_RDN_FN float4 rdnProjectPrev(FUSE_RDN_IN(RdnFrame) F, float3 p) {
    float3 v = p - rdnPrvO(F);
    float z = dot(v, rdnPrvF(F));
    if (!(z > 1e-6f)) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    float3 r = float3(F.prvRx, F.prvRy, F.prvRz);
    float3 u = float3(F.prvUx, F.prvUy, F.prvUz);
    float x = dot(v, r) / (z * dot(r, r));
    float y = dot(v, u) / (z * dot(u, u));
    return float4(x * 0.5f + 0.5f, 0.5f - y * 0.5f, 1.0f, 0.0f);
}
/// Representative pixel of a stratum (its centre, clamped).
FUSE_RDN_FN uint rdnStratumPixel(FUSE_RDN_IN(RdnFrame) F, uint sx, uint sy) {
    uint px = sx * FUSE_RDN_STRATUM + 1u < F.width ? sx * FUSE_RDN_STRATUM + 1u : F.width - 1u;
    uint py = sy * FUSE_RDN_STRATUM + 1u < F.height ? sy * FUSE_RDN_STRATUM + 1u : F.height - 1u;
    return py * F.width + px;
}
FUSE_RDN_FN float rdnLambda(FUSE_RDN_IN(RdnFrame) F, float delta, float mx) {
    if (!(mx > F.gradientEpsilon)) {
        return 0.0f;
    }
    float l = F.gradientScale * abs(delta) / mx;
    return l < 1.0f ? l : 1.0f;
}
/// Disk offsets of the pre-blur (8 taps: a unit ring and a half ring, alternating).
FUSE_RDN_FN float2 rdnDisk(uint k) {
    float r = (k & 1u) != 0u ? 0.5f : 1.0f;
    float c = 0.70710678f;
    float2 d = float2(1.0f, 0.0f);
    if (k == 1u) {
        d = float2(c, c);
    } else if (k == 2u) {
        d = float2(0.0f, 1.0f);
    } else if (k == 3u) {
        d = float2(-c, c);
    } else if (k == 4u) {
        d = float2(-1.0f, 0.0f);
    } else if (k == 5u) {
        d = float2(-c, -c);
    } else if (k == 6u) {
        d = float2(0.0f, -1.0f);
    } else if (k == 7u) {
        d = float2(c, -c);
    }
    return float2(d.x * r, d.y * r);
}
FUSE_RDN_FN int rdnRound(float v) { return int(floor(v + 0.5f)); }
FUSE_RDN_FN uint rdnMipW(FUSE_RDN_IN(RdnFrame) F, uint level) { return (F.width + (1u << level) - 1u) >> level; }
FUSE_RDN_FN uint rdnMipH(FUSE_RDN_IN(RdnFrame) F, uint level) { return (F.height + (1u << level) - 1u) >> level; }
FUSE_RDN_FN uint64_t rdnMipAddr(FUSE_RDN_IN(RdnFrame) F, uint level) {
    return level == 1u ? F.mip1 : (level == 2u ? F.mip2 : F.mip3);
}

// ---- 0 prepare ----------------------------------------------------------------------------------------------------
FUSE_RDN_FN float4 rdnFirefly(FUSE_RDN_IN(RdnFrame) F, uint64_t src, float4 c, uint x, uint y) {
    if (!(F.fireflyRatio > 0.0f)) {
        return c;
    }
    float mx = 0.0f;
    bool any = false;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int qx = int(x) + dx;
            int qy = int(y) + dy;
            if ((dx == 0 && dy == 0) || !rdnInside(F, qx, qy)) {
                continue;
            }
            uint q = uint(qy) * F.width + uint(qx);
            if (!(rdnLd1(F.inDepth, q) > 0.0f)) {
                continue;
            }
            float l = rdnLum(rdnLd4(src, q));
            mx = l > mx ? l : mx;
            any = true;
        }
    }
    float lc = rdnLum(c);
    float bound = F.fireflyRatio * mx;
    if (any && mx > 0.0f && lc > bound) {
        float s = bound / lc;
        return float4(c.x * s, c.y * s, c.z * s, c.w);
    }
    return c;
}

FUSE_RDN_FN void rdnPrepare(FUSE_RDN_IN(RdnFrame) F, uint x, uint y) {
    uint i = y * F.width + x;
    float4 nr = rdnLd4(F.inNormal, i);
    float z = rdnLd1(F.inDepth, i);
    float inst = (F.flags & FUSE_RDN_FLAG_INSTANCE) != 0u ? float(rdnLdU(F.inInstance, i) & 0xFFFFFFu) : 0.0f;
    float4 d = rdnLd4(F.inDiffuse, i);
    float4 s = rdnLd4(F.inSpecular, i);
    if (!(z > 0.0f)) {
        rdnSt4(F.guideCur, i, float4(0.0f, 0.0f, 0.0f, 0.0f));
        rdnSt4(F.auxCur, i, float4(1.0f, inst, 0.0f, 0.0f));
        rdnSt4(F.preD, i, d);
        rdnSt4(F.preS, i, s);
        return;
    }
    rdnSt4(F.guideCur, i, float4(nr.x, nr.y, nr.z, z));
    rdnSt4(F.auxCur, i, float4(nr.w, inst, 0.0f, 0.0f));
    rdnSt4(F.preD, i, rdnFirefly(F, F.inDiffuse, d, x, y));
    rdnSt4(F.preS, i, rdnFirefly(F, F.inSpecular, s, x, y));
}

// ---- 1 preblur ----------------------------------------------------------------------------------------------------
FUSE_RDN_FN float rdnHitFactor(FUSE_RDN_IN(RdnFrame) F, float hd, float z) {
    if (!(hd > 0.0f)) {
        return 1.0f;
    }
    return hd / (hd + F.hitDistScale * z);
}

FUSE_RDN_FN float4 rdnPreblurChannel(FUSE_RDN_IN(RdnFrame) F, uint64_t src, float radius, uint x, uint y, float4 g,
                                     float3 X, float tol, float rough, bool spec) {
    uint i = y * F.width + x;
    float4 c = rdnLd4(src, i);
    float sumW = 1.0f;
    float3 sum = rdnXyz(c);
    float hdW = c.w > 0.0f ? 1.0f : 0.0f;
    float hdSum = c.w > 0.0f ? c.w : 0.0f;
    // At least a 1.5 px ring for the hit-distance reconstruction; the colour taps fade in over the first pixel of radius.
    float r = radius > 1.5f ? radius : 1.5f;
    float cw = rdnClamp01(radius);
    for (uint k = 0u; k < 8u; ++k) {
        float2 o = rdnDisk(k);
        int qx = int(x) + rdnRound(o.x * r);
        int qy = int(y) + rdnRound(o.y * r);
        if (!rdnInside(F, qx, qy)) {
            continue;
        }
        uint q = uint(qy) * F.width + uint(qx);
        float4 gq = rdnLd4(F.guideCur, q);
        if (!(gq.w > 0.0f)) {
            continue;
        }
        float3 Xq = rdnWorld(F, false, uint(qx), uint(qy), gq.w);
        float pd = abs(dot(Xq - X, rdnXyz(g)));
        float wr = spec ? F.sigmaRoughness * abs(rdnLd4(F.auxCur, q).x - rough) : 0.0f;
        float w = rdnPowInt(rdnDot4(g, gq) > 0.0f ? rdnDot4(g, gq) : 0.0f, 8u) * exp(-(pd / tol + wr));
        float4 cq = rdnLd4(src, q);
        if (cq.w > 0.0f) {
            hdSum = hdSum + w * cq.w;
            hdW = hdW + w;
        }
        sum = sum + rdnXyz(cq) * (w * cw);
        sumW = sumW + w * cw;
    }
    float3 m = sum * (1.0f / sumW);
    return float4(m.x, m.y, m.z, hdW > 0.0f ? hdSum / hdW : 0.0f);
}

/// Mean hit distance of the valid 3 x 3 neighbours, the pixel itself excluded: the pre-blur radius must not depend on
/// the pixel's own sample (a radius chosen by the sample's own hit distance would blur bright and dark samples
/// differently - a selection bias).
FUSE_RDN_FN float rdnNeighbourHitDist(FUSE_RDN_IN(RdnFrame) F, uint64_t src, uint x, uint y) {
    float s = 0.0f;
    float n = 0.0f;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int qx = int(x) + dx;
            int qy = int(y) + dy;
            if ((dx == 0 && dy == 0) || !rdnInside(F, qx, qy)) {
                continue;
            }
            float hd = rdnLd4(src, uint(qy) * F.width + uint(qx)).w;
            if (hd > 0.0f) {
                s = s + hd;
                n = n + 1.0f;
            }
        }
    }
    return n > 0.0f ? s / n : 0.0f;
}

FUSE_RDN_FN void rdnPreblur(FUSE_RDN_IN(RdnFrame) F, uint x, uint y) {
    uint i = y * F.width + x;
    float4 g = rdnLd4(F.guideCur, i);
    if (!(g.w > 0.0f)) {
        rdnSt4(F.blurD, i, rdnLd4(F.preD, i));
        rdnSt4(F.blurS, i, rdnLd4(F.preS, i));
        return;
    }
    float rough = rdnLd4(F.auxCur, i).x;
    float3 X = rdnWorld(F, false, x, y, g.w);
    float tol = F.reprojDepth * g.w + F.depthEpsilon;
    bool on = (F.flags & FUSE_RDN_FLAG_PREBLUR) != 0u;
    float rD = on ? F.preblurRadiusD * rdnHitFactor(F, rdnNeighbourHitDist(F, F.preD, x, y), g.w) : 0.0f;
    float rS = on ? F.preblurRadiusS * rdnClamp01(rough) * rdnHitFactor(F, rdnNeighbourHitDist(F, F.preS, x, y), g.w)
                  : 0.0f;
    rdnSt4(F.blurD, i, rdnPreblurChannel(F, F.preD, rD, x, y, g, X, tol, rough, false));
    rdnSt4(F.blurS, i, rdnPreblurChannel(F, F.preS, rS, x, y, g, X, tol, rough, true));
}

// ---- 2 / 3 A-SVGF gradients -------------------------------------------------------------------------------------
FUSE_RDN_FN void rdnGradPrepare(FUSE_RDN_IN(RdnFrame) F, uint64_t dst, uint sx, uint sy) {
    uint s = sy * F.strataW + sx;
    float4 g = rdnLd4(F.guideCur, rdnStratumPixel(F, sx, sy));
    float4 in4 = rdnLd4(F.inGradient, s);
    if (!(in4.y >= 0.0f) || !(g.w > 0.0f)) {
        rdnSt4(dst, s, float4(0.0f, 0.0f, 0.0f, 0.0f));
        return;
    }
    float dm = abs(in4.x) > abs(in4.y) ? abs(in4.x) : abs(in4.y);
    float sm = abs(in4.z) > abs(in4.w) ? abs(in4.z) : abs(in4.w);
    rdnSt4(dst, s, float4(in4.x - in4.y, dm, in4.z - in4.w, sm));
}

FUSE_RDN_FN void rdnGradAtrous(FUSE_RDN_IN(RdnFrame) F, uint64_t src, uint64_t dst, uint stp, uint sx, uint sy) {
    uint s = sy * F.strataW + sx;
    float4 g = rdnLd4(F.guideCur, rdnStratumPixel(F, sx, sy));
    if (!(g.w > 0.0f)) {
        rdnSt4(dst, s, rdnLd4(src, s));
        return;
    }
    float sumW = 0.0f;
    float4 acc = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            int qx = int(sx) + dx * int(stp);
            int qy = int(sy) + dy * int(stp);
            if (qx < 0 || qy < 0 || qx >= int(F.strataW) || qy >= int(F.strataH)) {
                continue;
            }
            float4 gq = rdnLd4(F.guideCur, rdnStratumPixel(F, uint(qx), uint(qy)));
            if (!(gq.w > 0.0f)) {
                continue;
            }
            float wz = abs(g.w - gq.w) / (g.w * F.reprojDepth + F.depthEpsilon);
            float w = rdnAtrousTap(dx) * rdnAtrousTap(dy) * rdnNormalWeight(g, gq, F.sigmaNormalD) * exp(-wz);
            float4 v = rdnLd4(src, uint(qy) * F.strataW + uint(qx));
            acc = acc + v * w;
            sumW = sumW + w;
        }
    }
    rdnSt4(dst, s, acc * (1.0f / sumW));
}

// ---- 4 temporal ---------------------------------------------------------------------------------------------------
struct RdnHist {
    float4 d;   // sum w (rgb, len)
    float4 s;
    float4 mom; // sum w (m1 D, m2 D, m1 S, m2 S)
    float w;
};

/// One history tap j with weight wt if it passes the disocclusion tests (same instance, normals agree, the previous
/// surface point on the current tangent plane or at the current depth).
FUSE_RDN_FN void rdnTap(FUSE_RDN_IN(RdnFrame) F, int tx, int ty, float wt, float4 g, float inst, float3 X, float tol,
                        FUSE_RDN_INOUT(RdnHist) h) {
    if (!rdnInside(F, tx, ty) || !(wt > 0.0f)) {
        return;
    }
    uint j = uint(ty) * F.width + uint(tx);
    float4 gp = rdnLd4(F.guidePrev, j);
    if (!(gp.w > 0.0f) || rdnDot4(gp, g) < F.reprojNormal) {
        return;
    }
    if ((F.flags & FUSE_RDN_FLAG_INSTANCE) != 0u && rdnLd4(F.auxPrev, j).y != inst) {
        return;
    }
    float3 Xp = rdnWorld(F, true, uint(tx), uint(ty), gp.w);
    float plane = abs(dot(Xp - X, rdnXyz(g)));
    if (plane > tol && abs(gp.w - g.w) > tol) {
        return;
    }
    h.d = h.d + rdnLd4(F.histDPrev, j) * wt;
    h.s = h.s + rdnLd4(F.histSPrev, j) * wt;
    h.mom = h.mom + rdnLd4(F.momPrev, j) * wt;
    h.w = h.w + wt;
}

/// Gather of the previous state at UV (u, v): bilinear over the 4 taps that pass the disocclusion tests; when none
/// does, the consistent taps of the 3 x 3 neighbourhood of the nearest pixel ([SVGF] 4.1: sub-pixel jitter makes the
/// G-buffer of edge pixels alternate between surfaces, the neighbours keep the surface's history).
FUSE_RDN_FN RdnHist rdnGather(FUSE_RDN_IN(RdnFrame) F, float u, float v, float4 g, float inst, float3 X, float tol) {
    RdnHist h;
    h.d = float4(0.0f, 0.0f, 0.0f, 0.0f);
    h.s = float4(0.0f, 0.0f, 0.0f, 0.0f);
    h.mom = float4(0.0f, 0.0f, 0.0f, 0.0f);
    h.w = 0.0f;
    float w = float(F.width);
    float hh = float(F.height);
    float px = u * w - 0.5f;
    float py = v * hh - 0.5f;
    // Far outside (or NaN): no tap can be inside; the clamp keeps the int conversion defined.
    px = px > -2.0f ? (px < w + 1.0f ? px : w + 1.0f) : -2.0f;
    py = py > -2.0f ? (py < hh + 1.0f ? py : hh + 1.0f) : -2.0f;
    float x0 = floor(px);
    float y0 = floor(py);
    float fx = px - x0;
    float fy = py - y0;
    int ix = int(x0);
    int iy = int(y0);
    for (uint t = 0u; t < 4u; ++t) {
        float wx = (t & 1u) != 0u ? fx : 1.0f - fx;
        float wy = (t >> 1u) != 0u ? fy : 1.0f - fy;
        rdnTap(F, ix + int(t & 1u), iy + int(t >> 1u), wx * wy, g, inst, X, tol, h);
    }
    if (!(h.w >= F.minReprojWeight)) {
        h.d = float4(0.0f, 0.0f, 0.0f, 0.0f);
        h.s = h.d;
        h.mom = h.d;
        h.w = 0.0f;
        int cx = ix + (fx >= 0.5f ? 1 : 0);
        int cy = iy + (fy >= 0.5f ? 1 : 0);
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                rdnTap(F, cx + dx, cy + dy, 1.0f, g, inst, X, tol, h);
            }
        }
    }
    return h;
}

/// Mean +- k sigma box of the 3 x 3 pre-blurred neighbourhood (same surface) -> clipped history rgb.
FUSE_RDN_FN float4 rdnClip(FUSE_RDN_IN(RdnFrame) F, uint64_t src, float4 hist, float k, uint x, uint y) {
    if (!(k > 0.0f)) {
        return hist;
    }
    float3 m1 = float3(0.0f, 0.0f, 0.0f);
    float3 m2 = float3(0.0f, 0.0f, 0.0f);
    float n = 0.0f;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int qx = int(x) + dx;
            int qy = int(y) + dy;
            if (!rdnInside(F, qx, qy)) {
                continue;
            }
            uint q = uint(qy) * F.width + uint(qx);
            if (!(rdnLd4(F.guideCur, q).w > 0.0f)) {
                continue;
            }
            float3 c = rdnXyz(rdnLd4(src, q));
            m1 = m1 + c;
            m2 = m2 + c * c;
            n = n + 1.0f;
        }
    }
    float inv = 1.0f / n;
    m1 = m1 * inv;
    m2 = m2 * inv;
    float3 vr = m2 - m1 * m1;
    float3 sd = float3(sqrt(max(vr.x, 0.0f)), sqrt(max(vr.y, 0.0f)), sqrt(max(vr.z, 0.0f)));
    float3 lo = m1 - sd * k;
    float3 hi = m1 + sd * k;
    return float4(min(max(hist.x, lo.x), hi.x), min(max(hist.y, lo.y), hi.y), min(max(hist.z, lo.z), hi.z), hist.w);
}

struct RdnAccum {
    float4 color; // (rgb, len)
    float m1;
    float m2;
};

/// One channel's accumulation: `hist` (rgb, len) and moments (m1, m2) already normalised; valid = a history exists.
FUSE_RDN_FN RdnAccum rdnAccumulate(FUSE_RDN_IN(RdnFrame) F, float4 cur, float4 hist, float m1, float m2, bool valid,
                              float alphaMin, float maxHist, float lambda) {
    RdnAccum a;
    float l = rdnLum(cur);
    float3 c = rdnXyz(cur);
    if (!valid) {
        a.color = float4(c.x, c.y, c.z, 1.0f);
        a.m1 = l;
        a.m2 = l * l;
        return a;
    }
    float len = hist.w + 1.0f;
    len = len < maxHist ? len : maxHist;
    if ((F.flags & FUSE_RDN_FLAG_GRADIENTS) != 0u) {
        len = len * (1.0f - lambda);
        len = len > 1.0f ? len : 1.0f;
    }
    // Temporal anti-firefly: a sample far above the accumulated distribution is clamped to its bound.
    if (F.fireflySigma > 0.0f && hist.w >= 4.0f && l > 0.0f) {
        float sd = sqrt(max(m2 - m1 * m1, 0.0f));
        float bound = m1 + F.fireflySigma * sd;
        if (l > bound) {
            float s = bound / l;
            c = c * s;
            l = bound;
        }
    }
    float inv = 1.0f / len;
    float alpha = alphaMin > inv ? alphaMin : inv;
    if ((F.flags & FUSE_RDN_FLAG_GRADIENTS) != 0u) {
        alpha = alpha * (1.0f - lambda) + lambda;
    }
    float b = 1.0f - alpha;
    float3 o = rdnXyz(hist) * b + c * alpha;
    a.color = float4(o.x, o.y, o.z, len);
    a.m1 = m1 * b + l * alpha;
    a.m2 = m2 * b + (l * l) * alpha;
    return a;
}

FUSE_RDN_FN void rdnTemporal(FUSE_RDN_IN(RdnFrame) F, uint x, uint y) {
    uint i = y * F.width + x;
    float4 bD = rdnLd4(F.blurD, i);
    float4 bS = rdnLd4(F.blurS, i);
    float4 g = rdnLd4(F.guideCur, i);
    if (!(g.w > 0.0f)) {
        float lD = rdnLum(bD);
        float lS = rdnLum(bS);
        rdnSt4(F.accD, i, float4(bD.x, bD.y, bD.z, 0.0f));
        rdnSt4(F.accS, i, float4(bS.x, bS.y, bS.z, 0.0f));
        rdnSt4(F.momCur, i, float4(lD, lD * lD, lS, lS * lS));
        return;
    }
    float4 aux = rdnLd4(F.auxCur, i);
    float3 dir = rdnDir(F, false, float(x) + 0.5f, float(y) + 0.5f);
    float t = g.w / dot(dir, rdnCamF(F));
    float3 X = rdnCamO(F) + dir * t;
    float cosv = abs(dot(rdnXyz(g), dir));
    cosv = cosv > 0.05f ? cosv : 0.05f;
    float tol = F.reprojDepth * g.w / cosv + F.depthEpsilon;
    bool history = (F.flags & FUSE_RDN_FLAG_HISTORY) != 0u;

    // Surface reprojection (motion vectors).
    RdnHist hs;
    hs.d = float4(0.0f, 0.0f, 0.0f, 0.0f);
    hs.s = hs.d;
    hs.mom = hs.d;
    hs.w = 0.0f;
    RdnHist hv = hs;
    if (history) {
        float2 m = rdnLd2(F.inMotion, i);
        float u = (float(x) + 0.5f) / float(F.width) + F.motionScale * m.x;
        float v = (float(y) + 0.5f) / float(F.height) + F.motionScale * m.y;
        hs = rdnGather(F, u, v, g, aux.y, X, tol);
    }
    // Specular virtual motion: the reflected point seen "behind" the surface at the hit distance.
    float vAmt = 0.0f;
    if (history && (F.flags & FUSE_RDN_FLAG_VIRTUAL) != 0u && bS.w > 0.0f) {
        vAmt = 1.0f - rdnSmoothstep(F.virtRough0, F.virtRough1, aux.x);
        if (vAmt > 0.0f) {
            float3 Xv = rdnCamO(F) + dir * (t + bS.w);
            float4 pv = rdnProjectPrev(F, Xv);
            if (pv.z > 0.0f) {
                hv = rdnGather(F, pv.x, pv.y, g, aux.y, X, tol);
            }
        }
    }
    float lambdaD = 0.0f;
    float lambdaS = 0.0f;
    if ((F.flags & FUSE_RDN_FLAG_GRADIENTS) != 0u) {
        float4 lg = rdnLd4(F.lambda, (y / FUSE_RDN_STRATUM) * F.strataW + x / FUSE_RDN_STRATUM);
        lambdaD = rdnLambda(F, lg.x, lg.y);
        lambdaS = rdnLambda(F, lg.z, lg.w);
    }

    // Diffuse.
    bool vD = hs.w >= F.minReprojWeight && hs.w > 0.0f;
    float4 hD = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float m1D = 0.0f;
    float m2D = 0.0f;
    if (vD) {
        float inv = 1.0f / hs.w;
        hD = hs.d * inv;
        m1D = hs.mom.x * inv;
        m2D = hs.mom.y * inv;
        float len = hD.w;
        hD = rdnClip(F, F.blurD, hD, F.clampSigmaD, x, y);
        hD.w = len;
    }
    RdnAccum aD = rdnAccumulate(F, bD, hD, m1D, m2D, vD, F.alphaD, F.maxHistD, lambdaD);

    // Specular: surface and virtual histories blended by vAmt.
    bool okS = hs.w >= F.minReprojWeight && hs.w > 0.0f;
    bool okV = hv.w >= F.minReprojWeight && hv.w > 0.0f;
    float wS = okS ? 1.0f - vAmt : 0.0f;
    float wV = okV ? vAmt : 0.0f;
    bool vS = wS + wV > 0.0f;
    float4 hS = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float m1S = 0.0f;
    float m2S = 0.0f;
    if (vS) {
        float4 sS = okS ? hs.s * (1.0f / hs.w) : hS;
        float4 sV = okV ? hv.s * (1.0f / hv.w) : hS;
        float mS1 = okS ? hs.mom.z / hs.w : 0.0f;
        float mS2 = okS ? hs.mom.w / hs.w : 0.0f;
        float mV1 = okV ? hv.mom.z / hv.w : 0.0f;
        float mV2 = okV ? hv.mom.w / hv.w : 0.0f;
        float inv = 1.0f / (wS + wV);
        hS = (sS * wS + sV * wV) * inv;
        m1S = (mS1 * wS + mV1 * wV) * inv;
        m2S = (mS2 * wS + mV2 * wV) * inv;
        float len = hS.w;
        hS = rdnClip(F, F.blurS, hS, F.clampSigmaS, x, y);
        hS.w = len;
    }
    RdnAccum aS = rdnAccumulate(F, bS, hS, m1S, m2S, vS, F.alphaS, F.maxHistS, lambdaS);
    rdnSt4(F.accD, i, aD.color);
    rdnSt4(F.accS, i, aS.color);
    rdnSt4(F.momCur, i, float4(aD.m1, aD.m2, aS.m1, aS.m2));
}

// ---- 5 history-fix pyramid ---------------------------------------------------------------------------------------
FUSE_RDN_FN void rdnMip(FUSE_RDN_IN(RdnFrame) F, uint64_t src, uint64_t dst, uint level, uint tx, uint ty) {
    uint mw = rdnMipW(F, level);
    uint o = (ty * mw + tx) * 4u;
    float4 d = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 s = d;
    float4 z = d;
    float4 n = d;
    for (uint k = 0u; k < 4u; ++k) {
        uint cx = tx * 2u + (k & 1u);
        uint cy = ty * 2u + (k >> 1u);
        if (level == 1u) {
            if (cx >= F.width || cy >= F.height) {
                continue;
            }
            uint i = cy * F.width + cx;
            float4 g = rdnLd4(F.guideCur, i);
            if (!(g.w > 0.0f)) {
                continue;
            }
            float4 aD = rdnLd4(F.accD, i);
            float4 aS = rdnLd4(F.accS, i);
            float wD = aD.w < F.fixMaxWeight ? aD.w : F.fixMaxWeight;
            float wS = aS.w < F.fixMaxWeight ? aS.w : F.fixMaxWeight;
            d = d + float4(aD.x * wD, aD.y * wD, aD.z * wD, wD);
            s = s + float4(aS.x * wS, aS.y * wS, aS.z * wS, wS);
            z = z + float4(g.w, 1.0f, g.x, g.y);
            n = n + float4(g.z, rdnLd4(F.auxCur, i).x, 0.0f, 0.0f);
        } else {
            uint pw = rdnMipW(F, level - 1u);
            uint ph = rdnMipH(F, level - 1u);
            if (cx >= pw || cy >= ph) {
                continue;
            }
            uint c = (cy * pw + cx) * 4u;
            d = d + rdnLd4(src, c);
            s = s + rdnLd4(src, c + 1u);
            z = z + rdnLd4(src, c + 2u);
            n = n + rdnLd4(src, c + 3u);
        }
    }
    rdnSt4(dst, o, d);
    rdnSt4(dst, o + 1u, s);
    rdnSt4(dst, o + 2u, z);
    rdnSt4(dst, o + 3u, n);
}

// ---- 6 history fix ------------------------------------------------------------------------------------------------
/// Bilinear read of channel `ch` (0 = D, 1 = S) at pyramid `level`, weighted by the texels' mean depth and normal
/// (and, for the specular channel, mean roughness) against the pixel's: (sum rgb, sum w).
FUSE_RDN_FN float4 rdnMipSample(FUSE_RDN_IN(RdnFrame) F, uint level, uint ch, uint x, uint y, float4 g, float rough,
                                float tol) {
    uint mw = rdnMipW(F, level);
    uint mh = rdnMipH(F, level);
    float scale = 1.0f / float(1u << level);
    float px = (float(x) + 0.5f) * scale - 0.5f;
    float py = (float(y) + 0.5f) * scale - 0.5f;
    float x0 = floor(px);
    float y0 = floor(py);
    float fx = px - x0;
    float fy = py - y0;
    uint64_t base = rdnMipAddr(F, level);
    float4 acc = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (uint t = 0u; t < 4u; ++t) {
        int tx = int(x0) + int(t & 1u);
        int ty = int(y0) + int(t >> 1u);
        if (tx < 0 || ty < 0 || tx >= int(mw) || ty >= int(mh)) {
            continue;
        }
        uint o = (uint(ty) * mw + uint(tx)) * 4u;
        float4 zc = rdnLd4(base, o + 2u);
        if (!(zc.y > 0.0f)) {
            continue;
        }
        float4 nc = rdnLd4(base, o + 3u);
        float3 nm = float3(zc.z, zc.w, nc.x);
        float nl = sqrt(dot(nm, nm));
        float cosn = nl > 0.0f ? dot(nm, rdnXyz(g)) / nl : 0.0f;
        float bw = ((t & 1u) != 0u ? fx : 1.0f - fx) * ((t >> 1u) != 0u ? fy : 1.0f - fy);
        float dz = abs(zc.x / zc.y - g.w) / tol;
        float dr = ch == 1u ? F.sigmaRoughness * abs(nc.y / zc.y - rough) : 0.0f;
        float w = bw * rdnPowInt(cosn > 0.0f ? cosn : 0.0f, 8u) * exp(-(dz + dr));
        acc = acc + rdnLd4(base, o + ch) * w;
    }
    return acc;
}

FUSE_RDN_FN float4 rdnFixChannel(FUSE_RDN_IN(RdnFrame) F, float4 a, uint ch, uint x, uint y, float4 g, float rough,
                                  float tol) {
    if ((F.flags & FUSE_RDN_FLAG_HISTFIX) == 0u || !(a.w < F.fixFrames)) {
        return a;
    }
    float need = F.fixFrames - a.w;
    uint level = need >= 3.0f ? 3u : (need >= 2.0f ? 2u : 1u);
    float4 m = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (uint l = level; l <= FUSE_RDN_MIP_LEVELS; ++l) {
        m = rdnMipSample(F, l, ch, x, y, g, rough, tol * float(1u << l));
        if (m.w > 0.0f) {
            break;
        }
    }
    if (!(m.w > 0.0f)) {
        return a;
    }
    float inv = 1.0f / m.w;
    float tb = a.w / F.fixFrames;
    float3 o = float3(m.x * inv, m.y * inv, m.z * inv) * (1.0f - tb) + rdnXyz(a) * tb;
    return float4(o.x, o.y, o.z, a.w);
}

FUSE_RDN_FN void rdnHistoryFix(FUSE_RDN_IN(RdnFrame) F, uint x, uint y) {
    uint i = y * F.width + x;
    float4 g = rdnLd4(F.guideCur, i);
    float4 aD = rdnLd4(F.accD, i);
    float4 aS = rdnLd4(F.accS, i);
    if (!(g.w > 0.0f)) {
        rdnSt4(F.fixD, i, aD);
        rdnSt4(F.fixS, i, aS);
        return;
    }
    float tol = F.reprojDepth * g.w + F.depthEpsilon;
    float rough = rdnLd4(F.auxCur, i).x;
    rdnSt4(F.fixD, i, rdnFixChannel(F, aD, 0u, x, y, g, rough, tol));
    rdnSt4(F.fixS, i, rdnFixChannel(F, aS, 1u, x, y, g, rough, tol));
}

// ---- 7 variance ---------------------------------------------------------------------------------------------------
FUSE_RDN_FN void rdnVariance(FUSE_RDN_IN(RdnFrame) F, uint x, uint y) {
    uint i = y * F.width + x;
    float4 g = rdnLd4(F.guideCur, i);
    float4 fD = rdnLd4(F.fixD, i);
    float4 fS = rdnLd4(F.fixS, i);
    float4 mo = rdnLd4(F.momCur, i);
    float vD = max(mo.y - mo.x * mo.x, 0.0f);
    float vS = max(mo.w - mo.z * mo.z, 0.0f);
    if (!(g.w > 0.0f)) {
        rdnSt4(F.varD, i, float4(fD.x, fD.y, fD.z, 0.0f));
        rdnSt4(F.varS, i, float4(fS.x, fS.y, fS.z, 0.0f));
        return;
    }
    bool spatial = (F.flags & FUSE_RDN_FLAG_SPATIALVAR) != 0u;
    if (spatial && (fD.w < F.varianceHistory || fS.w < F.varianceHistory)) {
        float3 X = rdnWorld(F, false, x, y, g.w);
        float tol = F.sigmaPlane * g.w + F.depthEpsilon;
        float lD = rdnLum(fD);
        float lS = rdnLum(fS);
        float4 sw = float4(0.0f, 0.0f, 0.0f, 0.0f);
        float4 sm = float4(0.0f, 0.0f, 0.0f, 0.0f);
        for (int dy = -3; dy <= 3; ++dy) {
            for (int dx = -3; dx <= 3; ++dx) {
                int qx = int(x) + dx;
                int qy = int(y) + dy;
                if (!rdnInside(F, qx, qy)) {
                    continue;
                }
                uint q = uint(qy) * F.width + uint(qx);
                float4 gq = rdnLd4(F.guideCur, q);
                if (!(gq.w > 0.0f)) {
                    continue;
                }
                float3 Xq = rdnWorld(F, false, uint(qx), uint(qy), gq.w);
                float wp = abs(dot(Xq - X, rdnXyz(g))) / tol;
                float wn = rdnNormalWeight(g, gq, F.sigmaNormalD);
                float4 mq = rdnLd4(F.momCur, q);
                float wd = wn * exp(-(wp + abs(lD - rdnLum(rdnLd4(F.fixD, q))) / (F.sigmaLumD * 2.5f)));
                float ws = wn * exp(-(wp + abs(lS - rdnLum(rdnLd4(F.fixS, q))) / (F.sigmaLumS * 2.5f)));
                sw = sw + float4(wd, ws, 0.0f, 0.0f);
                sm = sm + float4(mq.x * wd, mq.y * wd, mq.z * ws, mq.w * ws);
            }
        }
        if (fD.w < F.varianceHistory && sw.x > 0.0f) {
            float m1 = sm.x / sw.x;
            float v = max(sm.y / sw.x - m1 * m1, 0.0f);
            vD = v * (F.varianceBoost / (fD.w > 1.0f ? fD.w : 1.0f));
        }
        if (fS.w < F.varianceHistory && sw.y > 0.0f) {
            float m1 = sm.z / sw.y;
            float v = max(sm.w / sw.y - m1 * m1, 0.0f);
            vS = v * (F.varianceBoost / (fS.w > 1.0f ? fS.w : 1.0f));
        }
    }
    rdnSt4(F.varD, i, float4(fD.x, fD.y, fD.z, vD));
    rdnSt4(F.varS, i, float4(fS.x, fS.y, fS.z, vS));
}

// ---- 8 a-trous ----------------------------------------------------------------------------------------------------
FUSE_RDN_FN float rdnGaussVar(FUSE_RDN_IN(RdnFrame) F, uint64_t src, uint x, uint y) {
    float gs = 0.0f;
    float gw = 0.0f;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int qx = int(x) + dx;
            int qy = int(y) + dy;
            if (!rdnInside(F, qx, qy)) {
                continue;
            }
            uint q = uint(qy) * F.width + uint(qx);
            if (!(rdnLd4(F.guideCur, q).w > 0.0f)) {
                continue;
            }
            float k = rdnGaussTap(dx) * rdnGaussTap(dy);
            gs = gs + k * rdnLd4(src, q).w;
            gw = gw + k;
        }
    }
    return gs / gw;
}

/// One channel's a-trous iteration at (x, y); spec = specular (lobe footprint, roughness-scaled normal exponent).
FUSE_RDN_FN float4 rdnAtrousChannel(FUSE_RDN_IN(RdnFrame) F, uint64_t src, bool spec, uint stp, uint x, uint y, float4 g,
                               float rough) {
    uint i = y * F.width + x;
    float4 in4 = rdnLd4(src, i);
    float gvar = rdnGaussVar(F, src, x, y);
    float sigmaL = spec ? F.sigmaLumS : F.sigmaLumD;
    float phiL = sigmaL * sqrt(gvar > 0.0f ? gvar : 0.0f) + 1e-6f;
    uint ne = spec ? rdnSpecNormalExp(F, rough) : F.sigmaNormalD;
    float R = F.specRadiusMin + rdnClamp01(rough) * F.specRadiusMax;
    float inv2R2 = R > 0.0f ? 1.0f / (2.0f * R * R) : 0.0f;
    if (spec && !(R > 0.0f)) {
        return in4;
    }
    float3 X = rdnWorld(F, false, x, y, g.w);
    float tol = F.sigmaPlane * g.w + F.depthEpsilon;
    float lp = rdnLum(in4);
    float sumW = 0.0f;
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float sv = 0.0f;
    int st = int(stp);
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            int ox = dx * st;
            int oy = dy * st;
            int qx = int(x) + ox;
            int qy = int(y) + oy;
            if (!rdnInside(F, qx, qy)) {
                continue;
            }
            uint q = uint(qy) * F.width + uint(qx);
            float4 gq = rdnLd4(F.guideCur, q);
            if (!(gq.w > 0.0f)) {
                continue;
            }
            float4 sq = rdnLd4(src, q);
            float3 Xq = rdnWorld(F, false, uint(qx), uint(qy), gq.w);
            float wp = abs(dot(Xq - X, rdnXyz(g))) / tol;
            float wl = abs(lp - rdnLum(sq)) / phiL;
            float lobe = spec ? float(ox * ox + oy * oy) * inv2R2 + F.sigmaRoughness * abs(rdnLd4(F.auxCur, q).x - rough)
                              : 0.0f;
            float w = rdnAtrousTap(dx) * rdnAtrousTap(dy) * rdnNormalWeight(g, gq, ne) * exp(-(wp + wl + lobe));
            sumW = sumW + w;
            sum = sum + rdnXyz(sq) * w;
            sv = sv + (w * w) * sq.w;
        }
    }
    if (!(sumW > 0.0f)) {
        return in4;
    }
    float3 o = sum * (1.0f / sumW);
    return float4(o.x, o.y, o.z, sv / (sumW * sumW));
}

FUSE_RDN_FN void rdnAtrous(FUSE_RDN_IN(RdnFrame) F, uint64_t srcD, uint64_t srcS, uint64_t dstD, uint64_t dstS, uint64_t hD,
                      uint64_t hS, uint stp, uint x, uint y) {
    uint i = y * F.width + x;
    float4 g = rdnLd4(F.guideCur, i);
    float4 oD = rdnLd4(srcD, i);
    float4 oS = rdnLd4(srcS, i);
    if (g.w > 0.0f) {
        float rough = rdnLd4(F.auxCur, i).x;
        oD = rdnAtrousChannel(F, srcD, false, stp, x, y, g, rough);
        oS = rdnAtrousChannel(F, srcS, true, stp, x, y, g, rough);
    }
    rdnSt4(dstD, i, oD);
    rdnSt4(dstS, i, oS);
    if (hD != uint64_t(0)) {
        rdnSt4(hD, i, float4(oD.x, oD.y, oD.z, rdnLd4(F.fixD, i).w));
        rdnSt4(hS, i, float4(oS.x, oS.y, oS.z, rdnLd4(F.fixS, i).w));
    }
}

// ---- dispatch ------------------------------------------------------------------------------------------------------
/// One invocation of pass `pass` at item (x, y); a0..a5 / stp are the pass's push arguments (RdnPush).
FUSE_RDN_FN void rdnRun(FUSE_RDN_IN(RdnFrame) F, uint pass, uint stp, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                   uint64_t a4, uint64_t a5, uint x, uint y) {
    if (pass == FUSE_RDN_PASS_GRAD_PREPARE || pass == FUSE_RDN_PASS_GRAD_ATROUS) {
        if (x >= F.strataW || y >= F.strataH) {
            return;
        }
        if (pass == FUSE_RDN_PASS_GRAD_PREPARE) {
            rdnGradPrepare(F, a2, x, y);
        } else {
            rdnGradAtrous(F, a0, a2, stp, x, y);
        }
        return;
    }
    if (pass == FUSE_RDN_PASS_MIP) {
        if (x >= rdnMipW(F, stp) || y >= rdnMipH(F, stp)) {
            return;
        }
        rdnMip(F, a0, a2, stp, x, y);
        return;
    }
    if (x >= F.width || y >= F.height) {
        return;
    }
    if (pass == FUSE_RDN_PASS_PREPARE) {
        rdnPrepare(F, x, y);
    } else if (pass == FUSE_RDN_PASS_PREBLUR) {
        rdnPreblur(F, x, y);
    } else if (pass == FUSE_RDN_PASS_TEMPORAL) {
        rdnTemporal(F, x, y);
    } else if (pass == FUSE_RDN_PASS_HISTFIX) {
        rdnHistoryFix(F, x, y);
    } else if (pass == FUSE_RDN_PASS_VARIANCE) {
        rdnVariance(F, x, y);
    } else if (pass == FUSE_RDN_PASS_ATROUS) {
        rdnAtrous(F, a0, a1, a2, a3, a4, a5, stp, x, y);
    }
}
