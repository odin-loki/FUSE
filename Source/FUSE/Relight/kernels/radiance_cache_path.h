// FUSE Relight RL-5.4: the path-tracer side of the radiance cache (see radiance_cache_core.h): the bodies of the
// path-tracing core's hooks ptRadianceCacheVertex / ptRadianceCacheEnd (pt_reference_core.h), shared by the three
// dialects. Included after pt_reference_types.h, the BSDF core and radiance_cache_core.h.
// Required accessors (besides the table ones of radiance_cache_core.h):
//   float4 rcPathLoad(RC_CTX_PARAM uint px, uint py, uint k);            recorded vertex word k (< kRcMaxVertices x 3)
//   void   rcPathStore(RC_CTX_PARAM uint px, uint py, uint k, float4 v);
//   void   rcRecordStore(RC_CTX_PARAM uint record, uint k, float4 v);    training record word k of `record`
//
// TRAINING (kPtFlagRcTrain; the "relight.radiance_cache.train" stage and RadianceCacheCpu::train): every scattering
// vertex k of the path (up to maxVertices; not ReSTIR-owned ones) is remembered with the path throughput thr_k and the
// radiance collected so far R_k (before its own next-event estimation). At the end of the path, with the total R,
//   L_k = (R - R_k) / thr_k
// is an unbiased one-sample estimate of the radiance vertex k scatters towards the previous vertex (its emission is in
// R_k already: the cache holds reflected light only), with every later bounce, NEE and MIS weight of the path in it.
// The records (position, facing normal, L_k) go to the tile's record slots; unused slots are written invalid, so the
// record buffer is fully defined every frame.
//
// QUERY (kPtFlagRadianceCache; the path tracer's paths): the path spread
//   s = sum_i |x_{i-1} x_i| / sqrt(p(w_i) |cos theta_i|)      (camera segment: |x_0 x_1| x pixelAngle / sqrt |cos|,
//                                                              dirac scatters add nothing)
// grows along the path [Bekaert et al. 2003; Mueller et al. 2021]; at a vertex after the G-buffer vertex (not on the
// primary chain, index >= minBounce, opaque, perceptual roughness >= minRoughness) whose spread reaches spreadThreshold
// x its cell size, the cache's resolved radiance of the vertex's cell replaces the rest of the path (the path's
// footprint is wider than a cell there, so the cell's spatial blur is below the path's own). The estimate is biased by
// the cell average (spatial, directional within a normal bin, the temporal window) and unbiased otherwise.

/// Vertex bookkeeping (spread, training record). Returns 1 when the vertex asks the cache (QUERY conditions met).
RC_FN uint rcHookStep(RC_CTX_PARAM PtParams P, RcParams R, RC_INOUT(RcPathState) st, uint px, uint py, uint bounce,
                      uint vflags, PtSurface S, float3 n, float3 d, float tHit, float prevPdf, float3 thr, float3 acc) {
    float cosT = max(abs(dot(n, d)), 0.05f);
    if (st.seen == 0u) {
        st.spread = st.spread + tHit * R.pixelAngle / sqrt(cosT);
    } else if ((vflags & kRcVertexPrevDelta) == 0u) {
        st.spread = st.spread + tHit / sqrt(max(prevPdf, 1e-4f) * cosT);
    }
    st.seen = st.seen + 1u;
    if ((vflags & kRcVertexReplaced) != 0u) {
        return 0u;
    }
    if ((P.flags & kPtFlagRcTrain) != 0u) {
        if (st.count < rcMinU(R.maxVertices, kRcMaxVertices)) {
            uint k = st.count * 3u;
            rcPathStore(RC_CTX_ARG px, py, k, float4(S.position.x, S.position.y, S.position.z, acc.x));
            rcPathStore(RC_CTX_ARG px, py, k + 1u, float4(n.x, n.y, n.z, acc.y));
            rcPathStore(RC_CTX_ARG px, py, k + 2u, float4(thr.x, thr.y, thr.z, acc.z));
            st.count = st.count + 1u;
        }
        return 0u;
    }
    if ((P.flags & kPtFlagRadianceCache) == 0u || (vflags & kRcVertexChain) != 0u || bounce < R.minBounce ||
        S.m.model != kBsdfModelOpaque || S.m.roughness < R.minRoughness) {
        return 0u;
    }
    if (st.spread < R.spreadThreshold * rcLevelSize(R, rcLevel(R, S.position))) {
        return 0u;
    }
    return 1u;
}

