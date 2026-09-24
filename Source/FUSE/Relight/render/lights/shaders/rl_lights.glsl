// FUSE Relight RL-4.4: the Relight light model in GLSL, the fallback for hosts with glslangValidator only
// (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.2). Same single-source core as rl_lights.slang and the CPU reference
// (Relight/kernels/light_core.h); this file sets the GLSL dialect and adds the light-set helpers that read the table
// and the WP-7.1 tree through buffer device addresses. Needs GL_GOOGLE_include_directive and
// -I <Relight>/kernels -I <Renderer>/shaders/light_tree.
//
// Consumers:
//   RlLight L = rlLoadLight(tableAddress, index);                       // RelightLightsGpu::tableAddress()
//   RlSetSample s = rlSetSample(tableAddress, lightCount, treeHeader, p, n, u0, u1, u2);
//   float pdf = rlSetPdf(tableAddress, lightCount, treeHeader, p, n, light, wi);
// (declare the ring's table range and the tree slot StorageRead: RelightLightsGraphRefs).
#ifndef FUSE_RELIGHT_LIGHTS_GLSL
#define FUSE_RELIGHT_LIGHTS_GLSL
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#include "lt_common.glsl"

#define float3 vec3
#define float4 vec4
#define FUSE_RL_FN
#define FUSE_RL_CONST const
#define FUSE_RL_OUT(T) out T
#define FUSE_RL_LIGHT_WORDS(name) vec4 name[6]
#define FUSE_RL_D3D_WORDS(name) vec4 name[5]
#define FUSE_RL_STABLE_COS(x) cos(x)
#define FUSE_RL_DISCRIMINANT(a, b, c) ((b) * (b) - 4.0f * (a) * (c))

#include "light_core.h"

layout(buffer_reference, std430, buffer_reference_align = 16) buffer RlWordsRef { vec4 v[]; };

RlLight rlLoadLight(uint64_t table, uint index) {
    RlWordsRef t = RlWordsRef(table);
    vec4 w[6];
    for (uint k = 0u; k < 6u; ++k) {
        w[k] = t.v[index * kRlLightWords + k];
    }
    return rlLightUnpack(w);
}

struct RlSetSample {
    uint light;     // LT_INVALID: none
    float pmf;
    float pdf;      // pmf x the light's solid angle pdf (delta: pmf)
    RlLightSample s;
};

RlSetSample rlSetSample(uint64_t table, uint lightCount, uint64_t tree, vec3 p, vec3 n, float u0, float u1, float u2) {
    RlSetSample r;
    r.light = LT_INVALID;
    r.pmf = 0.0;
    r.pdf = 0.0;
    r.s = rlLightSampleInvalid();
    LtSampleResult ts = lt_sample(tree, p, n, u0, u1, u2);
    if (ts.light >= lightCount) {
        return r;
    }
    RlLight L = rlLoadLight(table, ts.light);
    r.s = rlLightSample(L, p, u1, u2);
    r.light = ts.light;
    r.pmf = ts.pmf;
    r.pdf = (r.s.flags & kRlSampleDelta) != 0u ? ts.pmf : ts.pmf * r.s.pdf;
    return r;
}

float rlSetPdf(uint64_t table, uint lightCount, uint64_t tree, vec3 p, vec3 n, uint light, vec3 wi) {
    if (light >= lightCount) {
        return 0.0;
    }
    return lt_pmf(tree, p, n, light) * rlLightPdf(rlLoadLight(table, light), p, wi);
}

#endif // FUSE_RELIGHT_LIGHTS_GLSL
