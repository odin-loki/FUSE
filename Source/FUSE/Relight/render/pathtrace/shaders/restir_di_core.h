// FUSE Relight RL-5.2: ReSTIR DI for the path tracer's G-buffer vertex (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.3).
// FUSE's own code, written from the papers below (no RTXDI SDK or bridge source was opened):
//   [ReSTIR]  B. Bitterli, C. Wyman, M. Pharr, P. Shirley, A. Lefohn, W. Jarosz. "Spatiotemporal reservoir resampling
//             for real-time ray tracing with dynamic direct lighting". ACM TOG 39(4), 2020.
//   [Tiles]   C. Wyman, A. Panteleev. "Rearchitecting Spatiotemporal Resampling for Production". HPG 2021 (light
//             presampling into tiles that pixels draw candidates from).
//   [GRIS]    D. Lin, M. Kettunen, B. Bitterli, J. Pantaleoni, C. Yuksel, C. Wyman. "Generalized Resampled Importance
//             Sampling: Foundations of ReSTIR". ACM TOG 41(4), 2022 (unbiased contribution weights, pairwise MIS with
//             confidence weights, shift mappings).
// The Renderer's WP-7.2 kernel (restir_kernel.hpp) solves the same problem for a Lambert G-buffer over the WP-7.1 tree;
// this core follows its conventions (reservoir = sample + W + confidence M, visibility reuse, M capping, the unbiased /
// biased split) on the path tracer's own surfaces, BSDFs (RL-4.3) and light set (RL-4.4) - which WP-7.2's integrand
// does not model, so the math is restated here in the path tracer's single-source dialect.
//
// SINGLE SOURCE, compiled after pt_reference_core.h by the C++ runner (src/restir_di_cpp.hpp), Slang
// (shaders/restir_di.slang) and GLSL (shaders/restir_di.comp) with the pt dialect macros plus
//   RDI_CTX_PARAM / RDI_CTX_ARG   this core's context (C++ `const RdiCpuContext& rc,` / `rc,`; shaders empty)
//   RDI_PT                        the pt context argument inside it (C++ `*rc.pt,`; shaders empty)
//   RDI_LUT                       the BSDF table argument (C++ `rc.pt->lut,`; shaders empty)
//   RDI_PARAM_WORDS(name)         float4[kRdiParamWords]
// and the accessors
//   float4 rdiSurfaceWord(RDI_CTX_PARAM uint slot, uint pixel, uint k)    slot 0: this frame, 1: previous frame
//   float4 rdiReservoirWord(RDI_CTX_PARAM uint which, uint pixel, uint k) which 0: the stage's input, 1: history
//   uint   rdiTileLight(RDI_CTX_PARAM uint entry)                          presampled tiles (tileCount x tileSize)
//   float  rdiTilePmf(RDI_CTX_PARAM uint light)                            the tiles' exact per-entry pmf
//   float  rdiCdf(RDI_CTX_PARAM uint light)                                the tiles' CDF (float, last = 1)
//   float  rdiTreePmf(RDI_CTX_PARAM float3 p, float3 n, uint light)        WP-7.1 tree selection pmf (ptLightSetPdf's)
//
// THE INTEGRAND. At the G-buffer vertex x (the path tracer's primary surface after PSR) with outgoing wo, direct light
// is the integral over the light set of F(y) = f(wo, wi) |cos| Le(y -> x) G(x, y) V(x, y), y a point on an emitter
// (area measure, G = |cos_y| / |x - y|^2) or, for distant lights, a direction (solid angle; delta distant lights:
// counting measure, G = 1). F uses the path tracer's own NEE rules (bsdfEval, the opaque geometric-side test, the
// same shadow ray), so the replaced estimate integrates exactly what NEE + MIS integrate. Le is evaluated at the
// POINT y (sphere / cylinder: y must face x - these emitters are convex, so a facing point is the first hit; planar:
// front side or two-sided; shaping from the light's centre as rlLightEval), so a sample moves between receivers by
// the identity shift in area measure (Jacobian 1) [GRIS §6]. Target p-hat = luminance(F) (unshadowed, or x V in the
// unbiased reuse).
//
// STAGES (per frame, one pixel per item; each reads only the previous stage's buffers):
//   presample  [Tiles] tileCount x tileSize light indices drawn from the power CDF (host: luminance(Le) x area; distant
//              lights x a fixed area); the pmf used below is the CDF's exact 24-bit bin width (rdiTilePmf), so the
//              tile entries are distributed exactly as the MIS weights assume.
//   surface    the path tracer's own primary chain (ptRenderSample with kPtFlagDiRecord, NEE off: the chain does not
//              depend on it) stops at the G-buffer vertex: (instance, primitive, barycentrics), direction, position,
//              distance, normal -> the surface record.
//   initial    RIS [ReSTIR Alg. 3] over tileCandidates entries of the pixel's tile (one tile per 8x8 block and
//              frame), treeCandidates light-tree samples and bsdfCandidates BSDF rays (each emitter a ray reaches first
//              is a candidate: the geometry hit's emissive triangle, analytic lights before it, distant lights when it
//              escapes), combined by the balance heuristic over the three strategies: w = p-hat(y) / sum_s N_s p_s(y)
//              (all in the sample's measure), W = sum w / p-hat(Y), M = 1; visibility reuse (W = 0 when Y is
//              occluded). The BSDF strategy bounds the weights of near emitters, where light sampling's 1 / d^2 does
//              not (without it the estimator is unbiased but heavy-tailed: sample means of a few hundred frames sit
//              ~1% low next to the Cornell ceiling panel).
//   temporal   the canonical reservoir + the previous frame's final reservoir at the pixel the surface reprojects to
//              (previous camera), when that surface is similar (normal, distance); history M capped at
//              maxHistory x the canonical M.
//   spatial    the canonical reservoir + up to spatialSamples neighbours in a disk of `radius` pixels (similar
//              surfaces); iterations ping-pong.
//   combine    unbiased: [GRIS] pairwise MIS with confidence weights, every p-hat visibility-tested in its own domain:
//                with k neighbours, c = M_c / k and f_i = (M_i + c) / (M_c + sum M_j),
//                  neighbour i's sample  m_i = f_i M_i p_i(y_i) / (M_i p_i(y_i) + c p_c(y_i))
//                  canonical sample      m_c = sum_i f_i c p_c(y_c) / (M_i p_i(y_c) + c p_c(y_c))   (k = 0: 1)
//              - a partition of unity wherever p_c > 0 - then w = m p_c(y) W, W_out = sum w / p_c(Y);
//              fast (biased): [ReSTIR Alg. 4] w = p_c(y) W M without visibility, W_out = sum w / (p_c(Y) sum M).
//   shade      direct = F(Y) W with the shadow ray (the canonical surface); the final reservoir becomes the history.
// The path tracer's trace pass (kPtFlagRestirDi) takes `direct` at the vertex whose (instance, primitive,
// barycentrics) match the surface record of the frame's first sample, skips its NEE there and gives light-set
// emitters found by that vertex's non-dirac continuation MIS weight 0: the path estimator stays unbiased whenever the
// ReSTIR estimate is (unbiased mode).
//
// PACKED RECORDS (float4 words; integers as exact floats):
//   params     w0 = (flags, tileCount, tileSize, tileCandidates), w1 = (treeCandidates, spatialSamples, iteration,
//              bsdfCandidates),
//              w2 = (radius, maxHistory, normalThreshold, depthThreshold), w3 = 0
//   surface    w0 = (instance, primitive, u, v) (instance -1: none), w1 = (incoming direction, valid),
//              w2 = (position, |position - eye|: the similarity tests' distance), w3 = (shading normal facing wo, 0)
//   reservoir  w0 = (y, W), w1 = (light or -1, M, p-hat at the owner, kind)
//   output     w0 = the surface record's w0 (the trace pass's key), w1 = (direct, valid)

