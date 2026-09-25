// FUSE Relight RL-5.6 "relight.vol.*". Slang twin of rl_vol.comp (see there); compile with -fp-mode precise and
// -I <Relight>/kernels -I <Relight>/render/lights/shaders -I <Renderer>/shaders/light_tree.
struct VolPush {
    uint64_t block;
    uint64_t tlas;
    uint64_t lightTable;
    uint64_t lightTree;
    uint stage;
    uint count;
    uint reserved0;
    uint reserved1;
};
[[vk::push_constant]] ConstantBuffer<VolPush> pc;

#include "rl_lights.slang"

#define VOL_FN
#define VOL_CONST static const
#define VOL_OUT(T) out T
#define VOL_INOUT(T) inout T
#define VOL_CTX_PARAM
#define VOL_CTX_ARG
#define VOL_PARAM_WORDS(name) float4 name[11]
#define VOL_INT_WORDS(name) uint4 name[3]

#include "rl_vol_types.h"

uint64_t volAddress(uint slot) { return ((uint64_t*)pc.block)[slot]; }

float4 volLoad(uint buf, uint index) { return ((float4*)volAddress(buf))[index]; }
void volStore(uint buf, uint index, float4 v) { ((float4*)volAddress(buf))[index] = v; }
float volLoadDepth(uint pixel) {
    uint64_t a = volAddress(8u);
    return a != 0 ? ((float*)a)[pixel] : 0.0;
}
float volUnorm8(uint c, uint shift) { return float((c >> shift) & 255u) * (1.0 / 255.0); }
VolVertex volLoadVertex(uint vertex) {
    uint* w = (uint*)volAddress(9u);
    VolVertex v;
    v.position = float3(asfloat(w[vertex * 6u]), asfloat(w[vertex * 6u + 1u]), asfloat(w[vertex * 6u + 2u]));
    uint c = w[vertex * 6u + 3u];
    v.color = float4(volUnorm8(c, 16u), volUnorm8(c, 8u), volUnorm8(c, 0u), volUnorm8(c, 24u));
    return v;
}
uint4 volLoadSystem(uint system) { return ((uint4*)(pc.block + 352))[system]; }
RlLight volLoadLight(uint light) { return rlLoadLight(pc.lightTable, light); }
VolLightPick volSampleLight(float3 p, float u0, float u1, float u2) {
    uint lightCount = ((uint4*)(pc.block + 304))[2].y;
    RlSetSample s = rlSetSample(pc.lightTable, lightCount, pc.lightTree, p, float3(0.0, 0.0, 0.0), u0, u1, u2);
    VolLightPick r;
    r.light = s.light;
    r.pdf = s.pdf;
    r.wi = s.s.wi;
    r.dist = s.s.dist;
    r.radiance = s.s.radiance;
    r.flags = s.s.flags;
    return r;
}
bool volOccluded(float3 o, float3 d, float tmin, float tmax) {
    if (pc.tlas == 0) {
        return false;
    }
    RaytracingAccelerationStructure tlas = RaytracingAccelerationStructure(pc.tlas);
    RayDesc desc;
    desc.Origin = o;
    desc.TMin = tmin;
    desc.Direction = d;
    desc.TMax = tmax;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(tlas, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 2u, desc);
    while (q.Proceed()) {
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

#include "rl_vol_core.h"

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    uint id = tid.x;
    if (id >= pc.count) {
        return;
    }
    float4 w[11];
    for (uint k = 0u; k < 11u; ++k) {
        w[k] = ((float4*)(pc.block + 128))[k];
    }
    uint4 iw[3];
    for (uint k = 0u; k < 3u; ++k) {
        iw[k] = ((uint4*)(pc.block + 304))[k];
    }
    VolParams P = volParamsUnpack(w, iw);
    if (pc.stage == kVolStageInject || pc.stage == kVolStageTemporal) {
        uint z = id % P.gz;
        uint col = id / P.gz;
        uint x = col % P.gx;
        uint y = col / P.gx;
        if (pc.stage == kVolStageInject) {
            volInject(P, x, y, z);
        } else {
            volTemporal(P, x, y, z);
        }
    } else if (pc.stage == kVolStageIntegrate) {
        volIntegrate(P, id % P.gx, id / P.gx);
    } else {
        volApply(P, id % P.width, id / P.width);
    }
}
