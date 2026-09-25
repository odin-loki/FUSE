// FUSE Relight RL-5.3: ptRestirGiVertex for the Slang path-tracing core (included by rl_pt.slang before
// pt_reference_core.h). See restir_gi_core.h.
//   record mode (kPtFlagGiRecord; "relight.restir_gi.surface"): stash the G-buffer vertex and its index, stop the path;
//   apply mode (kPtFlagRestirGi; "relight.pt.trace"): at the vertex whose (instance, primitive, barycentrics) match
//   the per-pixel ReSTIR GI output at RGI_HOOK_OUTPUT (kRgiOutWords float4), `indirect` = its shaded estimate.
// RGI_HOOK_OUTPUT: the output's device address; the trace pass defines it as pc.restirGi before including
// rl_pt.slang, every other includer gets 0 (apply mode off).
#pragma once

#ifndef RGI_HOOK_OUTPUT
#define RGI_HOOK_OUTPUT uint64_t(0)
#endif

static bool g_rgiHave = false;
static PtRawHit g_rgiHit;
static float3 g_rgiDir = float3(0.0, 0.0, 0.0);
static uint g_rgiBounce = 0u;

uint ptRestirGiVertex(PtParams P, uint px, uint py, PtRawHit h, float3 d, uint bounce, inout float3 indirect) {
    if ((P.flags & kPtFlagGiRecord) != 0u) {
        g_rgiHave = true;
        g_rgiHit = h;
        g_rgiDir = d;
        g_rgiBounce = bounce;
        return 2u;
    }
    uint64_t a = RGI_HOOK_OUTPUT;
    if (a == 0) {
        return 0u;
    }
    uint i = py * P.width + px;
    float4 k = ((float4*)a)[i * 2u];
    float4 v = ((float4*)a)[i * 2u + 1u];
    if (!(v.w > 0.5) || k.x != float(h.instance) || k.y != float(h.primitive) || k.z != h.u || k.w != h.v) {
        return 0u;
    }
    indirect = float3(v.x, v.y, v.z);
    return 1u;
}
