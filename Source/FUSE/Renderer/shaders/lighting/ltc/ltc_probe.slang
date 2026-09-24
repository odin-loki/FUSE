// WP-2.2 GPU gate kernel: Slang twin of ltc_probe.comp (see there).
#include "gpu_scene.slang"
#include "lc_common.slang"
#include "lc_ltc.slang"

struct FuseLtcProbeCase {
    GpuLight light;
    float position[3];
    float roughness;
    float normal[3];
    float metallic;
    float albedo[3];
    float pad0;
    float view[3];
    float pad1;
};

struct FuseLtcProbePush {
    uint64_t cases;
    uint64_t lut;
    uint64_t out_;
    uint count;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<FuseLtcProbePush> pc;

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pc.count) {
        return;
    }
    const FuseLtcProbeCase c = ((FuseLtcProbeCase*)pc.cases)[i];
    LcSurface s;
    s.position = fuseLcVec3(c.position);
    s.normal = fuseLcVec3(c.normal);
    s.albedo = fuseLcVec3(c.albedo);
    s.roughness = c.roughness;
    s.metallic = c.metallic;
    s.ao = 1.0;
    s.emissive = float3(0.0, 0.0, 0.0);
    const float3 v = fuseLcVec3(c.view);
    float* lut = (float*)pc.lut;
    const FuseLcTerms t = fuseLcSurfaceTerms(s, v, lut);
    ((float4*)pc.out_)[i] = float4(fuseLcLightMs(c.light, s, v, t, lut), 0.0);
}
