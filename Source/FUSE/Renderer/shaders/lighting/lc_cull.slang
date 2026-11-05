// WP-2.1 "light.cull". Slang twin of lc_cull.comp (see there).
#include "lc_includes.slang"

groupshared uint s_scan[64];

[numthreads(64, 1, 1)]
void main(uint3 group: SV_GroupID, uint t: SV_GroupIndex) {
    const LcFrame F = fuseLcFrame();
    const uint c = group.x;
    if (c >= F.clusterCount) {
        return; // workgroup-uniform
    }
    const uint slice = c % F.slicesZ;
    const LcAabb aabb = ((LcAabb*)F.aabbs)[c];
    LcBounds* bounds = (LcBounds*)F.bounds;
    uint* candidates = fuseLcWords(F.sliceLights + uint64_t(slice) * uint64_t(F.lightCapacity) * 4ull);
    const uint candidateCount = fuseLcWords(F.sliceCounts)[slice];
    uint* slots = fuseLcWords(F.clusterSlots + uint64_t(c) * uint64_t(F.capacity) * 4ull);
    uint running = 0u;
    for (uint base = 0u; base < candidateCount; base += 64u) {
        const uint i = base + t;
        uint light = 0u;
        uint flag = 0u;
        if (i < candidateCount) {
            light = candidates[i];
            const LcBounds b = bounds[light];
            flag = fuseLcSphereAabb(fuseLcVec3(b.center), b.radius, aabb) ? 1u : 0u;
        }
        s_scan[t] = flag;
        GroupMemoryBarrierWithGroupSync();
        for (uint offset = 1u; offset < 64u; offset <<= 1u) {
            const uint v = t >= offset ? s_scan[t - offset] : 0u;
            GroupMemoryBarrierWithGroupSync();
            s_scan[t] += v;
            GroupMemoryBarrierWithGroupSync();
        }
        const uint index = running + s_scan[t] - 1u;
        if (flag != 0u && index < F.capacity) {
            slots[index] = light;
        }
        const uint total = s_scan[63];
        GroupMemoryBarrierWithGroupSync();
        running += total;
    }
    if (t == 0u) {
        fuseLcWords(F.clusterCounts)[c] = min(running, F.capacity);
        fuseLcWords(F.clusterDropped)[c] = running > F.capacity ? running - F.capacity : 0u;
    }
}
