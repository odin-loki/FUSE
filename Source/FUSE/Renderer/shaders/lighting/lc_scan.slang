// WP-2.1 "light.scan". Slang twin of lc_scan.comp (see there).
#include "lc_includes.slang"

groupshared uint s_scan[256];
groupshared uint s_atCapacity;
groupshared uint s_nonEmpty;
groupshared uint s_dropped;

[numthreads(256, 1, 1)]
void main(uint t: SV_GroupIndex) {
    const LcFrame F = fuseLcFrame();
    if (t == 0u) {
        s_atCapacity = 0u;
        s_nonEmpty = 0u;
        s_dropped = 0u;
    }
    uint* counts = fuseLcWords(F.clusterCounts);
    uint* droppedWords = fuseLcWords(F.clusterDropped);
    uint* grid = fuseLcWords(F.grid);
    uint running = 0u;
    uint atCapacity = 0u;
    uint nonEmpty = 0u;
    uint dropped = 0u;
    for (uint base = 0u; base < F.clusterCount; base += 256u) {
        const uint c = base + t;
        const uint count = c < F.clusterCount ? counts[c] : 0u;
        const uint drop = c < F.clusterCount ? droppedWords[c] : 0u;
        s_scan[t] = count;
        GroupMemoryBarrierWithGroupSync();
        for (uint offset = 1u; offset < 256u; offset <<= 1u) {
            const uint v = t >= offset ? s_scan[t - offset] : 0u;
            GroupMemoryBarrierWithGroupSync();
            s_scan[t] += v;
            GroupMemoryBarrierWithGroupSync();
        }
        if (c < F.clusterCount) {
            grid[c * 2u] = running + s_scan[t] - count;
            grid[c * 2u + 1u] = count;
        }
        atCapacity += drop != 0u ? 1u : 0u;
        nonEmpty += count != 0u ? 1u : 0u;
        dropped += drop;
        const uint total = s_scan[255];
        GroupMemoryBarrierWithGroupSync();
        running += total;
    }
    InterlockedAdd(s_atCapacity, atCapacity);
    InterlockedAdd(s_nonEmpty, nonEmpty);
    InterlockedAdd(s_dropped, dropped);
    GroupMemoryBarrierWithGroupSync();
    if (t == 0u) {
        uint* header = fuseLcWords(F.listHeader);
        header[kFuseLcHeaderTotal] = running;
        header[kFuseLcHeaderAtCapacity] = s_atCapacity;
        header[kFuseLcHeaderDropped] = s_dropped;
        header[kFuseLcHeaderNonEmpty] = s_nonEmpty;
        header[kFuseLcHeaderClusters] = F.clusterCount;
        header[kFuseLcHeaderCapacity] = F.capacity;
        header[kFuseLcHeaderLights] = F.lightCount;
    }
}
