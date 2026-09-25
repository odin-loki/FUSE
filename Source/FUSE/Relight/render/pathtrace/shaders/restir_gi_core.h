// FUSE Relight RL-5.3: ReSTIR GI for the path tracer's G-buffer vertex (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.4).
// FUSE's own code, written from the papers below (no RTXDI SDK, bridge or Remix shader source was opened):
//   [GI]    Y. Ouyang, S. Liu, M. Kettunen, M. Pharr, J. Pantaleoni. "ReSTIR GI: Path Resampling for Real-Time Path
//           Tracing". HPG 2021 / CGF 40(8) (secondary-vertex reservoirs from the first bounce, temporal and spatial
//           reuse, the reconnection Jacobian).
//   [GRIS]  D. Lin, M. Kettunen, B. Bitterli, J. Pantaleoni, C. Yuksel, C. Wyman. "Generalized Resampled Importance
//           Sampling: Foundations of ReSTIR". ACM TOG 41(4), 2022 (unbiased contribution weights, pairwise MIS with
//           confidence weights, the reconnection shift and its Jacobian).
// Same structure as RL-5.2's ReSTIR DI (restir_di_core.h): surface records, reservoir = sample + W + confidence M,
// temporal and spatial reuse, unbiased (visibility-tested pairwise MIS) and fast (biased) modes.
//
// SINGLE SOURCE, compiled after pt_reference_core.h by the C++ runner (src/restir_gi_cpp.hpp), Slang
// (shaders/restir_gi.slang) and GLSL (shaders/restir_gi.comp) with the pt dialect macros plus
//   RGI_CTX_PARAM / RGI_CTX_ARG   this core's context (C++ `const RgiCpuContext& gc,` / `gc,`; shaders empty)
//   RGI_PT                        the pt context argument inside it (C++ `*gc.pt,`; shaders empty)
//   RGI_LUT                       the BSDF table argument (C++ `gc.pt->lut,`; shaders empty)
//   RGI_PARAM_WORDS(name)         float4[kRgiParamWords]
// and the accessors
//   float4 rgiSurfaceWord(RGI_CTX_PARAM uint slot, uint pixel, uint k)    slot 0: this frame, 1: previous frame
//   float4 rgiReservoirWord(RGI_CTX_PARAM uint which, uint pixel, uint k) which 0: the stage's input, 1: history
//
// THE SPLIT. At the G-buffer vertex x1 (vertex index b1) of the frame's first sample the path tracer (kPtFlagRestirGi,
// pt_reference_core.h) keeps its own NEE (or RL-5.2's DI) and traces its own continuation, but that continuation only
// collects the LIGHT-SET emission of its first segment (MIS-weighted exactly as before) and stops. Everything else the
// continuation would bring - non-light-set emission (unlit / non-light emissive surfaces, the sky), and every vertex
// after the first hit - is the ReSTIR GI integral
//   I(x1) = integral over x2 of f1(wo, x1 -> x2) Lo(x2 -> x1) G(x1, x2) V(x1, x2) dA(x2),
// with Lo the radiance leaving x2 towards x1 as estimated by the path tracer's rules from x2 on (the same vertex
// budget maxBounces - b1 - 1). Both parts are unbiased, so their sum is the path tracer's estimate in expectation.
//
// THE SAMPLE (the reconnection shift of [GRIS §7] at x2). x2 is a surface point (area measure: the reconnection
// Jacobian |cos2'| / d'^2 / (|cos2| / d^2) is folded into the area-measure target, so every shift has Jacobian 1 in
// the stored representation) or, for rays that escape to the sky, a direction (solid angle, Jacobian 1). The tail
// after x2 is stored so that f2 can be re-evaluated for a new x1':
//   Le2          x2's non-light-set emission (direction independent in the path tracer);
//   NEE at x2    direction wy, cY = Le(y) / p_light (visibility traced whatever f2 is: light sampling does not depend
//                on wo) and p_light (0: MIS weight 1 - delta lights, or BSDF rays do not collect lights);
//   continuation direction w3 (non-dirac lobes only), cR = Li_rest(w3) / p(w3 | wo) and up to two light-set emitters
//                that the continuation reaches before scattering again, eK = Le_K / p(w3 | wo) with their light-set
//                densities p_K: the primary-sample-space reconnection keeps w3, whose Jacobian
//                p(w3 | wo) / p(w3 | wo') cancels against the new density, so f2(wo', w3) (cR + ...) is the shifted
//                tail's contribution [GRIS §7.2; Ouyang et al. §4];
//   so Lo(x2 -> x1') = Le2 + f2(wo', wy) cY m(p_light, pb(wo', wy)) + f2(wo', w3) (cR + sum_K eK m(pb(wo', w3), p_K)),
//   every f2 through bsdfEval (projected), pb = bsdfPdf and m = the path tracer's MIS weight (ptMisWeight), all
//   evaluated for the new wo': the MIS between NEE and BSDF sampling at x2 is the path tracer's own, in every domain
//   (without it, NEE-only at glossy x2 is unbiased but heavy-tailed). A third emitter on the same continuation goes to
//   the residual. A shift to an x1' on the other side of x2's surface is undefined (target 0), so the frames and
//   every side test at x2 are those of the original sample.
//   Non-reconnectable parts - a dirac lobe at x1 (glass, mirror, opacity pass-through), a portal or a blended unlit
//   layer at x2 (the path goes on along the incoming direction), a dirac continuation at x2 - are the canonical
//   pixel's RESIDUAL: its own path tracer estimate, added at shade time, never reused.
// Russian roulette in the tail uses the throughput after x2 only (independent of wo), so the tail is shift-invariant.
//
// STAGES (per frame, one pixel per item; each reads only the previous stage's buffers):
//   surface    the path tracer's own primary chain (ptRenderSample with kPtFlagGiRecord, NEE off) stops at the
//              G-buffer vertex: the surface record (+ its vertex index).
//   initial    one BSDF sample at x1 (the path tracer's sampling) traced to x2, the tail as above: M = 1,
//              W = 1 / p_area(x2) (direction: 1 / p_solid angle), + the residual.
//   temporal   the canonical reservoir + the previous frame's final reservoir at the reprojected pixel (similar
//              surface, same vertex index); history M capped at maxHistory x the canonical M.
//   spatial    the canonical reservoir + up to spatialSamples disk neighbours (similar surfaces, same vertex index).
//   combine    as restir_di_core.h: unbiased = [GRIS] pairwise MIS with confidence weights, every target visibility-
//              tested in its own domain; fast = [ReSTIR] M-weighted combination without visibility.
//   shade      indirect = F(Y) W (visibility-tested at the canonical vertex) + the residual; the final reservoir
//              becomes the history.
//
// PACKED RECORDS (float4 words; integers as exact floats):
//   params     w0 = (flags, spatialSamples, iteration, 0), w1 = (radius, maxHistory, normalThreshold, depthThreshold)
//   surface    w0 = (instance, primitive, u, v) (instance -1: none), w1 = (incoming direction, valid),
//              w2 = (position, |position - eye|), w3 = (shading normal facing wo, vertex index b1)
//   reservoir  w0 = x2's (instance, primitive, u, v), w1 = (W, M, sample flags, p-hat at the owner),
//              w2 = (wy | the escape direction, p_light), w3 = (cY, Le2.r), w4 = (w3, Le2.g), w5 = (cR, Le2.b),
//              w6 = (e0, p_0), w7 = (e1, p_1), w8 = (the canonical pixel's residual, 0)
//   output     w0 = the surface record's w0 (the trace pass's key), w1 = (indirect, valid)