PT_CONST uint kRdiParamWords = 4u;
PT_CONST uint kRdiSurfaceWords = 4u;
PT_CONST uint kRdiReservoirWords = 2u;
PT_CONST uint kRdiOutWords = 2u;
PT_CONST uint kRdiMaxNeighbors = 8u;

PT_CONST uint kRdiFlagUnbiased = 1u;
PT_CONST uint kRdiFlagHistory = 2u;           ///< the previous frame's surfaces / final reservoirs are valid
PT_CONST uint kRdiFlagInitialVisibility = 4u; ///< visibility reuse in the initial stage
PT_CONST uint kRdiFlagTemporal = 8u;

PT_CONST uint kRdiKindPoint = 0u;
PT_CONST uint kRdiKindDirection = 1u;

struct RdiParams {
    uint flags;
    uint tileCount;
    uint tileSize;
    uint tileCandidates;
    uint treeCandidates;
    uint spatialSamples;
    uint iteration;
    uint bsdfCandidates;
    float radius;
    float maxHistory;
    float normalThreshold;
    float depthThreshold;
};

struct RdiReservoir {
    float3 y;     ///< point on the emitter (kind point) or unit direction to it (kind direction)
    float W;      ///< unbiased contribution weight
    uint light;   ///< kPtInvalid: no sample
    float M;      ///< confidence (0: no surface)
    float phat;   ///< target at the owner when chosen (diagnostic)
    uint kind;
};

