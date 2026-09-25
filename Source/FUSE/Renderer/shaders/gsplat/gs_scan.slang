// WP-9.2 "gsplat.scan". Slang twin of gs_scan.comp (see there).
#include "gs_common.slang"

groupshared uint s_scan[256];

[numthreads(256, 1, 1)]
void main(uint3 ltid: SV_GroupThreadID) {
    gsBegin();
    const uint tid = ltid.x;
    uint* ranges = gsUints(F.ranges);
    for (uint t = tid; t < F.tileCount; t += kGsGroup) {
        ranges[t * 2u] = 0u;
        ranges[t * 2u + 1u] = 0u;
    }
    uint* offsets = gsUints(F.offsets);
    const uint n = F.splatCount;
    uint carry = 0u;
    for (uint base = 0u; base < n; base += kGsGroup) {
        const uint i = base + tid;
        const uint v = i < n ? gsTilesTouched(gsProjected(i, 3u)) : 0u;
        s_scan[tid] = v;
        GroupMemoryBarrierWithGroupSync();
        for (uint off = 1u; off < kGsGroup; off <<= 1u) {
            const uint add = tid >= off ? s_scan[tid - off] : 0u;
            GroupMemoryBarrierWithGroupSync();
            s_scan[tid] += add;
            GroupMemoryBarrierWithGroupSync();
        }
        if (i < n) {
            offsets[i] = carry + s_scan[tid] - v;
        }
        carry += s_scan[kGsGroup - 1u];
        GroupMemoryBarrierWithGroupSync();
    }
    if (tid == 0u) {
        uint* counters = gsUints(F.counters);
        counters[0] = carry;
        counters[1] = carry > F.capacity ? carry - F.capacity : 0u;
        counters[2] = 0u;
        counters[3] = 0u;
    }
    uint64_t* keys = gsKeys(F.keys);
    uint* values = gsUints(F.values);
    const uint64_t pad = (uint64_t(pc.padKeyHigh) << 32) | uint64_t(0xFFFFFFFFu);
    for (uint e = min(carry, F.capacity) + tid; e < F.capacity; e += kGsGroup) {
        keys[e] = pad;
        values[e] = kGsPadValue;
    }
}
