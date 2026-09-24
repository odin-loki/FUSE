// WP-2.1 "light.bin". Slang twin of lc_bin.comp (see there).
#include "lc_includes.slang"

groupshared uint s_scan[64];

[numthreads(64, 1, 1)]
void main(uint3 group: SV_GroupID, uint t: SV_GroupIndex) {
    const LcFrame F = fuseLcFrame();
    const uint w = group.x;
    const bool directional = w == F.slicesZ;
    LcBounds* bounds = (LcBounds*)F.bounds;
    uint* list = fuseLcWords(directional ? F.directional : F.sliceLights + uint64_t(w) * uint64_t(F.lightCapacity) * 4ull);
    uint running = 0u;
    for (uint base = 0u; base < F.lightCount; base += 64u) {
        const uint light = base + t;
        uint flag = 0u;
        if (light < F.lightCount) {
            const LcBounds b = bounds[light];
            flag = directional ? (b.type == kFuseLightDirectional ? 1u : 0u) : ((w >= b.sliceLo && w <= b.sliceHi) ? 1u : 0u);
        }
        s_scan[t] = flag;
        GroupMemoryBarrierWithGroupSync();
        for (uint offset = 1u; offset < 64u; offset <<= 1u) {
            const uint v = t >= offset ? s_scan[t - offset] : 0u;
            GroupMemoryBarrierWithGroupSync();
            s_scan[t] += v;
            GroupMemoryBarrierWithGroupSync();
        }
        if (flag != 0u) {
            list[running + s_scan[t] - 1u] = light;
        }
        const uint total = s_scan[63];
        GroupMemoryBarrierWithGroupSync();
        running += total;
    }
    if (t == 0u) {
        if (directional) {
            fuseLcWords(F.listHeader)[kFuseLcHeaderDirectional] = running;
        } else {
            fuseLcWords(F.sliceCounts)[w] = running;
        }
    }
}