/// A surface record reloaded into the path tracer's shading frame.
struct RdiVertex {
    bool valid;
    PtSurface S;
    float3 pos;
    float3 n;     ///< shading normal of the frame (opaque: flipped to the incoming side)
    float3 ng;
    float3 tx;
    float3 ty;
    float3 wo;    ///< local
    float3 treeN;
    float depth;
};

PT_FN uint rdiMinU(uint a, uint b) { return b < a ? b : a; }

PT_FN RdiParams rdiParamsUnpack(RDI_PARAM_WORDS(w)) {
    RdiParams R;
    R.flags = uint(w[0].x);
    R.tileCount = uint(w[0].y);
    R.tileSize = uint(w[0].z);
    R.tileCandidates = uint(w[0].w);
    R.treeCandidates = uint(w[1].x);
    R.spatialSamples = uint(w[1].y);
    R.iteration = uint(w[1].z);
    R.bsdfCandidates = uint(w[1].w);
    R.radius = w[2].x;
    R.maxHistory = w[2].y;
    R.normalThreshold = w[2].z;
    R.depthThreshold = w[2].w;
    return R;
}

PT_FN RdiReservoir rdiEmpty() {
    RdiReservoir r;
    r.y = float3(0.0f, 0.0f, 0.0f);
    r.W = 0.0f;
    r.light = kPtInvalid;
    r.M = 0.0f;
    r.phat = 0.0f;
    r.kind = kRdiKindPoint;
    return r;
}

PT_FN RdiReservoir rdiLoadReservoir(RDI_CTX_PARAM uint which, uint pixel) {
    float4 a = rdiReservoirWord(RDI_CTX_ARG which, pixel, 0u);
    float4 b = rdiReservoirWord(RDI_CTX_ARG which, pixel, 1u);
    RdiReservoir r;
    r.y = float3(a.x, a.y, a.z);
    r.W = a.w;
    r.light = b.x >= 0.0f ? uint(b.x) : kPtInvalid;
    r.M = b.y;
    r.phat = b.z;
    r.kind = uint(b.w);
    return r;
}

PT_FN float4 rdiReservoirWord0(RdiReservoir r) { return float4(r.y.x, r.y.y, r.y.z, r.W); }
PT_FN float4 rdiReservoirWord1(RdiReservoir r) {
    return float4(r.light == kPtInvalid ? -1.0f : float(r.light), r.M, r.phat, float(r.kind));
}

/// The shading frame of hit h reached along d (as ptRenderSample builds it).
PT_FN RdiVertex rdiMakeVertex(RDI_CTX_PARAM PtRawHit h, float3 d) {
    RdiVertex V;
    V.valid = true;
    V.S = ptLoadSurface(RDI_PT h);
    V.pos = V.S.position;
    V.n = V.S.normal;
    V.ng = V.S.geoNormal;
    if (V.S.m.model == kBsdfModelOpaque && dot(V.ng, d) > 0.0f) {
        V.n = -V.n;
        V.ng = -V.ng;
    }
    ptBasis(V.n, V.tx, V.ty);
    float3 woW = -d;
    V.wo = float3(dot(woW, V.tx), dot(woW, V.ty), dot(woW, V.n));
    V.treeN = ptTreeNormal(V.S, V.n);
    V.depth = 0.0f;
    return V;
}

PT_FN RdiVertex rdiLoadVertex(RDI_CTX_PARAM uint slot, uint pixel) {
    float4 w0 = rdiSurfaceWord(RDI_CTX_ARG slot, pixel, 0u);
    float4 w1 = rdiSurfaceWord(RDI_CTX_ARG slot, pixel, 1u);
    PtRawHit h;
    h.hit = true;
    h.t = 0.0f;
    h.instance = w0.x >= 0.0f ? uint(w0.x) : 0u;
    h.primitive = uint(w0.y);
    h.u = w0.z;
    h.v = w0.w;
    RdiVertex V = rdiMakeVertex(RDI_CTX_ARG h, float3(w1.x, w1.y, w1.z));
    V.valid = w1.w > 0.5f && w0.x >= 0.0f;
    V.depth = rdiSurfaceWord(RDI_CTX_ARG slot, pixel, 2u).w;
    return V;
}

