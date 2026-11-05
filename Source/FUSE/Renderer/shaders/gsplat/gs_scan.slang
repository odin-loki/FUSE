// WP-9.2 "gsplat.scan". Slang twin of gs_scan.comp (see there: pc.reserved phases 0 / 1 / 2 / 3).
#include "gs_common.slang"

groupshared uint s_scan[256];

uint gsScanGroup(uint tid, uint v) {
    s_scan[tid] = v;
    GroupMemoryBarrierWithGroupSync();
    for (uint off = 1u; off < kGsGroup; off <<= 1u) {
        const uint add = tid >= off ? s_scan[tid - off] : 0u;
        GroupMemoryBarrierWithGroupSync();
        s_scan[tid] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    return s_scan[tid];
}

uint gsTouched(uint i) { return i < F.splatCount ? gsTilesTouched(gsProjected(i, 3u)) : 0u; }

void gsFinish(uint tid, uint carry) {
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

void gsClearRanges(uint tid) {
    uint* ranges = gsUints(F.ranges);
    for (uint t = tid; t < F.tileCount; t += kGsGroup) {
        ranges[t * 2u] = 0u;
        ranges[t * 2u + 1u] = 0u;
    }
}

[numthreads(256, 1, 1)]
void main(uint3 ltid: SV_GroupThreadID, uint3 group: SV_GroupID) {
    gsBegin();
    const uint tid = ltid.x;
    const uint mode = pc.reserved;
    const uint n = F.splatCount;
    uint* offsets = gsUints(F.offsets);
    uint* blocks = gsUints(uint64_t(F.reserved0) | (uint64_t(F.reserved1) << 32));
    if (mode == 1u) {
        const uint g = group.x;
        const uint total = gsScanGroup(tid, gsTouched(g * kGsGroup + tid));
        if (tid == kGsGroup - 1u) {
            blocks[g] = total;
        }
        return;
    }
    if (mode == 3u) {
        const uint g = group.x;
        const uint i = g * kGsGroup + tid;
        const uint v = gsTouched(i);
        const uint inclusive = gsScanGroup(tid, v);
        if (i < n) {
            offsets[i] = blocks[g] + inclusive - v;
        }
        return;
    }
    gsClearRanges(tid);
    uint carry = 0u;
    if (mode == 2u) {
        const uint blockCount = (n + kGsGroup - 1u) / kGsGroup;
        for (uint base = 0u; base < blockCount; base += kGsGroup) {
            const uint b = base + tid;
            const uint v = b < blockCount ? blocks[b] : 0u;
            const uint inclusive = gsScanGroup(tid, v);
            if (b < blockCount) {
                blocks[b] = carry + inclusive - v;
            }
            carry += s_scan[kGsGroup - 1u];
            GroupMemoryBarrierWithGroupSync();
        }
    } else {
        for (uint base = 0u; base < n; base += kGsGroup) {
            const uint i = base + tid;
            const uint v = gsTouched(i);
            const uint inclusive = gsScanGroup(tid, v);
            if (i < n) {
                offsets[i] = carry + inclusive - v;
            }
            carry += s_scan[kGsGroup - 1u];
            GroupMemoryBarrierWithGroupSync();
        }
    }
    gsFinish(tid, carry);
}