PT_CONST uint kRgiParamWords = 2u;
PT_CONST uint kRgiSurfaceWords = 4u;
PT_CONST uint kRgiReservoirWords = 9u;
PT_CONST uint kRgiOutWords = 2u;
PT_CONST uint kRgiMaxNeighbors = 8u;

PT_CONST uint kRgiFlagUnbiased = 1u;
PT_CONST uint kRgiFlagHistory = 2u;  ///< the previous frame's surfaces / final reservoirs are valid
PT_CONST uint kRgiFlagTemporal = 4u;

// Reservoir sample flags.
PT_CONST uint kRgiSampleValid = 1u;
PT_CONST uint kRgiSampleDirection = 2u; ///< an escaped ray (solid angle); else a surface point x2 (area)
PT_CONST uint kRgiSampleNee = 4u;
PT_CONST uint kRgiSampleCont = 8u;
PT_CONST uint kRgiSampleBack = 16u;     ///< x1 -> x2 arrives at the back of x2's geometric normal
PT_CONST uint kRgiSampleEmit0 = 32u;    ///< emitter slot 0 of the continuation holds a light
PT_CONST uint kRgiSampleEmit1 = 64u;

struct RgiParams {
    uint flags;
    uint spatialSamples;
    uint iteration;
    float radius;
    float maxHistory;
    float normalThreshold;
    float depthThreshold;
};

struct RgiReservoir {
    float4 key;   ///< x2 (instance, primitive, u, v)
    float W;      ///< unbiased contribution weight
    float M;      ///< confidence (0: no surface)
    uint flags;   ///< kRgiSample*
    float phat;   ///< target at the owner when chosen (diagnostic)
    float3 a;     ///< wy (NEE at x2) or the escape direction
    float pY;     ///< NEE light-set density (0: MIS weight 1)
    float3 cY;
    float3 w3;
    float3 c3;    ///< cR
    float3 e0;
    float p0;
    float3 e1;
    float p1;
    float3 le;    ///< Le2 (direction: the sky radiance)
    float3 residual;
};

/// A surface record reloaded into the path tracer's shading frame.
struct RgiVertex {
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
    uint bounce;
};

PT_FN uint rgiMinU(uint a, uint b) { return b < a ? b : a; }

PT_FN RgiParams rgiParamsUnpack(RGI_PARAM_WORDS(w)) {
    RgiParams R;
    R.flags = uint(w[0].x);
    R.spatialSamples = uint(w[0].y);
    R.iteration = uint(w[0].z);
    R.radius = w[1].x;
    R.maxHistory = w[1].y;
    R.normalThreshold = w[1].z;
    R.depthThreshold = w[1].w;
    return R;
}

PT_FN RgiReservoir rgiEmpty() {
    RgiReservoir r;
    float3 z = float3(0.0f, 0.0f, 0.0f);
    r.key = float4(-1.0f, 0.0f, 0.0f, 0.0f);
    r.W = 0.0f;
    r.M = 0.0f;
    r.flags = 0u;
    r.phat = 0.0f;
    r.a = z;
    r.pY = 0.0f;
    r.cY = z;
    r.w3 = z;
    r.c3 = z;
    r.e0 = z;
    r.p0 = 0.0f;
    r.e1 = z;
    r.p1 = 0.0f;
    r.le = z;
    r.residual = z;
    return r;
}

PT_FN RgiReservoir rgiLoadReservoir(RGI_CTX_PARAM uint which, uint pixel) {
    float4 w1 = rgiReservoirWord(RGI_CTX_ARG which, pixel, 1u);
    float4 w2 = rgiReservoirWord(RGI_CTX_ARG which, pixel, 2u);
    float4 w3 = rgiReservoirWord(RGI_CTX_ARG which, pixel, 3u);
    float4 w4 = rgiReservoirWord(RGI_CTX_ARG which, pixel, 4u);
    float4 w5 = rgiReservoirWord(RGI_CTX_ARG which, pixel, 5u);
    float4 w6 = rgiReservoirWord(RGI_CTX_ARG which, pixel, 6u);
    float4 w7 = rgiReservoirWord(RGI_CTX_ARG which, pixel, 7u);
    float4 w8 = rgiReservoirWord(RGI_CTX_ARG which, pixel, 8u);
    RgiReservoir r;
    r.key = rgiReservoirWord(RGI_CTX_ARG which, pixel, 0u);
    r.W = w1.x;
    r.M = w1.y;
    r.flags = uint(w1.z);
    r.phat = w1.w;
    r.a = float3(w2.x, w2.y, w2.z);
    r.pY = w2.w;
    r.cY = float3(w3.x, w3.y, w3.z);
    r.w3 = float3(w4.x, w4.y, w4.z);
    r.c3 = float3(w5.x, w5.y, w5.z);
    r.e0 = float3(w6.x, w6.y, w6.z);
    r.p0 = w6.w;
    r.e1 = float3(w7.x, w7.y, w7.z);
    r.p1 = w7.w;
    r.le = float3(w3.w, w4.w, w5.w);
    r.residual = float3(w8.x, w8.y, w8.z);
    return r;
}