/// F(y) at V in the sample's measure (see THE INTEGRAND), its geometry factor g; with `vis` the shadow ray too.
PT_FN float3 rdiEval(RDI_CTX_PARAM PtParams P, RdiVertex V, uint light, float3 y, uint kind, bool vis,
                     PT_OUT(float) g) {
    float3 zero = float3(0.0f, 0.0f, 0.0f);
    g = 0.0f;
    if (!V.valid || light >= P.lightCount) {
        return zero;
    }
    RlLight L = ptLoadLight(RDI_PT light);
    float3 wi = float3(0.0f, 0.0f, 1.0f);
    float3 le = zero;
    if (kind == kRdiKindDirection) {
        if (L.kind != kRlKindDistant) {
            return zero;
        }
        wi = y;
        if (L.area > 0.0f) {
            le = PT_B3(rlLightEval(L, PT_L3(V.pos), PT_L3(wi)));
        } else {
            le = PT_B3(L.radiance);
        }
        g = 1.0f;
    } else {
        if (L.kind == kRlKindDistant) {
            return zero;
        }
        float3 v = y - V.pos;
        float d2 = dot(v, v);
        if (!(d2 > 1e-12f)) {
            return zero;
        }
        float dist = sqrt(d2);
        wi = v * (1.0f / dist);
        float3 lc = PT_B3(L.position);
        float3 ny = float3(0.0f, 0.0f, 1.0f);
        bool twoSided = false;
        if (L.kind == kRlKindSphere) {
            float3 dc = V.pos - lc;
            if (dot(dc, dc) <= L.radius * L.radius) {
                return zero;
            }
            ny = ptSafeNormalize(y - lc, -wi);
        } else if (L.kind == kRlKindRect || L.kind == kRlKindDisk || L.kind == kRlKindTriangle) {
            ny = PT_B3(rlPlanarNormal(L));
            twoSided = (L.flags & kRlFlagTwoSided) != 0u;
        } else if (L.kind == kRlKindCylinder) {
            float3 ax = ptSafeNormalize(PT_B3(L.u), float3(0.0f, 0.0f, 1.0f));
            float3 q = y - lc;
            ny = ptSafeNormalize(q - ax * dot(q, ax), -wi);
        } else {
            return zero;
        }
        float cosL = -dot(ny, wi);
        if (!(cosL > 0.0f) && !twoSided) {
            return zero;
        }
        le = PT_B3(L.radiance) * rlShapingAt(L, PT_L3(V.pos));
        g = abs(cosL) / d2;
    }
    if (!(ptMax3(le) > 0.0f) || !(g > 0.0f)) {
        return zero;
    }
    float3 wiL = float3(dot(wi, V.tx), dot(wi, V.ty), dot(wi, V.n));
    if (V.S.m.model == kBsdfModelOpaque && (V.S.m.flags & kBsdfFlagSssThin) == 0u) {
        if (!(dot(wi, V.ng) > 0.0f && wiL.z > 0.0f)) {
            return zero;
        }
    }
    float3 f = bsdfEval(RDI_LUT V.S.m, V.wo, wiL);
    float3 F = f * le * g;
    if (!(ptMax3(F) > 0.0f)) {
        return zero;
    }
    if (vis) {
        // The path tracer's NEE shadow ray (pt_reference_core.h): offset origin, aimed at the sampled point.
        float3 so = ptOffset(V.pos, V.ng, wi, P.rayEps);
        float3 sd = wi;
        float tmax = kPtFar;
        if (kind == kRdiKindPoint) {
            float3 toL = y - so;
            float len = sqrt(dot(toL, toL));
            sd = len > 0.0f ? toL * (1.0f / len) : wi;
            tmax = len * (1.0f - 1e-4f) - P.rayEps;
        }
        if (!(tmax > 0.0f) || !ptVisible(RDI_PT P, so, sd, tmax)) {
            return zero;
        }
    }
    return F;
}

PT_FN float rdiTarget(RDI_CTX_PARAM PtParams P, RdiVertex V, RdiReservoir r, bool vis) {
    if (r.light == kPtInvalid) {
        return 0.0f;
    }
    float g = 0.0f;
    return ptLum(rdiEval(RDI_CTX_ARG P, V, r.light, r.y, r.kind, vis, g));
}

/// A light sample of `light` at V for (u1, u2): the point / direction and the light's own density (solid angle;
/// delta: 1).
PT_FN bool rdiCandidate(RDI_CTX_PARAM RdiVertex V, uint light, float u1, float u2, PT_OUT(float3) y, PT_OUT(uint) kind,
                        PT_OUT(float) pdfSa) {
    RlLight L = ptLoadLight(RDI_PT light);
    RlLightSample s = rlLightSample(L, PT_L3(V.pos), u1, u2);
    y = float3(0.0f, 0.0f, 0.0f);
    kind = kRdiKindPoint;
    pdfSa = 0.0f;
    if ((s.flags & kRlSampleValid) == 0u) {
        return false;
    }
    if (L.kind == kRlKindDistant) {
        y = PT_B3(s.wi);
        kind = kRdiKindDirection;
        pdfSa = (s.flags & kRlSampleDelta) != 0u ? 1.0f : s.pdf;
    } else {
        y = V.pos + PT_B3(s.wi) * s.dist;
        pdfSa = s.pdf;
    }
    return pdfSa > 0.0f;
}

