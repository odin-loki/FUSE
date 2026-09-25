// FUSE Relight RL-5.1: the path-tracing core in Slang (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.1). The body is the
// single-source core Relight/kernels/pt_reference_core.h (the CPU reference compiles the same text through
// kernels/pt_reference_cpp.hpp); this file sets the Slang dialect and the scene accessors (twins of rl_pt.glsl: ray
// query on the WP-6.0 TLAS, tables by device address, RL-4.4 rl_lights.slang, RL-4.3 bsdf.slang, the WP-0.4 bindless
// heap for textures). The includer declares the push constants `pc` (PtTracePush) first. Compile with -fp-mode precise
// and -I <Relight>/kernels, <Relight>/shaders/material, <Relight>/render/lights/shaders, <Renderer>/shaders/light_tree.
#pragma once

#define FUSE_BSDF_LUT_READ(i) ((float*)pc.lut)[(i)]
#include "bsdf.slang"
#include "rl_lights.slang"

[[vk::binding(1, 0)]] Texture2D<float4> g_ptTextures[];
[[vk::binding(2, 0)]] SamplerState g_ptSamplers[];

#define PT_FN
#define PT_CONST static const
#define PT_OUT(T) out T
#define PT_INOUT(T) inout T
#define PT_CTX_PARAM
#define PT_CTX_ARG
#define PT_LUT_ARG
#define PT_L3(v) (v)
#define PT_B3(v) (v)
#define PT_PARAM_WORDS(name) float4 name[11]

#include "pt_reference_types.h"

PtRawHit ptRawTrace(float3 o, float3 d, float tmin, float tmax, uint cullMask) {
    PtRawHit h;
    h.hit = false;
    h.t = 0.0;
    h.instance = kPtInvalid;
    h.primitive = kPtInvalid;
    h.u = 0.0;
    h.v = 0.0;
    RayDesc desc;
    desc.Origin = o;
    desc.TMin = tmin;
    desc.Direction = d;
    desc.TMax = tmax;
    RaytracingAccelerationStructure tlas = RaytracingAccelerationStructure(pc.tlas);
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(tlas, RAY_FLAG_FORCE_OPAQUE, cullMask & 0x7Fu, desc);
    while (q.Proceed()) {
    }
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        float2 bary = q.CommittedTriangleBarycentrics();
        h.hit = true;
        h.t = q.CommittedRayT();
        h.instance = q.CommittedInstanceID();
        h.primitive = q.CommittedPrimitiveIndex();
        h.u = bary.x;
        h.v = bary.y;
    }
    return h;
}

float4 ptInstanceWord(uint slot, uint k) { return ((float4*)pc.instances)[slot * kPtInstanceWords + k]; }
float4 ptTriangleWord(uint tri, uint k) { return ((float4*)pc.triangles)[tri * kPtTriangleWords + k]; }
float4 ptMaterialWord(uint material, uint k) { return ((float4*)pc.materials)[material * kPtMaterialWords + k]; }
float4 ptPortalWord(uint portal, uint k) { return ((float4*)pc.portals)[portal * kPtPortalWords + k]; }
float ptLightMapValue(uint index) { return ((float*)pc.lightMap)[index]; }

float4 ptTextureSample(uint texture, uint smp, float u, float v) {
    // WP-0.4 handle: index in bits 0..19, type in bits 20..23 (0: invalid).
    if (((texture >> 20u) & 0xFu) == 0u || ((smp >> 20u) & 0xFu) == 0u) {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    return g_ptTextures[NonUniformResourceIndex(texture & 0xFFFFFu)].SampleLevel(
        g_ptSamplers[NonUniformResourceIndex(smp & 0xFFFFFu)], float2(u, v), 0.0);
}

RlLight ptLoadLight(uint light) { return rlLoadLight(pc.lightTable, light); }

PtLightPick ptSampleLightSet(float3 p, float3 n, float u0, float u1, float u2) {
    PtLightPick pick;
    pick.light = kPtInvalid;
    pick.pdf = 0.0;
    pick.wi = float3(0.0, 0.0, 1.0);
    pick.dist = 0.0;
    pick.radiance = float3(0.0, 0.0, 0.0);
    pick.delta = false;
    RlSetSample s = rlSetSample(pc.lightTable, pc.lightCount, pc.lightTree, p, n, u0, u1, u2);
    if (s.light == kLtInvalid || s.light >= pc.lightCount || (s.s.flags & kRlSampleValid) == 0u) {
        return pick;
    }
    pick.light = s.light;
    pick.pdf = s.pdf;
    pick.wi = s.s.wi;
    pick.dist = s.s.dist;
    pick.radiance = s.s.radiance;
    pick.delta = (s.s.flags & kRlSampleDelta) != 0u;
    return pick;
}

float ptLightSetPdf(float3 p, float3 n, uint light, float3 wi) {
    return rlSetPdf(pc.lightTable, pc.lightCount, pc.lightTree, p, n, light, wi);
}

// RL-5.2: ptRestirDiVertex (ReSTIR DI record / apply hook; reads pc.restirDi).
#include "restir_di_hook.slang"
// RL-5.3: ptRestirGiVertex (ReSTIR GI record / apply hook; reads RGI_HOOK_OUTPUT).
#include "restir_gi_hook.slang"
// RL-5.4: ptRadianceCacheVertex / ptRadianceCacheEnd (hash-grid radiance cache; queries when RC_HOOK_QUERY).
#include "radiance_cache_hook.slang"

#include "pt_reference_core.h"