/// Word k (< kRgiReservoirWords) of a reservoir.
PT_FN float4 rgiReservoirPack(RgiReservoir r, uint k) {
    if (k == 0u) {
        return r.key;
    }
    if (k == 1u) {
        return float4(r.W, r.M, float(r.flags), r.phat);
    }
    if (k == 2u) {
        return float4(r.a.x, r.a.y, r.a.z, r.pY);
    }
    if (k == 3u) {
        return float4(r.cY.x, r.cY.y, r.cY.z, r.le.x);
    }
    if (k == 4u) {
        return float4(r.w3.x, r.w3.y, r.w3.z, r.le.y);
    }
    if (k == 5u) {
        return float4(r.c3.x, r.c3.y, r.c3.z, r.le.z);
    }
    if (k == 6u) {
        return float4(r.e0.x, r.e0.y, r.e0.z, r.p0);
    }
    if (k == 7u) {
        return float4(r.e1.x, r.e1.y, r.e1.z, r.p1);
    }
    return float4(r.residual.x, r.residual.y, r.residual.z, 0.0f);
}

/// The shading frame of hit h reached along d (as ptRenderSample builds it).
PT_FN RgiVertex rgiMakeVertex(RGI_CTX_PARAM PtRawHit h, float3 d) {
    RgiVertex V;
    V.valid = true;
    V.S = ptLoadSurface(RGI_PT h);
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
    V.bounce = 0u;
    return V;
}

PT_FN PtRawHit rgiKeyHit(float4 k) {
    PtRawHit h;
    h.hit = true;
    h.t = 0.0f;
    h.instance = k.x >= 0.0f ? uint(k.x) : 0u;
    h.primitive = uint(k.y);
    h.u = k.z;
    h.v = k.w;
    return h;
}

PT_FN RgiVertex rgiLoadVertex(RGI_CTX_PARAM uint slot, uint pixel) {
    float4 w0 = rgiSurfaceWord(RGI_CTX_ARG slot, pixel, 0u);
    float4 w1 = rgiSurfaceWord(RGI_CTX_ARG slot, pixel, 1u);
    RgiVertex V = rgiMakeVertex(RGI_CTX_ARG rgiKeyHit(w0), float3(w1.x, w1.y, w1.z));
    V.valid = w1.w > 0.5f && w0.x >= 0.0f;
    V.depth = rgiSurfaceWord(RGI_CTX_ARG slot, pixel, 2u).w;
    V.bounce = uint(rgiSurfaceWord(RGI_CTX_ARG slot, pixel, 3u).w);
    return V;
}

/// The path tracer's side rule for a direction leaving vertex (m, ng) in the local frame (opaque, not thin: above
/// both the geometric and the shading surface).
PT_FN bool rgiSideOk(BsdfMaterial m, float3 ng, float3 wiW, float3 wiL) {
    if (m.model == kBsdfModelOpaque && (m.flags & kBsdfFlagSssThin) == 0u) {
        return dot(wiW, ng) > 0.0f && wiL.z > 0.0f;
    }
    return true;
}

/// Nothing (that passes the alpha test, visible mask: what the continuation would hit) between x1 and the point p.
PT_FN bool rgiSegmentClear(RGI_CTX_PARAM PtParams P, RgiVertex V, float3 wi, float3 p) {
    float3 so = ptOffset(V.pos, V.ng, wi, P.rayEps);
    float3 toP = p - so;
    float len = sqrt(dot(toP, toP));
    float3 sd = len > 0.0f ? toP * (1.0f / len) : wi;
    float tmax = len * (1.0f - 1e-4f) - P.rayEps;
    if (!(tmax > 0.0f)) {
        return false;
    }
    PtRawHit h = ptTraceScene(RGI_PT P, so, sd, 0.0f, tmax, kPtMaskVisible);
    return !h.hit;
}

