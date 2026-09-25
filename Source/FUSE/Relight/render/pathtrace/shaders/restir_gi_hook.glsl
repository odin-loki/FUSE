// FUSE Relight RL-5.3: ptRestirGiVertex for the GLSL path-tracing core (included by rl_pt.glsl before
// pt_reference_core.h). GLSL twin of restir_gi_hook.slang (see there and restir_gi_core.h).
#ifndef FUSE_RELIGHT_RESTIR_GI_HOOK_GLSL
#define FUSE_RELIGHT_RESTIR_GI_HOOK_GLSL

#ifndef RGI_HOOK_OUTPUT
#define RGI_HOOK_OUTPUT uint64_t(0)
#endif

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RgiHookOutRef { vec4 v[]; };

bool g_rgiHave = false;
PtRawHit g_rgiHit;
vec3 g_rgiDir = vec3(0.0);
uint g_rgiBounce = 0u;

uint ptRestirGiVertex(PtParams P, uint px, uint py, PtRawHit h, vec3 d, uint bounce, inout vec3 indirect) {
    if ((P.flags & kPtFlagGiRecord) != 0u) {
        g_rgiHave = true;
        g_rgiHit = h;
        g_rgiDir = d;
        g_rgiBounce = bounce;
        return 2u;
    }
    uint64_t a = RGI_HOOK_OUTPUT;
    if (a == uint64_t(0)) {
        return 0u;
    }
    uint i = py * P.width + px;
    vec4 k = RgiHookOutRef(a).v[i * 2u];
    vec4 v = RgiHookOutRef(a).v[i * 2u + 1u];
    if (!(v.w > 0.5) || k.x != float(h.instance) || k.y != float(h.primitive) || k.z != h.u || k.w != h.v) {
        return 0u;
    }
    indirect = v.xyz;
    return 1u;
}

#endif // FUSE_RELIGHT_RESTIR_GI_HOOK_GLSL