// ---- presample -------------------------------------------------------------------------------------------------

/// Tile entry `entry`: the first light whose CDF exceeds a 24-bit uniform.
PT_FN uint rdiPresample(RDI_CTX_PARAM PtParams P, uint entry) {
    if (P.lightCount == 0u) {
        return 0u;
    }
    uint seed = ptSeed(entry, 0x7113u, P.frameSeed, P.sampleBase);
    float u = ptRandom(seed, 0u);
    uint lo = 0u;
    uint hi = P.lightCount - 1u;
    while (lo < hi) {
        uint mid = (lo + hi) >> 1u;
        if (u < rdiCdf(RDI_CTX_ARG mid)) {
            hi = mid;
        } else {
            lo = mid + 1u;
        }
    }
    return lo;
}

// ---- surface ---------------------------------------------------------------------------------------------------

/// The surface record of the G-buffer vertex (hit h along d); `have` false: no vertex (sky, unlit, ...).
PT_FN void rdiSurfaceWords(RDI_CTX_PARAM PtParams P, bool have, PtRawHit h, float3 d, PT_OUT(float4) w0,
                           PT_OUT(float4) w1, PT_OUT(float4) w2, PT_OUT(float4) w3) {
    w0 = float4(-1.0f, 0.0f, 0.0f, 0.0f);
    w1 = float4(0.0f, 0.0f, 0.0f, 0.0f);
    w2 = float4(0.0f, 0.0f, 0.0f, 0.0f);
    w3 = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (!have) {
        return;
    }
    RdiVertex V = rdiMakeVertex(RDI_CTX_ARG h, d);
    float3 e = V.pos - P.camOrigin;
    w0 = float4(float(h.instance), float(h.primitive), h.u, h.v);
    w1 = float4(d.x, d.y, d.z, 1.0f);
    w2 = float4(V.pos.x, V.pos.y, V.pos.z, sqrt(dot(e, e)));
    w3 = float4(V.n.x, V.n.y, V.n.z, 0.0f);
}

// ---- initial ---------------------------------------------------------------------------------------------------

/// The balance-heuristic mixture density sum_s N_s p_s of sample (light, y) at V in the sample's measure: tiles and
/// tree (selection pmf x the light's own solid-angle density pdfSa) and BSDF sampling (bsdfPdf; not for delta
/// lights, which only light sampling reaches), all x the geometry factor g.
PT_FN float rdiMixPdf(RDI_CTX_PARAM RdiParams R, RdiVertex V, uint nT, uint light, float3 y, uint kind, float pdfSa,
                      bool delta, float g) {
    float sel = float(nT) * rdiTilePmf(RDI_CTX_ARG light) +
                float(R.treeCandidates) * rdiTreePmf(RDI_CTX_ARG V.pos, V.treeN, light);
    float dens = sel * pdfSa * g;
    if (R.bsdfCandidates > 0u && !delta) {
        float3 wi = kind == kRdiKindDirection ? y : ptSafeNormalize(y - V.pos, V.n);
        float3 wiL = float3(dot(wi, V.tx), dot(wi, V.ty), dot(wi, V.n));
        dens = dens + float(R.bsdfCandidates) * bsdfPdf(RDI_LUT V.S.m, V.wo, wiL) * g;
    }
    return dens;
}

/// One RIS candidate (streaming, [ReSTIR Alg. 2]): weight p-hat / mixture.
PT_FN void rdiOffer(RDI_CTX_PARAM PtParams P, RdiParams R, RdiVertex V, uint nT, uint light, float3 y, uint kind,
                    float pdfSa, bool delta, float u, PT_INOUT(RdiReservoir) r, PT_INOUT(float) wSum,
                    PT_INOUT(float) pSel) {
    float g = 0.0f;
    float ph = ptLum(rdiEval(RDI_CTX_ARG P, V, light, y, kind, false, g));
    if (!(ph > 0.0f)) {
        return;
    }
    float src = rdiMixPdf(RDI_CTX_ARG R, V, nT, light, y, kind, pdfSa, delta, g);
    if (!(src > 0.0f)) {
        return;
    }
    float w = ph / src;
    wSum = wSum + w;
    if (u * wSum < w) {
        r.y = y;
        r.kind = kind;
        r.light = light;
        pSel = ph;
    }
}

