// FUSE Relight RL-5.2: ptRestirDiVertex for the GLSL path-tracing core (included by rl_pt.glsl before
// pt_reference_core.h). GLSL twin of restir_di_hook.slang (see there and restir_di_core.h).
#ifndef FUSE_RELIGHT_RESTIR_DI_HOOK_GLSL
#define FUSE_RELIGHT_RESTIR_DI_HOOK_GLSL

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RdiHookOutRef { vec4 v[]; };

bool g_rdiHave = false;
PtRawHit g_rdiHit;
vec3 g_rdiDir = vec3(0.0);

uint ptRestirDiVertex(PtParams P, uint px, uint py, PtRawHit h, vec3 d, inout vec3 direct) {
    if ((P.flags & kPtFlagDiRecord) != 0u) {
        g_rdiHave = true;
        g_rdiHit = h;
        g_rdiDir = d;
        return 2u;
    }
    if (pc.restirDi == uint64_t(0)) {
        return 0u;
    }
    uint i = py * P.width + px;
    vec4 k = RdiHookOutRef(pc.restirDi).v[i * 2u];
    vec4 v = RdiHookOutRef(pc.restirDi).v[i * 2u + 1u];
    if (!(v.w > 0.5) || k.x != float(h.instance) || k.y != float(h.primitive) || k.z != h.u || k.w != h.v) {
        return 0u;
    }
    direct = v.xyz;
    return 1u;
}

#endif // FUSE_RELIGHT_RESTIR_DI_HOOK_GLSL
