// WP-2.1 "light.cull". Slang twin of lc_cull.comp (see there).
#include "lc_includes.slang"

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const LcFrame F = fuseLcFrame();
    const uint c = tid.x;
    if (c >= F.clusterCount) {
        return;
    }
    const uint slice = c % F.slicesZ;
    const LcAabb aabb = ((LcAabb*)F.aabbs)[c];
    LcBounds* bounds = (LcBounds*)F.bounds;
    uint* candidates = fuseLcWords(F.sliceLights + uint64_t(slice) * uint64_t(F.lightCapacity) * 4ull);
    const uint candidateCount = fuseLcWords(F.sliceCounts)[slice];
    uint* slots = fuseLcWords(F.clusterSlots + uint64_t(c) * uint64_t(F.capacity) * 4ull);
    uint count = 0u;
    uint dropped = 0u;
    for (uint i = 0u; i < candidateCount; ++i) {
        const uint light = candidates[i];
        const LcBounds b = bounds[light];
        if (!fuseLcSphereAabb(fuseLcVec3(b.center), b.radius, aabb)) {
            continue;
        }
        if (count < F.capacity) {
            slots[count] = light;
            ++count;
        } else {
            ++dropped;
        }
    }
    fuseLcWords(F.clusterCounts)[c] = count;
    fuseLcWords(F.clusterDropped)[c] = dropped;
}