PT_FN RdiReservoir rdiInitial(RDI_CTX_PARAM PtParams P, RdiParams R, uint px, uint py) {
    RdiReservoir r = rdiEmpty();
    RdiVertex V = rdiLoadVertex(RDI_CTX_ARG 0u, py * P.width + px);
    if (!V.valid) {
        return r;
    }
    r.M = 1.0f;
    uint seed = ptSeed(px, py, P.frameSeed ^ 0x52444901u, P.sampleBase);
    uint nT = R.tileCount > 0u && R.tileSize > 0u ? R.tileCandidates : 0u;
    uint nB = R.treeCandidates;
    uint tile = 0u;
    if (nT > 0u) {
        tile = ptHash((px >> 3u) ^ ptHash((py >> 3u) ^ ptHash(P.frameSeed ^ ptHash(P.sampleBase ^ 0x7E1Eu)))) %
               R.tileCount;
    }
    float wSum = 0.0f;
    float pSel = 0.0f;
    // Light sampling: tile entries, then light-tree samples.
    for (uint j = 0u; j < nT + nB; ++j) {
        uint dim = 4u * j;
        float u1 = ptRandom(seed, dim + 1u);
        float u2 = ptRandom(seed, dim + 2u);
        uint light = kPtInvalid;
        if (j < nT) {
            uint e = rdiMinU(uint(ptRandom(seed, dim) * float(R.tileSize)), R.tileSize - 1u);
            light = rdiTileLight(RDI_CTX_ARG tile * R.tileSize + e);
        } else {
            PtLightPick lp = ptSampleLightSet(RDI_PT V.pos, V.treeN, ptRandom(seed, dim), u1, u2);
            light = lp.light;
        }
        if (light >= P.lightCount) {
            continue;
        }
        float3 y;
        uint kind;
        float pdfSa;
        if (!rdiCandidate(RDI_CTX_ARG V, light, u1, u2, y, kind, pdfSa)) {
            continue;
        }
        RlLight L = ptLoadLight(RDI_PT light);
        bool delta = L.kind == kRlKindDistant && !(L.area > 0.0f);
        rdiOffer(RDI_CTX_ARG P, R, V, nT, light, y, kind, pdfSa, delta, ptRandom(seed, dim + 3u), r, wSum, pSel);
    }
    // BSDF sampling: every light-set emitter the ray reaches first (the geometry hit's emissive triangle, analytic
    // lights before it, distant lights when it escapes) is a candidate; near emitters, where light sampling's
    // 1 / distance^2 has no bounded weight, are sampled well here (as NEE + MIS does in the path tracer).
    for (uint j = 0u; j < R.bsdfCandidates; ++j) {
        uint dim = 256u + 8u * j;
        float4 u4 = float4(ptRandom(seed, dim), ptRandom(seed, dim + 1u), ptRandom(seed, dim + 2u),
                           ptRandom(seed, dim + 3u));
        BsdfSample bs = bsdfSample(RDI_LUT V.S.m, V.wo, u4);
        uint lobe = bs.flags >> kBsdfLobeShift;
        if ((bs.flags & kBsdfSampleValid) == 0u || (bs.flags & (kBsdfSampleDelta | kBsdfSampleSss)) != 0u ||
            !(bs.pdf > 0.0f)) {
            continue;
        }
        float3 wiW = V.tx * bs.wi.x + V.ty * bs.wi.y + V.n * bs.wi.z;
        if (V.S.m.model == kBsdfModelOpaque && (V.S.m.flags & kBsdfFlagSssThin) == 0u && lobe != kBsdfLobeOpacity &&
            dot(wiW, V.ng) <= 0.0f) {
            continue;
        }
        float3 o = ptOffset(V.pos, V.ng, wiW, P.rayEps);
        PtRawHit h = ptTraceScene(RDI_PT P, o, wiW, 0.0f, kPtFar, kPtMaskVisible);
        float tHit = h.hit ? h.t : kPtFar;
        uint seedB = ptHash(seed ^ (0xB5D10000u + j));
        if (h.hit) {
            PtSurface S2 = ptLoadSurface(RDI_PT h);
            if (S2.light != kPtInvalid && S2.light < P.lightCount) {
                RlLight L2 = ptLoadLight(RDI_PT S2.light);
                rdiOffer(RDI_CTX_ARG P, R, V, nT, S2.light, S2.position, kRdiKindPoint,
                         rlLightPdf(L2, PT_L3(V.pos), PT_L3(ptSafeNormalize(S2.position - V.pos, wiW))), false,
                         ptRandom(seedB, 0u), r, wSum, pSel);
            }
        }
        for (uint i = 0u; i < P.analyticCount && i < P.lightCount; ++i) {
            RlLight L = ptLoadLight(RDI_PT i);
            if (L.kind == kRlKindDistant) {
                if (!h.hit && L.area > 0.0f) {
                    rdiOffer(RDI_CTX_ARG P, R, V, nT, i, wiW, kRdiKindDirection, rlLightPdf(L, PT_L3(V.pos), PT_L3(wiW)),
                             false, ptRandom(seedB, 1u + i), r, wSum, pSel);
                }
                continue;
            }
            float t = ptAnalyticDistance(L, o, wiW);
            if (t < tHit) {
                float3 yl = o + wiW * t;
                rdiOffer(RDI_CTX_ARG P, R, V, nT, i, yl, kRdiKindPoint,
                         rlLightPdf(L, PT_L3(V.pos), PT_L3(ptSafeNormalize(yl - V.pos, wiW))), false,
                         ptRandom(seedB, 1u + i), r, wSum, pSel);
            }
        }
    }
    if (r.light != kPtInvalid) {
        r.W = wSum / pSel;
        r.phat = pSel;
        if ((R.flags & kRdiFlagInitialVisibility) != 0u && !(rdiTarget(RDI_CTX_ARG P, V, r, true) > 0.0f)) {
            r.W = 0.0f;
        }
    }
    return r;
}

