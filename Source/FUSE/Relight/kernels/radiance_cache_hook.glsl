// FUSE Relight RL-5.4: ptRadianceCacheVertex / ptRadianceCacheEnd for the GLSL path-tracing core (included by
// rl_pt.glsl before pt_reference_core.h). GLSL twin of radiance_cache_hook.slang (see there).
#ifndef FUSE_RELIGHT_RADIANCE_CACHE_HOOK_GLSL
#define FUSE_RELIGHT_RADIANCE_CACHE_HOOK_GLSL

layout(buffer_reference, std430, buffer_reference_align = 4) buffer RcU32Ref { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer RcVec4Ref { vec4 v[]; };

#define RC_FN
#define RC_CONST const
#define RC_OUT(T) out T
#define RC_INOUT(T) inout T
#define RC_CTX_PARAM
#define RC_CTX_ARG
#define RC_ASUINT(x) floatBitsToUint(x)
#define RC_ASFLOAT(x) uintBitsToFloat(x)
#define RC_PRECISE precise
#define RC_PARAM_WORDS(name) vec4 name[5]

#include "radiance_cache_types.h"

uint64_t g_rcTable = uint64_t(0);   // the table (header + slots)
uint64_t g_rcParams = uint64_t(0);  // RcParams words: the table header, or the train stage's ring block
uint64_t g_rcRecords = uint64_t(0); // training records (train stage)
uint g_rcCount = 0u;
uint g_rcSeen = 0u;
float g_rcSpread = 0.0;
vec4 g_rcVerts[24]; // kRcMaxVertices x 3

uint rcTableLoad(uint w) { return RcU32Ref(g_rcTable).v[w]; }
void rcTableStore(uint w, uint x) { RcU32Ref(g_rcTable).v[w] = x; }
uint rcTableCas(uint w, uint c, uint x) { return atomicCompSwap(RcU32Ref(g_rcTable).v[w], c, x); }
uint rcTableAdd(uint w, uint x) { return atomicAdd(RcU32Ref(g_rcTable).v[w], x); }
vec4 rcPathLoad(uint px, uint py, uint k) { return g_rcVerts[k]; }
void rcPathStore(uint px, uint py, uint k, vec4 v) { g_rcVerts[k] = v; }
void rcRecordStore(uint record, uint k, vec4 v) { RcVec4Ref(g_rcRecords).v[record * kRcRecordWords + k] = v; }

#include "radiance_cache_core.h"
#include "radiance_cache_path.h"

RcParams rcHookParams() {
    vec4 w[5];
    for (uint k = 0u; k < 5u; ++k) {
        w[k] = RcVec4Ref(g_rcParams).v[k];
    }
    return rcParamsUnpack(w);
}

bool rcHookBind(PtParams P) {
#ifdef RC_HOOK_QUERY
    if (g_rcParams == uint64_t(0) && (P.rcTableLo != 0u || P.rcTableHi != 0u)) {
        uint64_t a = (uint64_t(P.rcTableLo) << 8) | (uint64_t(P.rcTableHi) << 32);
        g_rcTable = a;
        g_rcParams = a;
    }
#endif
    return g_rcParams != uint64_t(0);
}

uint ptRadianceCacheVertex(PtParams P, uint px, uint py, uint bounce, uint vflags, PtSurface S, vec3 n, vec3 d,
                           float tHit, float prevPdf, vec3 thr, vec3 acc, inout vec3 cached) {
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

void ptRadianceCacheEnd(PtParams P, uint px, uint py, vec3 acc) {
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

#endif // FUSE_RELIGHT_RADIANCE_CACHE_HOOK_GLSL
