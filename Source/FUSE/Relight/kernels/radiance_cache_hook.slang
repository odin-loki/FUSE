// FUSE Relight RL-5.4: ptRadianceCacheVertex / ptRadianceCacheEnd for the Slang path-tracing core (included by
// rl_pt.slang before pt_reference_core.h; bodies in radiance_cache_path.h, the cache in radiance_cache_core.h).
//   query mode   (kPtFlagRadianceCache; "relight.pt.trace", which defines RC_HOOK_QUERY): the table's device address
//                comes from PtParams.rcTableLo / rcTableHi (PathTracerGpu::setRadianceCache), its header holds the
//                frame's RcParams; queries (and the hit counters) read / add there;
//   train mode   (kPtFlagRcTrain; "relight.radiance_cache.train"): the stage's main sets g_rcTable / g_rcParams /
//                g_rcRecords from its ring block before tracing; the End hook writes the tile's records.
// Every other includer (the ReSTIR passes) leaves the addresses 0: both hooks are no-ops there.
#pragma once

#define RC_FN
#define RC_CONST static const
#define RC_OUT(T) out T
#define RC_INOUT(T) inout T
#define RC_CTX_PARAM
#define RC_CTX_ARG
#define RC_ASUINT(x) asuint(x)
#define RC_ASFLOAT(x) asfloat(x)
#define RC_PRECISE precise
#define RC_PARAM_WORDS(name) float4 name[5]

#include "radiance_cache_types.h"

static uint64_t g_rcTable = 0;   // the table (header + slots)
static uint64_t g_rcParams = 0;  // RcParams words: the table header, or the train stage's ring block
static uint64_t g_rcRecords = 0; // training records (train stage)
static uint g_rcCount = 0u;
static uint g_rcSeen = 0u;
static float g_rcSpread = 0.0;
static float4 g_rcVerts[24]; // kRcMaxVertices x 3

uint rcTableLoad(uint w) { return ((uint*)g_rcTable)[w]; }
void rcTableStore(uint w, uint x) { ((uint*)g_rcTable)[w] = x; }
uint rcTableCas(uint w, uint c, uint x) {
    uint prev;
    InterlockedCompareExchange(((uint*)g_rcTable)[w], c, x, prev);
    return prev;
}
uint rcTableAdd(uint w, uint x) {
    uint prev;
    InterlockedAdd(((uint*)g_rcTable)[w], x, prev);
    return prev;
}
float4 rcPathLoad(uint px, uint py, uint k) { return g_rcVerts[k]; }
void rcPathStore(uint px, uint py, uint k, float4 v) { g_rcVerts[k] = v; }
void rcRecordStore(uint record, uint k, float4 v) { ((float4*)g_rcRecords)[record * kRcRecordWords + k] = v; }

#include "radiance_cache_core.h"
#include "radiance_cache_path.h"

RcParams rcHookParams() {
    float4 w[5];
    for (uint k = 0u; k < 5u; ++k) {
        w[k] = ((float4*)g_rcParams)[k];
    }
    return rcParamsUnpack(w);
}

bool rcHookBind(PtParams P) {
#ifdef RC_HOOK_QUERY
    if (g_rcParams == 0 && (P.rcTableLo != 0u || P.rcTableHi != 0u)) {
        uint64_t a = (uint64_t(P.rcTableLo) << 8) | (uint64_t(P.rcTableHi) << 32);
        g_rcTable = a;
        g_rcParams = a;
    }
#endif
    return g_rcParams != 0;
}

uint ptRadianceCacheVertex(PtParams P, uint px, uint py, uint bounce, uint vflags, PtSurface S, float3 n, float3 d,
                           float tHit, float prevPdf, float3 thr, float3 acc, inout float3 cached) {
    if (!rcHookBind(P)) {
        return 0u;
    }
    RcParams R = rcHookParams();
    RcPathState st;
    st.count = g_rcCount;
    st.seen = g_rcSeen;
    st.spread = g_rcSpread;
    st.pad = 0.0;
    uint r = rcHookVertex(P, R, st, px, py, bounce, vflags, S, n, d, tHit, prevPdf, thr, acc, cached);
    g_rcCount = st.count;
    g_rcSeen = st.seen;
    g_rcSpread = st.spread;
    return r;
}

void ptRadianceCacheEnd(PtParams P, uint px, uint py, float3 acc) {
    if (!rcHookBind(P)) {
        return;
    }
    RcParams R = rcHookParams();
    RcPathState st;
    st.count = g_rcCount;
    st.seen = g_rcSeen;
    st.spread = g_rcSpread;
    st.pad = 0.0;
    rcHookEnd(P, R, st, px, py, acc);
    g_rcCount = st.count;
    g_rcSeen = st.seen;
    g_rcSpread = st.spread;
}