// ---- reuse -----------------------------------------------------------------------------------------------------

/// Neighbour surface q of `slot` usable for reuse (geometry only: normal and distance similarity).
PT_FN bool rdiSimilar(RDI_CTX_PARAM RdiParams R, uint slot, uint q, float3 n, float depth) {
    float4 w0 = rdiSurfaceWord(RDI_CTX_ARG slot, q, 0u);
    float4 w1 = rdiSurfaceWord(RDI_CTX_ARG slot, q, 1u);
    if (!(w1.w > 0.5f) || w0.x < 0.0f) {
        return false;
    }
    float4 w2 = rdiSurfaceWord(RDI_CTX_ARG slot, q, 2u);
    float4 w3 = rdiSurfaceWord(RDI_CTX_ARG slot, q, 3u);
    if (!(dot(n, float3(w3.x, w3.y, w3.z)) >= R.normalThreshold)) {
        return false;
    }
    return abs(w2.w - depth) <= R.depthThreshold * depth;
}

/// Temporal (canonical + reprojected history) or spatial (canonical + disk neighbours) reuse at pixel (px, py).
PT_FN RdiReservoir rdiReuse(RDI_CTX_PARAM PtParams P, RdiParams R, uint px, uint py, bool temporal) {
    uint pixel = py * P.width + px;
    RdiReservoir canon = rdiLoadReservoir(RDI_CTX_ARG 0u, pixel);
    RdiVertex V = rdiLoadVertex(RDI_CTX_ARG 0u, pixel);
    if (!V.valid) {
        return canon;
    }
    uint seed = ptSeed(px, py, P.frameSeed ^ (temporal ? 0x52444954u : (0x52444960u + R.iteration)), P.sampleBase);
    uint qs[8];
    for (uint z = 0u; z < 8u; ++z) {
        qs[z] = 0u;
    }
    uint k = 0u;
    uint slotN = temporal ? 1u : 0u;
    uint whichN = temporal ? 1u : 0u;
    if (temporal) {
        if ((R.flags & (kRdiFlagHistory | kRdiFlagTemporal)) == (kRdiFlagHistory | kRdiFlagTemporal)) {
            float ux = 0.0f;
            float uy = 0.0f;
            if (ptProject(P.prevOrigin, P.prevRight, P.prevUp, P.prevForward, V.pos, ux, uy) && ux >= 0.0f &&
                ux < 1.0f && uy >= 0.0f && uy < 1.0f) {
                uint qx = rdiMinU(uint(ux * float(P.width)), P.width - 1u);
                uint qy = rdiMinU(uint(uy * float(P.height)), P.height - 1u);
                uint q = qy * P.width + qx;
                if (rdiSimilar(RDI_CTX_ARG R, 1u, q, V.n, V.depth)) {
                    qs[0] = q;
                    k = 1u;
                }
            }
        }
    } else {
        uint count = rdiMinU(R.spatialSamples, kRdiMaxNeighbors);
        for (uint j = 0u; j < count; ++j) {
            float ox = 0.0f;
            float oy = 0.0f;
            rlConcentric(ptRandom(seed, 2u * j), ptRandom(seed, 2u * j + 1u), ox, oy);
            int dx = int(ox * R.radius);
            int dy = int(oy * R.radius);
            int qx = int(px) + dx;
            int qy = int(py) + dy;
            if ((dx == 0 && dy == 0) || qx < 0 || qy < 0 || qx >= int(P.width) || qy >= int(P.height)) {
                continue;
            }
            uint q = uint(qy) * P.width + uint(qx);
            if (rdiSimilar(RDI_CTX_ARG R, 0u, q, V.n, V.depth)) {
                qs[k] = q;
                k = k + 1u;
            }
        }
    }
    bool unbiased = (R.flags & kRdiFlagUnbiased) != 0u;
    float mc = canon.M;
    float mTot = mc;
    for (uint i = 0u; i < k; ++i) {
        float mi = rdiLoadReservoir(RDI_CTX_ARG whichN, qs[i]).M;
        if (temporal) {
            mi = min(mi, R.maxHistory * mc);
        }
        mTot = mTot + mi;
    }
    RdiReservoir res = rdiEmpty();
    res.M = mTot;
    float wSum = 0.0f;
    float pSel = 0.0f;
    uint dimSel = 64u;
    float pcc = 0.0f;
    if (canon.light != kPtInvalid && canon.W > 0.0f) {
        pcc = rdiTarget(RDI_CTX_ARG P, V, canon, unbiased);
    }
    float c = k > 0u ? mc / float(k) : mc;
    float mcWeight = k == 0u ? 1.0f : 0.0f;
    for (uint i = 0u; i < k; ++i) {
        RdiReservoir ri = rdiLoadReservoir(RDI_CTX_ARG whichN, qs[i]);
        float mi = ri.M;
        if (temporal) {
            mi = min(mi, R.maxHistory * mc);
        }
        bool live = ri.light != kPtInvalid && ri.W > 0.0f;
        if (!unbiased) {
            if (live) {
                float pci = rdiTarget(RDI_CTX_ARG P, V, ri, false);
                float w = pci * ri.W * mi;
                if (w > 0.0f) {
                    wSum = wSum + w;
                    if (ptRandom(seed, dimSel + i) * wSum < w) {
                        res.y = ri.y;
                        res.kind = ri.kind;
                        res.light = ri.light;
                        pSel = pci;
                    }
                }
            }
            continue;
        }
        RdiVertex Vi = rdiLoadVertex(RDI_CTX_ARG slotN, qs[i]);
        float fi = (mi + c) / mTot;
        if (pcc > 0.0f) {
            float pic = rdiTarget(RDI_CTX_ARG P, Vi, canon, true);
            mcWeight = mcWeight + fi * (c * pcc) / (mi * pic + c * pcc);
        }
        if (live) {
            float pci = rdiTarget(RDI_CTX_ARG P, V, ri, true);
            if (pci > 0.0f) {
                float pii = rdiTarget(RDI_CTX_ARG P, Vi, ri, true);
                float m = fi * (mi * pii) / (mi * pii + c * pci);
                float w = m * pci * ri.W;
                if (w > 0.0f) {
                    wSum = wSum + w;
                    if (ptRandom(seed, dimSel + i) * wSum < w) {
                        res.y = ri.y;
                        res.kind = ri.kind;
                        res.light = ri.light;
                        pSel = pci;
                    }
                }
            }
        }
    }
    if (pcc > 0.0f) {
        float w = unbiased ? mcWeight * pcc * canon.W : pcc * canon.W * mc;
        if (w > 0.0f) {
            wSum = wSum + w;
            if (ptRandom(seed, dimSel + kRdiMaxNeighbors) * wSum < w) {
                res.y = canon.y;
                res.kind = canon.kind;
                res.light = canon.light;
                pSel = pcc;
            }
        }
    }
    if (res.light != kPtInvalid && pSel > 0.0f) {
        res.W = unbiased ? wSum / pSel : wSum / (pSel * mTot);
        res.phat = pSel;
    } else {
        res.light = kPtInvalid;
    }
    return res;
}

// ---- shade -----------------------------------------------------------------------------------------------------

/// direct = F(Y) W at the canonical surface (w = 1: a ReSTIR vertex, 0: no surface).
PT_FN float4 rdiShade(RDI_CTX_PARAM PtParams P, RdiReservoir r, uint pixel) {
    RdiVertex V = rdiLoadVertex(RDI_CTX_ARG 0u, pixel);
    if (!V.valid) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    float3 direct = float3(0.0f, 0.0f, 0.0f);
    if (r.light != kPtInvalid && r.W > 0.0f) {
        float g = 0.0f;
        direct = rdiEval(RDI_CTX_ARG P, V, r.light, r.y, r.kind, true, g) * r.W;
    }
    return float4(direct.x, direct.y, direct.z, 1.0f);
}