/// The integrand of sample r at vertex V (see THE SAMPLE; area measure for surface samples, solid angle for escape
/// directions); with `vis` the x1 -> x2 segment (or the escape) is traced.
PT_FN float3 rgiEval(RGI_CTX_PARAM PtParams P, RgiVertex V, RgiReservoir r, bool vis) {
    float3 zero = float3(0.0f, 0.0f, 0.0f);
    if (!V.valid || (r.flags & kRgiSampleValid) == 0u) {
        return zero;
    }
    if ((r.flags & kRgiSampleDirection) != 0u) {
        float3 wiD = r.a;
        float3 wiDL = float3(dot(wiD, V.tx), dot(wiD, V.ty), dot(wiD, V.n));
        if (!rgiSideOk(V.S.m, V.ng, wiD, wiDL)) {
            return zero;
        }
        float3 fD = bsdfEval(RGI_LUT V.S.m, V.wo, wiDL) * r.le;
        if (!(ptMax3(fD) > 0.0f)) {
            return zero;
        }
        if (vis) {
            PtRawHit hd = ptTraceScene(RGI_PT P, ptOffset(V.pos, V.ng, wiD, P.rayEps), wiD, 0.0f, kPtFar,
                                       kPtMaskVisible);
            if (hd.hit) {
                return zero;
            }
        }
        return fD;
    }
    PtSurface S2 = ptLoadSurface(RGI_PT rgiKeyHit(r.key));
    float3 v = S2.position - V.pos;
    float d2 = dot(v, v);
    if (!(d2 > 1e-12f)) {
        return zero;
    }
    float3 wi = v * (1.0f / sqrt(d2));
    bool back = dot(wi, S2.geoNormal) > 0.0f;
    if (back != ((r.flags & kRgiSampleBack) != 0u)) {
        return zero;
    }
    float g = abs(dot(S2.geoNormal, wi)) / d2;
    float3 wiL = float3(dot(wi, V.tx), dot(wi, V.ty), dot(wi, V.n));
    if (!(g > 0.0f) || !rgiSideOk(V.S.m, V.ng, wi, wiL)) {
        return zero;
    }
    float3 f1 = bsdfEval(RGI_LUT V.S.m, V.wo, wiL);
    if (!(ptMax3(f1) > 0.0f)) {
        return zero;
    }
    float3 lo = r.le;
    if ((r.flags & (kRgiSampleNee | kRgiSampleCont)) != 0u) {
        // x2's frame as the path tracer builds it for a ray arriving along wi.
        float3 n2 = S2.normal;
        float3 ng2 = S2.geoNormal;
        if (S2.m.model == kBsdfModelOpaque && dot(ng2, wi) > 0.0f) {
            n2 = -n2;
            ng2 = -ng2;
        }
        float3 tx2;
        float3 ty2;
        ptBasis(n2, tx2, ty2);
        float3 wo2 = float3(-dot(wi, tx2), -dot(wi, ty2), -dot(wi, n2));
        if ((r.flags & kRgiSampleNee) != 0u) {
            float3 wyL = float3(dot(r.a, tx2), dot(r.a, ty2), dot(r.a, n2));
            if (rgiSideOk(S2.m, ng2, r.a, wyL)) {
                float wy = 1.0f;
                if (r.pY > 0.0f) {
                    wy = ptMisWeight(P.flags, r.pY, bsdfPdf(RGI_LUT S2.m, wo2, wyL));
                }
                lo = lo + bsdfEval(RGI_LUT S2.m, wo2, wyL) * r.cY * wy;
            }
        }
        if ((r.flags & kRgiSampleCont) != 0u) {
            float3 w3L = float3(dot(r.w3, tx2), dot(r.w3, ty2), dot(r.w3, n2));
            bool ok = true;
            if (S2.m.model == kBsdfModelOpaque && (S2.m.flags & kBsdfFlagSssThin) == 0u) {
                ok = dot(r.w3, ng2) > 0.0f;
            }
            if (ok) {
                float3 li = r.c3;
                if ((r.flags & (kRgiSampleEmit0 | kRgiSampleEmit1)) != 0u) {
                    float pb = bsdfPdf(RGI_LUT S2.m, wo2, w3L);
                    if ((r.flags & kRgiSampleEmit0) != 0u) {
                        li = li + r.e0 * ptMisWeight(P.flags, pb, r.p0);
                    }
                    if ((r.flags & kRgiSampleEmit1) != 0u) {
                        li = li + r.e1 * ptMisWeight(P.flags, pb, r.p1);
                    }
                }
                lo = lo + bsdfEval(RGI_LUT S2.m, wo2, w3L) * li;
            }
        }
    }
    float3 F = f1 * lo * g;
    if (!(ptMax3(F) > 0.0f)) {
        return zero;
    }
    if (vis && !rgiSegmentClear(RGI_CTX_ARG P, V, wi, S2.position)) {
        return zero;
    }
    return F;
}

PT_FN float rgiTarget(RGI_CTX_PARAM PtParams P, RgiVertex V, RgiReservoir r, bool vis) {
    return ptLum(rgiEval(RGI_CTX_ARG P, V, r, vis));
}

// ---- the tail: the path tracer's loop from a ray ------------------------------------------------------------------

/// Emitter K of a continuation (see THE SAMPLE): slots 0 and 1 of `cap`, the rest weighted for the owner (overflow).
PT_FN void rgiCapture(PtParams P, float3 v, float pL, float prevPdf, PT_INOUT(RgiReservoir) cap,
                      PT_INOUT(float3) overflow) {
    if ((cap.flags & kRgiSampleEmit0) == 0u) {
        cap.flags = cap.flags | kRgiSampleEmit0;
        cap.e0 = v;
        cap.p0 = pL;
    } else if ((cap.flags & kRgiSampleEmit1) == 0u) {
        cap.flags = cap.flags | kRgiSampleEmit1;
        cap.e1 = v;
        cap.p1 = pL;
    } else {
        overflow = overflow + v * ptMisWeight(P.flags, prevPdf, pL);
    }
}

