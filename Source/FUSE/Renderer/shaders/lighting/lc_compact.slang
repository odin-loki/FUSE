// WP-2.1 "light.compact". Slang twin of lc_compact.comp (see there).
#include "lc_includes.slang"

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const LcFrame F = fuseLcFrame();
    const uint c = tid.x;
    if (c >= F.clusterCount) {
        return;
    }
    uint* grid = fuseLcWords(F.grid);
    const uint offset = grid[c * 2u];
    const uint count = grid[c * 2u + 1u];
    uint* src = fuseLcWords(F.clusterSlots + uint64_t(c) * uint64_t(F.capacity) * 4ull);
    uint* dst = fuseLcWords(F.lightList);
    for (uint i = 0u; i < count; ++i) {
        dst[offset + i] = src[i];
    }
}
