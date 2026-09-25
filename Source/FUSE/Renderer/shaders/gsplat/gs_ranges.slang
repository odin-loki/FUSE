// WP-9.2 "gsplat.ranges". Slang twin of gs_ranges.comp (see there).
#include "gs_common.slang"

[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    gsBegin();
    const uint i = tid.x;
    const uint count = min(gsUints(F.counters)[0], F.capacity);
    if (i >= count) {
        return;
    }
    uint64_t* keys = gsKeys(F.keys);
    uint* ranges = gsUints(F.ranges);
    const uint tile = uint(keys[i] >> 32);
    if (i == 0u || uint(keys[i - 1u] >> 32) != tile) {
        ranges[tile * 2u] = i;
    }
    if (i + 1u == count || uint(keys[i + 1u] >> 32) != tile) {
        ranges[tile * 2u + 1u] = i + 1u;
    }
}