/// Radiance the path tracer's rules collect along the ray (o, d) whose first hit is vertex `bounce`, with the MIS
/// state of the vertex the ray left (prevDelta, prevPdf, prevP, prevN, prevRestir: see pt_reference_core.h). With
/// `skipFirst` the first segment's light-set emission is left out (the trace pass collected it). With `capture`
/// (a non-dirac continuation of x2) the light-set emitters reached before the next scattering vertex are not added
/// but handed to rgiCapture unweighted, with their light-set density. Russian roulette uses the throughput of this
/// tail only. Random numbers: `seed`, kPtDimsPerBounce per vertex.
PT_FN float3 rgiTrace(RGI_CTX_PARAM PtParams P, float3 o0, float3 d0, uint bounce, uint seed, bool prevDelta0,
                      float prevPdf0, float3 prevP0, float3 prevN0, bool prevRestir0, bool skipFirst, bool capture,
                      PT_INOUT(RgiReservoir) cap, PT_INOUT(float3) overflow) {
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    bool capturing = capture && !prevDelta0;
    float3 thr = float3(1.0f, 1.0f, 1.0f);
    float3 o = o0;
    float3 d = d0;
    bool prevDelta = prevDelta0;
    float prevPdf = prevPdf0;
    float3 prevP = prevP0;
    float3 prevN = prevN0;
    bool prevRestir = prevRestir0;
    bool nee = (P.flags & kPtFlagNee) != 0u;
    bool bsdfLights = (P.flags & kPtFlagBsdfLights) != 0u;
    for (uint b = bounce; b < P.maxBounces + 1u; ++b) {
        uint dim = (b - bounce) * kPtDimsPerBounce;
        bool first = skipFirst && b == bounce;
        PtRawHit h = ptTraceScene(RGI_PT P, o, d, 0.0f, kPtFar, kPtMaskVisible);
        float tHit = h.hit ? h.t : kPtFar;
        float3 le = float3(0.0f, 0.0f, 0.0f);
        if (capturing) {
            if (bsdfLights) {
                for (uint i = 0u; i < P.analyticCount; ++i) {
                    RlLight L = ptLoadLight(RGI_PT i);
                    bool distant = L.kind == kRlKindDistant;
                    if (distant ? h.hit : ptAnalyticDistance(L, o, d) >= tHit) {
                        continue;
                    }
                    float3 lei = PT_B3(rlLightEval(L, PT_L3(o), PT_L3(d)));
                    if (ptMax3(lei) > 0.0f) {
                        float pL = nee ? ptLightSetPdf(RGI_PT prevP, prevN, i, d) : 0.0f;
                        rgiCapture(P, thr * lei, pL, prevPdf, cap, overflow);
                    }
                }
            }
        } else if (!first && (prevDelta || (bsdfLights && !prevRestir))) {
            le = ptAnalyticEmission(RGI_PT P, o, d, tHit, !h.hit, prevDelta, prevPdf, prevP, prevN);
        }
        if (!h.hit) {
            sum = sum + thr * (le + P.sky);
            break;
        }
        PtSurface S = ptLoadSurface(RGI_PT h);
        float3 contrib = le;
        if (capturing && S.light != kPtInvalid && ptMax3(S.emission) > 0.0f) {
            if (bsdfLights) {
                float cov = (S.flags & (kPtMatUnlit | kPtMatAlphaBlend)) == (kPtMatUnlit | kPtMatAlphaBlend)
                                ? S.alpha
                                : 1.0f;
                float pL = nee ? ptLightSetPdf(RGI_PT prevP, prevN, S.light, d) : 0.0f;
                rgiCapture(P, thr * S.emission * cov, pL, prevPdf, cap, overflow);
            }
        } else if (ptMax3(S.emission) > 0.0f && !(first && S.light != kPtInvalid)) {
            float w = 1.0f;
            if (S.light != kPtInvalid && !prevDelta) {
                if (!bsdfLights || prevRestir) {
                    w = 0.0f;
                } else if (nee) {
                    w = ptMisWeight(P.flags, prevPdf, ptLightSetPdf(RGI_PT prevP, prevN, S.light, d));
                }
            }
            float coverage = (S.flags & (kPtMatUnlit | kPtMatAlphaBlend)) == (kPtMatUnlit | kPtMatAlphaBlend)
                                 ? S.alpha
                                 : 1.0f;
            contrib = contrib + S.emission * (w * coverage);
        }
        sum = sum + thr * contrib;
        if ((S.flags & (kPtMatUnlit | kPtMatAlphaBlend)) == (kPtMatUnlit | kPtMatAlphaBlend) && S.alpha < 1.0f &&
            b < P.maxBounces) {
            thr = thr * (1.0f - max(S.alpha, 0.0f));
            o = ptOffset(S.position, S.geoNormal, d, P.rayEps);
            continue;
        }
        if ((S.flags & kPtMatUnlit) != 0u || b == P.maxBounces) {
            break;
        }
        if (S.portal != kPtInvalid) {
            if (S.portal < P.portalCount) {
                float4 m0 = ptPortalWord(RGI_PT S.portal, 0u);
                float4 m1 = ptPortalWord(RGI_PT S.portal, 1u);
                float4 m2 = ptPortalWord(RGI_PT S.portal, 2u);
                float3 exitP = ptXformPoint(m0, m1, m2, S.position);
                float3 exitD = ptSafeNormalize(ptXformVector(m0, m1, m2, d), d);
                float3 exitN = ptSafeNormalize(ptXformVector(m0, m1, m2, S.geoNormal), S.geoNormal);
                o = ptOffset(exitP, exitN, exitD, P.rayEps);
                d = exitD;
                prevDelta = true;
                capturing = false;
                continue;
            }
            break;
        }
        float3 n = S.normal;
        float3 ng = S.geoNormal;
        if (S.m.model == kBsdfModelOpaque && dot(ng, d) > 0.0f) {
            n = -n;
            ng = -ng;
        }
        float3 tx;
        float3 ty;
        ptBasis(n, tx, ty);
        float3 wo = float3(-dot(d, tx), -dot(d, ty), -dot(d, n));
        float3 treeN = ptTreeNormal(S, n);
        if (nee && P.lightCount > 0u) {
            PtLightPick lp = ptSampleLightSet(RGI_PT S.position, treeN, ptRandom(seed, dim + 0u),
                                              ptRandom(seed, dim + 1u), ptRandom(seed, dim + 2u));
            if (lp.light != kPtInvalid && lp.pdf > 0.0f && ptMax3(lp.radiance) > 0.0f) {
                float3 wiL = float3(dot(lp.wi, tx), dot(lp.wi, ty), dot(lp.wi, n));
                float3 f = bsdfEval(RGI_LUT S.m, wo, wiL);
                if (rgiSideOk(S.m, ng, lp.wi, wiL) && ptMax3(f) > 0.0f) {
                    float3 so = ptOffset(S.position, ng, lp.wi, P.rayEps);
                    float3 sd = lp.wi;
                    float tmax = kPtFar;
                    if (lp.dist < kPtFar * 0.5f) {
                        float3 toL = S.position + lp.wi * lp.dist - so;
                        float len = sqrt(dot(toL, toL));
                        sd = len > 0.0f ? toL * (1.0f / len) : lp.wi;
                        tmax = len * (1.0f - 1e-4f) - P.rayEps;
                    }
                    if (tmax > 0.0f && ptVisible(RGI_PT P, so, sd, tmax)) {
                        float w = 1.0f;
                        if (!lp.delta && bsdfLights) {
                            w = ptMisWeight(P.flags, lp.pdf, bsdfPdf(RGI_LUT S.m, wo, wiL));
                        }
                        sum = sum + thr * f * lp.radiance * (w / lp.pdf);
                    }
                }
            }
        }
        float4 u4 = float4(ptRandom(seed, dim + 3u), ptRandom(seed, dim + 4u), ptRandom(seed, dim + 5u),
                           ptRandom(seed, dim + 6u));
        BsdfSample bs = bsdfSample(RGI_LUT S.m, wo, u4);
        if ((bs.flags & kBsdfSampleValid) == 0u || !(ptMax3(bs.weight) > 0.0f)) {
            break;
        }
        uint lobe = bs.flags >> kBsdfLobeShift;
        float3 wiW = tx * bs.wi.x + ty * bs.wi.y + n * bs.wi.z;
        if (S.m.model == kBsdfModelOpaque && (S.m.flags & kBsdfFlagSssThin) == 0u && lobe != kBsdfLobeOpacity &&
            dot(wiW, ng) <= 0.0f) {
            break;
        }
        thr = thr * bs.weight;
        capturing = false;
        prevDelta = (bs.flags & kBsdfSampleDelta) != 0u;
        prevRestir = false;
        prevPdf = bs.pdf;
        prevP = S.position;
        prevN = treeN;
        o = ptOffset(S.position, ng, wiW, P.rayEps);
        d = wiW;
        if ((P.flags & kPtFlagRussianRoulette) != 0u && b + 1u >= P.rrStart) {
            float q = min(ptMax3(thr), 0.95f);
            if (!(ptRandom(seed, dim + 7u) < q)) {
                break;
            }
            thr = thr * (1.0f / q);
        }
    }
    return sum;
}

