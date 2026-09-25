// FUSE Relight RL-5.2: ptRestirDiVertex for the Slang path-tracing core (included by rl_pt.slang before
// pt_reference_core.h; the includer's push constants `pc` carry `restirDi`). See restir_di_core.h.
//   record mode (kPtFlagDiRecord; "relight.restir_di.surface"): stash the G-buffer vertex, stop the path;
//   apply mode (kPtFlagRestirDi; "relight.pt.trace"): at the vertex whose (instance, primitive, barycentrics) match
//   the per-pixel ReSTIR output at pc.restirDi (kRdiOutWords float4), `direct` = its shaded estimate.
#pragma once

static bool g_rdiHave = false;
static PtRawHit g_rdiHit;
static float3 g_rdiDir = float3(0.0, 0.0, 0.0);

uint ptRestirDiVertex(PtParams P, uint px, uint py, PtRawHit h, float3 d, inout float3 direct) {
    if ((P.flags & kPtFlagDiRecord) != 0u) {
        g_rdiHave = true;
        g_rdiHit = h;
        g_rdiDir = d;
        return 2u;
    }
    if (pc.restirDi == 0) {
        return 0u;
    }
    uint i = py * P.width + px;
    float4 k = ((float4*)pc.restirDi)[i * 2u];
    float4 v = ((float4*)pc.restirDi)[i * 2u + 1u];
    if (!(v.w > 0.5) || k.x != float(h.instance) || k.y != float(h.primitive) || k.z != h.u || k.w != h.v) {
        return 0u;
    }
    direct = float3(v.x, v.y, v.z);
    return 1u;
}
