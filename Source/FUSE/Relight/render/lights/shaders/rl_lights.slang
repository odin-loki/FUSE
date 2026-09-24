// FUSE Relight RL-4.4: the Relight light model in Slang (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.2). The body is the
// single-source core Relight/kernels/light_core.h (the CPU reference compiles the same text through
// kernels/light_cpp.hpp); this file sets the Slang dialect and adds the light-set helpers (twins of rl_lights.glsl).
// Compile with -I <Relight>/kernels -I <Renderer>/shaders/light_tree and -fp-mode precise (the WP-7.1 sampler must
// return the CPU tree's bits).
#pragma once

#include "lt_common.slang"

#define FUSE_RL_FN
#define FUSE_RL_CONST static const
#define FUSE_RL_OUT(T) out T
#define FUSE_RL_LIGHT_WORDS(name) float4 name[6]
#define FUSE_RL_D3D_WORDS(name) float4 name[5]
#define FUSE_RL_STABLE_COS(x) cos(x)
#define FUSE_RL_DISCRIMINANT(a, b, c) ((b) * (b) - 4.0f * (a) * (c))

#include "light_core.h"

RlLight rlLoadLight(uint64_t table, uint index) {
    float4* t = (float4*)table;
    float4 w[6];
    for (uint k = 0u; k < 6u; ++k) {
        w[k] = t[index * kRlLightWords + k];
    }
    return rlLightUnpack(w);
}

struct RlSetSample {
    uint light;
    float pmf;
    float pdf;
    RlLightSample s;
};

RlSetSample rlSetSample(uint64_t table, uint lightCount, uint64_t tree, float3 p, float3 n, float u0, float u1, float u2) {
    RlSetSample r;
    r.light = kLtInvalid;
    r.pmf = 0.0;
    r.pdf = 0.0;
    r.s = rlLightSampleInvalid();
    LtSampleResult ts = ltSample(tree, p, n, u0, u1, u2);
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

float rlSetPdf(uint64_t table, uint lightCount, uint64_t tree, float3 p, float3 n, uint light, float3 wi) {
    if (light >= lightCount) {
        return 0.0;
    }
    return ltPmf(tree, p, n, light) * rlLightPdf(rlLoadLight(table, light), p, wi);
}