// ---- surface ---------------------------------------------------------------------------------------------------

/// The surface record of the G-buffer vertex (hit h along d, vertex index `bounce`); `have` false: none.
PT_FN void rgiSurfaceWords(RGI_CTX_PARAM PtParams P, bool have, PtRawHit h, float3 d, uint bounce, PT_OUT(float4) w0,
                           PT_OUT(float4) w1, PT_OUT(float4) w2, PT_OUT(float4) w3) {
    w0 = float4(-1.0f, 0.0f, 0.0f, 0.0f);
    w1 = float4(0.0f, 0.0f, 0.0f, 0.0f);
    w2 = float4(0.0f, 0.0f, 0.0f, 0.0f);
    w3 = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (!have) {
        return;
    }
    RgiVertex V = rgiMakeVertex(RGI_CTX_ARG h, d);
    float3 e = V.pos - P.camOrigin;
    w0 = float4(float(h.instance), float(h.primitive), h.u, h.v);
    w1 = float4(d.x, d.y, d.z, 1.0f);
    w2 = float4(V.pos.x, V.pos.y, V.pos.z, sqrt(dot(e, e)));
    w3 = float4(V.n.x, V.n.y, V.n.z, float(bounce));
}

// ---- initial ---------------------------------------------------------------------------------------------------

