// FUSE Relight RL-5.5: the A-SVGF temporal-gradient producer on the path tracer (docs/plans/FUSE_REMIX_PORT_PLAN.md
// §5.5 "Anti-lag"; Schied, Peters, Dachsbacher, "Gradient Estimation for Real-Time Adaptive Temporal Filtering",
// HPG 2018). Single-source over the RL-5.1 path-tracing core (kernels/pt_reference_core.h, the PT_* dialect macros):
// included after it by the GLSL / Slang kernels (rl_dn_gradient.{comp,slang}, through rl_pt.{glsl,slang}) and by the
// C++ dialect (src/rl_dn_gradient_cpp.hpp, the CPU oracle).
//
// One invocation per 3 x 3 stratum of the render extent, two path-traced estimates (samplesPerPixel paths each):
//   1. re-shade: the stratum's record from the previous frame names a pixel q and that frame's camera, frame seed and
//      sample base (the previous parameter words). The same paths (same camera ray, same random numbers) are traced
//      again in the CURRENT scene (lights, instances, materials): (lumD', lumS'). With the previous frame's
//      (lumD, lumS) of those paths the gradient sample is (lumD', lumD, lumS', lumS) - only a change of the scene makes
//      it differ from 0, never the noise. (A-SVGF forward-projects the sample into the current frame; this producer
//      keeps it at its stratum: the denoiser's gradient filter is 5 x 5 strata wide, and camera motion alone does not
//      change the re-shaded paths.) Invalid (no record, the path left the scene, the extent changed): dPrev = -1.
//   2. record: a new pixel of the stratum (hash of the stratum and this frame's seed / sample base) traced with this
//      frame's parameters: (q, lumD, lumS, valid) for the next frame.
// Luminances are of the demodulated channels (PtSample::diffuse / specular), the signals the denoiser filters. The
// ReSTIR DI / GI hooks are off in both estimates (their reservoirs belong to the main pass).

/// Pixel index (y * width + x) of the stratum's sample for a frame.
PT_FN uint rldnPick(uint sx, uint sy, uint frameSeed, uint sampleBase, uint width, uint height) {
    uint k = ptHash(sx ^ ptHash(sy ^ ptHash(frameSeed ^ ptHash(sampleBase ^ 0x5BD1E995u)))) % 9u;
    uint px = sx * 3u + k % 3u;
    uint py = sy * 3u + k / 3u;
    px = px < width ? px : width - 1u;
    py = py < height ? py : height - 1u;
    return py * width + px;
}

/// The current scene's parameters with the previous frame's camera and random-number stream (hooks off).
PT_FN PtParams rldnPatch(PtParams cur, PtParams prev) {
    PtParams P = cur;
    P.camOrigin = prev.camOrigin;
    P.camRight = prev.camRight;
    P.camUp = prev.camUp;
    P.camForward = prev.camForward;
    P.frameSeed = prev.frameSeed;
    P.sampleBase = prev.sampleBase;
    P.flags = cur.flags & ~(kPtFlagRestirDi | kPtFlagDiRecord | kPtFlagRestirGi | kPtFlagGiRecord);
    return P;
}

/// Mean demodulated luminances of pixel (px, py)'s paths: (lumD, lumS, hit (1 / 0), 0).
PT_FN float4 rldnShade(PT_CTX_PARAM PtParams P, uint px, uint py) {
    uint spp = P.samplesPerPixel > 1u ? P.samplesPerPixel : 1u;
    float d = 0.0f;
    float s = 0.0f;
    float hit = 0.0f;
    for (uint k = 0u; k < spp; ++k) {
        PtSample r = ptRenderSample(PT_CTX_ARG P, px, py, P.sampleBase + k);
        d = d + ptLum(r.diffuse);
        s = s + ptLum(r.specular);
        if (k == 0u && (r.flags & kPtSampleHit) != 0u) {
            hit = 1.0f;
        }
    }
    float inv = 1.0f / float(spp);
    return float4(d * inv, s * inv, hit, 0.0f);
}

/// One stratum: `rec` = the previous frame's record (q, lumD, lumS, valid); `havePrev` = the previous parameters are
/// valid for this extent. Writes the gradient sample and the new record.
PT_FN void rldnStratum(PT_CTX_PARAM PtParams cur, PtParams prev, bool havePrev, float4 rec, uint sx, uint sy,
                       PT_OUT(float4) grad, PT_OUT(float4) next) {
    grad = float4(0.0f, -1.0f, 0.0f, -1.0f);
    uint n = cur.width * cur.height;
    if (havePrev && rec.w > 0.0f && rec.x >= 0.0f && rec.x < float(n)) {
        uint q = uint(rec.x);
        PtParams P = rldnPatch(cur, prev);
        float4 v = rldnShade(PT_CTX_ARG P, q % cur.width, q / cur.width);
        if (v.z > 0.0f) {
            grad = float4(v.x, rec.y, v.y, rec.z);
        }
    }
    uint p = rldnPick(sx, sy, cur.frameSeed, cur.sampleBase, cur.width, cur.height);
    PtParams C = cur;
    C.flags = cur.flags & ~(kPtFlagRestirDi | kPtFlagDiRecord | kPtFlagRestirGi | kPtFlagGiRecord);
    float4 w = rldnShade(PT_CTX_ARG C, p % cur.width, p / cur.width);
    next = float4(float(p), w.x, w.y, w.z);
}