/// The hash-grid answer at an asking vertex (with the query / hit counters). Returns 1 with `cached` on a hit.
RC_FN uint rcHookQuery(RC_CTX_PARAM RcParams R, float3 p, float3 n, RC_OUT(float3) cached) {
    float samples = 0.0f;
    bool hit = rcLookup(RC_CTX_ARG R, p, n, cached, samples);
    if ((R.flags & kRcFlagStats) != 0u) {
        rcTableAdd(RC_CTX_ARG kRcCounterQueries, 1u);
        if (hit) {
            rcTableAdd(RC_CTX_ARG kRcCounterHits, 1u);
        }
    }
    return hit ? 1u : 0u;
}

/// Vertex hook body. Returns 1 when the path ends with `cached` (x the path throughput).
RC_FN uint rcHookVertex(RC_CTX_PARAM PtParams P, RcParams R, RC_INOUT(RcPathState) st, uint px, uint py, uint bounce,
                        uint vflags, PtSurface S, float3 n, float3 d, float tHit, float prevPdf, float3 thr, float3 acc,
                        RC_OUT(float3) cached) {
    cached = float3(0.0f, 0.0f, 0.0f);
    if (rcHookStep(RC_CTX_ARG P, R, st, px, py, bounce, vflags, S, n, d, tHit, prevPdf, thr, acc) == 0u) {
        return 0u;
    }
    return rcHookQuery(RC_CTX_ARG R, S.position, n, cached);
}

RC_FN float rcSafeRatio(float a, float t) { return t > 1e-12f ? max(a, 0.0f) / t : 0.0f; }

/// End-of-path hook body: training records of the tile (TRAINING), then the state is reset.
RC_FN void rcHookEnd(RC_CTX_PARAM PtParams P, RcParams R, RC_INOUT(RcPathState) st, uint px, uint py, float3 acc) {
    if ((P.flags & kPtFlagRcTrain) != 0u) {
        uint base = rcTrainTile(R, P.width, px, py) * kRcMaxVertices;
        for (uint k = 0u; k < kRcMaxVertices; ++k) {
            if (k < st.count) {
                float4 a = rcPathLoad(RC_CTX_ARG px, py, k * 3u);
                float4 b = rcPathLoad(RC_CTX_ARG px, py, k * 3u + 1u);
                float4 c = rcPathLoad(RC_CTX_ARG px, py, k * 3u + 2u);
                float3 L = float3(rcSafeRatio(acc.x - a.w, c.x), rcSafeRatio(acc.y - b.w, c.y),
                                  rcSafeRatio(acc.z - c.w, c.z));
                rcRecordStore(RC_CTX_ARG base + k, 0u, float4(a.x, a.y, a.z, 1.0f));
                rcRecordStore(RC_CTX_ARG base + k, 1u, float4(b.x, b.y, b.z, 0.0f));
                rcRecordStore(RC_CTX_ARG base + k, 2u, float4(L.x, L.y, L.z, 0.0f));
            } else {
                rcRecordStore(RC_CTX_ARG base + k, 0u, float4(0.0f, 0.0f, 0.0f, 0.0f));
                rcRecordStore(RC_CTX_ARG base + k, 1u, float4(0.0f, 0.0f, 0.0f, 0.0f));
                rcRecordStore(RC_CTX_ARG base + k, 2u, float4(0.0f, 0.0f, 0.0f, 0.0f));
            }
        }
    }
    st.count = 0u;
    st.seen = 0u;
    st.spread = 0.0f;
    st.pad = 0.0f;
}