PT_FN RgiReservoir rgiInitial(RGI_CTX_PARAM PtParams P, uint px, uint py) {
    RgiReservoir r = rgiEmpty();
    RgiVertex V = rgiLoadVertex(RGI_CTX_ARG 0u, py * P.width + px);
    if (!V.valid || V.bounce >= P.maxBounces) {
        return r;
    }
    r.M = 1.0f;
    uint seed = ptSeed(px, py, P.frameSeed ^ 0x52474901u, P.sampleBase);
    uint seedTail = ptHash(seed ^ 0x7A11u);
    float4 u4 = float4(ptRandom(seed, 0u), ptRandom(seed, 1u), ptRandom(seed, 2u), ptRandom(seed, 3u));
    BsdfSample bs = bsdfSample(RGI_LUT V.S.m, V.wo, u4);
    if ((bs.flags & kBsdfSampleValid) == 0u || !(ptMax3(bs.weight) > 0.0f)) {
        return r;
    }
    uint lobe = bs.flags >> kBsdfLobeShift;
    float3 wi = V.tx * bs.wi.x + V.ty * bs.wi.y + V.n * bs.wi.z;
    if (V.S.m.model == kBsdfModelOpaque && (V.S.m.flags & kBsdfFlagSssThin) == 0u && lobe != kBsdfLobeOpacity &&
        dot(wi, V.ng) <= 0.0f) {
        return r;
    }
    bool diOn = (P.flags & kPtFlagRestirDi) != 0u;
    float3 ovf = float3(0.0f, 0.0f, 0.0f);
    float3 o = ptOffset(V.pos, V.ng, wi, P.rayEps);
    uint b2 = V.bounce + 1u;
    if ((bs.flags & kBsdfSampleDelta) != 0u) {
        // A dirac lobe at x1: not reconnectable.
        r.residual = bs.weight * rgiTrace(RGI_CTX_ARG P, o, wi, b2, seedTail, true, bs.pdf, V.pos, V.treeN, diOn,
                                          true, false, r, ovf);
        return r;
    }
    PtRawHit h = ptTraceScene(RGI_PT P, o, wi, 0.0f, kPtFar, kPtMaskVisible);
    if (!h.hit) {
        // Escaped: the sky (the light-set distant lights are the trace pass's) as a direction sample.
        r.flags = kRgiSampleValid | kRgiSampleDirection;
        r.a = wi;
        r.le = P.sky;
        float ph = rgiTarget(RGI_CTX_ARG P, V, r, false);
        if (!(ph > 0.0f)) {
            r.flags = 0u;
            return r;
        }
        r.W = 1.0f / bs.pdf;
        r.phat = ph;
        return r;
    }
    PtSurface S2 = ptLoadSurface(RGI_PT h);
    bool passThrough = (S2.flags & (kPtMatUnlit | kPtMatAlphaBlend)) == (kPtMatUnlit | kPtMatAlphaBlend) &&
                       S2.alpha < 1.0f && b2 < P.maxBounces;
    if (S2.portal != kPtInvalid || passThrough) {
        // The path goes on along wi (portal / blended unlit layer): not reconnectable.
        r.residual = bs.weight * rgiTrace(RGI_CTX_ARG P, o, wi, b2, seedTail, false, bs.pdf, V.pos, V.treeN, diOn,
                                          true, false, r, ovf);
        return r;
    }
    // x2's frame is built from the reconnection direction x1 -> x2 (what every domain evaluates), not the offset
    // ray's direction, so the canonical target equals the generated tail exactly (near-specular x2).
    float3 wiR = ptSafeNormalize(S2.position - V.pos, wi);
    r.key = float4(float(h.instance), float(h.primitive), h.u, h.v);
    r.flags = kRgiSampleValid | (dot(wiR, S2.geoNormal) > 0.0f ? kRgiSampleBack : 0u);
    if (S2.light == kPtInvalid && ptMax3(S2.emission) > 0.0f) {
        float coverage = (S2.flags & (kPtMatUnlit | kPtMatAlphaBlend)) == (kPtMatUnlit | kPtMatAlphaBlend)
                             ? S2.alpha
                             : 1.0f;
        r.le = S2.emission * coverage;
    }
    if ((S2.flags & kPtMatUnlit) == 0u && b2 < P.maxBounces) {
        // x2 scatters: NEE and the continuation (MIS re-evaluated by rgiEval), in x2's frame for this ray.
        float3 n2 = S2.normal;
        float3 ng2 = S2.geoNormal;
        if (S2.m.model == kBsdfModelOpaque && dot(ng2, wiR) > 0.0f) {
            n2 = -n2;
            ng2 = -ng2;
        }
        float3 tx2;
        float3 ty2;
        ptBasis(n2, tx2, ty2);
        float3 wo2 = float3(-dot(wiR, tx2), -dot(wiR, ty2), -dot(wiR, n2));
        bool bsdfLights = (P.flags & kPtFlagBsdfLights) != 0u;
        float3 treeN2 = ptTreeNormal(S2, n2);
        bool nee = (P.flags & kPtFlagNee) != 0u;
        if (nee && P.lightCount > 0u) {
            PtLightPick lp = ptSampleLightSet(RGI_PT S2.position, treeN2, ptRandom(seed, 4u), ptRandom(seed, 5u),
                                              ptRandom(seed, 6u));
            if (lp.light != kPtInvalid && lp.pdf > 0.0f && ptMax3(lp.radiance) > 0.0f) {
                float3 wyL = float3(dot(lp.wi, tx2), dot(lp.wi, ty2), dot(lp.wi, n2));
                if (rgiSideOk(S2.m, ng2, lp.wi, wyL)) {
                    float3 so = ptOffset(S2.position, ng2, lp.wi, P.rayEps);
                    float3 sd = lp.wi;
                    float tmax = kPtFar;
                    if (lp.dist < kPtFar * 0.5f) {
                        float3 toL = S2.position + lp.wi * lp.dist - so;
                        float len = sqrt(dot(toL, toL));
                        sd = len > 0.0f ? toL * (1.0f / len) : lp.wi;
                        tmax = len * (1.0f - 1e-4f) - P.rayEps;
                    }
                    if (tmax > 0.0f && ptVisible(RGI_PT P, so, sd, tmax)) {
                        r.flags = r.flags | kRgiSampleNee;
                        r.a = lp.wi;
                        r.pY = !lp.delta && bsdfLights ? lp.pdf : 0.0f;
                        r.cY = lp.radiance * (1.0f / lp.pdf);
                    }
                }
            }
        }
        float4 v4 = float4(ptRandom(seed, 7u), ptRandom(seed, 8u), ptRandom(seed, 9u), ptRandom(seed, 10u));
        BsdfSample b3 = bsdfSample(RGI_LUT S2.m, wo2, v4);
        if ((b3.flags & kBsdfSampleValid) != 0u && ptMax3(b3.weight) > 0.0f) {
            uint lobe3 = b3.flags >> kBsdfLobeShift;
            float3 w3 = tx2 * b3.wi.x + ty2 * b3.wi.y + n2 * b3.wi.z;
            bool ok = !(S2.m.model == kBsdfModelOpaque && (S2.m.flags & kBsdfFlagSssThin) == 0u &&
                        lobe3 != kBsdfLobeOpacity && dot(w3, ng2) <= 0.0f);
            if (ok) {
                float3 o3 = ptOffset(S2.position, ng2, w3, P.rayEps);
                bool delta3 = (b3.flags & kBsdfSampleDelta) != 0u;
                float3 li = rgiTrace(RGI_CTX_ARG P, o3, w3, b2 + 1u, seedTail, delta3, b3.pdf, S2.position, treeN2,
                                     false, false, !delta3, r, ovf);
                if (delta3) {
                    r.residual = bs.weight * b3.weight * li;
                } else {
                    r.residual = bs.weight * b3.weight * ovf;
                    float inv = 1.0f / b3.pdf;
                    r.e0 = r.e0 * inv;
                    r.e1 = r.e1 * inv;
                    if (ptMax3(li) > 0.0f || (r.flags & (kRgiSampleEmit0 | kRgiSampleEmit1)) != 0u) {
                        r.flags = r.flags | kRgiSampleCont;
                        r.w3 = w3;
                        r.c3 = li * inv;
                    }
                }
            }
        }
    }
    float3 v = S2.position - V.pos;
    float d2 = dot(v, v);
    float cos2 = abs(dot(S2.geoNormal, v)) / sqrt(max(d2, 1e-30f));
    float ph = rgiTarget(RGI_CTX_ARG P, V, r, false);
    if (!(ph > 0.0f) || !(cos2 > 0.0f) || !(d2 > 1e-12f)) {
        r.flags = 0u;
        return r;
    }
    r.W = d2 / (bs.pdf * cos2);
    r.phat = ph;
    return r;
}

// ---- reuse -----------------------------------------------------------------------------------------------------

/// Neighbour surface q of `slot` usable for reuse (normal, distance, the same vertex index).
PT_FN bool rgiSimilar(RGI_CTX_PARAM RgiParams R, uint slot, uint q, float3 n, float depth, uint bounce) {
    float4 w0 = rgiSurfaceWord(RGI_CTX_ARG slot, q, 0u);
    float4 w1 = rgiSurfaceWord(RGI_CTX_ARG slot, q, 1u);
    if (!(w1.w > 0.5f) || w0.x < 0.0f) {
        return false;
    }
    float4 w2 = rgiSurfaceWord(RGI_CTX_ARG slot, q, 2u);
    float4 w3 = rgiSurfaceWord(RGI_CTX_ARG slot, q, 3u);
    if (uint(w3.w) != bounce) {
        return false;
    }
    if (!(dot(n, float3(w3.x, w3.y, w3.z)) >= R.normalThreshold)) {
        return false;
    }
    return abs(w2.w - depth) <= R.depthThreshold * depth;
}

