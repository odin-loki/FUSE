// WP-3.2 "vsm.probe". Slang twin of vsm_probe.comp (see there).
#include "vsm_raster_common.slang"

struct VsmrProbeIn {
    float position[3];
    uint slot;
    float normal[3];
    int level;
};

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pc.count) {
        return;
    }
    const VsmrProbeIn p = ((VsmrProbeIn*)pc.in_)[i];
    const VsmrSample r = fuseVsmrShadowSample(pc.shadow, p.slot, float3(p.position[0], p.position[1], p.position[2]),
                                              float3(p.normal[0], p.normal[1], p.normal[2]), p.level);
    ((float4*)pc.out_)[i] = float4(r.visibility, asfloat(r.level), r.receiverDepth, r.margin);
}