PT_FN void rgiTake(PT_INOUT(RgiReservoir) res, RgiReservoir src) {
    res.key = src.key;
    res.flags = src.flags;
    res.a = src.a;
    res.pY = src.pY;
    res.cY = src.cY;
    res.w3 = src.w3;
    res.c3 = src.c3;
    res.e0 = src.e0;
    res.p0 = src.p0;
    res.e1 = src.e1;
    res.p1 = src.p1;
    res.le = src.le;
}

/// Temporal (canonical + reprojected history) or spatial (canonical + disk neighbours) reuse at pixel (px, py).
PT_FN RgiReservoir rgiReuse(RGI_CTX_PARAM PtParams P, RgiParams R, uint px, uint py, bool temporal) {
    uint pixel = py * P.width + px;
    RgiReservoir canon = rgiLoadReservoir(RGI_CTX_ARG 0u, pixel);
    RgiVertex V = rgiLoadVertex(RGI_CTX_ARG 0u, pixel);
    if (!V.valid) {
        return canon;
    }
    uint seed = ptSeed(px, py, P.frameSeed ^ (temporal ? 0x52474954u : (0x52474960u + R.iteration)), P.sampleBase);
    uint qs[8];
    for (uint z = 0u; z < 8u; ++z) {
        qs[z] = 0u;
    }
    uint k = 0u;
    uint slotN = temporal ? 1u : 0u;
    uint whichN = temporal ? 1u : 0u;
    if (temporal) {
        if ((R.flags & (kRgiFlagHistory | kRgiFlagTemporal)) == (kRgiFlagHistory | kRgiFlagTemporal)) {
            float ux = 0.0f;
            float uy = 0.0f;
            if (ptProject(P.prevOrigin, P.prevRight, P.prevUp, P.prevForward, V.pos, ux, uy) && ux >= 0.0f &&
                ux < 1.0f && uy >= 0.0f && uy < 1.0f) {
                uint qx = rgiMinU(uint(ux * float(P.width)), P.width - 1u);
                uint qy = rgiMinU(uint(uy * float(P.height)), P.height - 1u);
                uint q = qy * P.width + qx;
                if (rgiSimilar(RGI_CTX_ARG R, 1u, q, V.n, V.depth, V.bounce)) {
                    qs[0] = q;
                    k = 1u;
                }
            }
        }
    } else {
        uint count = rgiMinU(R.spatialSamples, kRgiMaxNeighbors);
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
            if (rgiSimilar(RGI_CTX_ARG R, 0u, q, V.n, V.depth, V.bounce)) {
                qs[k] = q;
                k = k + 1u;
            }
        }
    }
    bool unbiased = (R.flags & kRgiFlagUnbiased) != 0u;
    float mc = canon.M;
    float mTot = mc;
    for (uint i = 0u; i < k; ++i) {
        float mi = rgiLoadReservoir(RGI_CTX_ARG whichN, qs[i]).M;
        if (temporal) {
            mi = min(mi, R.maxHistory * mc);
        }
        mTot = mTot + mi;
    }
    RgiReservoir res = rgiEmpty();
    res.M = mTot;
    res.residual = canon.residual;
    float wSum = 0.0f;
    float pSel = 0.0f;
    uint dimSel = 64u;
    float pcc = 0.0f;
    if ((canon.flags & kRgiSampleValid) != 0u && canon.W > 0.0f) {
        pcc = rgiTarget(RGI_CTX_ARG P, V, canon, unbiased);
    }
    float c = k > 0u ? mc / float(k) : mc;
    float mcWeight = k == 0u ? 1.0f : 0.0f;
    for (uint i = 0u; i < k; ++i) {
        RgiReservoir ri = rgiLoadReservoir(RGI_CTX_ARG whichN, qs[i]);
        float mi = ri.M;
        if (temporal) {
            mi = min(mi, R.maxHistory * mc);
        }
        bool live = (ri.flags & kRgiSampleValid) != 0u && ri.W > 0.0f;
        if (!unbiased) {
            if (live) {
                float pci = rgiTarget(RGI_CTX_ARG P, V, ri, false);
                float w = pci * ri.W * mi;
                if (w > 0.0f) {
                    wSum = wSum + w;
                    if (ptRandom(seed, dimSel + i) * wSum < w) {
                        rgiTake(res, ri);
                        pSel = pci;
                    }
                }
            }
            continue;
        }
        RgiVertex Vi = rgiLoadVertex(RGI_CTX_ARG slotN, qs[i]);
        float fi = (mi + c) / mTot;
        if (pcc > 0.0f) {
            float pic = rgiTarget(RGI_CTX_ARG P, Vi, canon, true);
            mcWeight = mcWeight + fi * (c * pcc) / (mi * pic + c * pcc);
        }
        if (live) {
            float pci = rgiTarget(RGI_CTX_ARG P, V, ri, true);
            if (pci > 0.0f) {
                float pii = rgiTarget(RGI_CTX_ARG P, Vi, ri, true);
                float m = fi * (mi * pii) / (mi * pii + c * pci);
                float w = m * pci * ri.W;
                if (w > 0.0f) {
                    wSum = wSum + w;
                    if (ptRandom(seed, dimSel + i) * wSum < w) {
                        rgiTake(res, ri);
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
            if (ptRandom(seed, dimSel + kRgiMaxNeighbors) * wSum < w) {
                rgiTake(res, canon);
                pSel = pcc;
            }
        }
    }
    if ((res.flags & kRgiSampleValid) != 0u && pSel > 0.0f) {
        res.W = unbiased ? wSum / pSel : wSum / (pSel * mTot);
        res.phat = pSel;
    } else {
        res.flags = 0u;
    }
    return res;
}

// ---- shade -----------------------------------------------------------------------------------------------------

/// indirect = F(Y) W at the canonical vertex + the residual (w = 1: a ReSTIR GI vertex, 0: no surface).
PT_FN float4 rgiShade(RGI_CTX_PARAM PtParams P, RgiReservoir r, uint pixel) {
    RgiVertex V = rgiLoadVertex(RGI_CTX_ARG 0u, pixel);
    if (!V.valid) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    float3 ind = r.residual;
    if ((r.flags & kRgiSampleValid) != 0u && r.W > 0.0f) {
        ind = ind + rgiEval(RGI_CTX_ARG P, V, r, true) * r.W;
    }
    return float4(ind.x, ind.y, ind.z, 1.0f);
}
