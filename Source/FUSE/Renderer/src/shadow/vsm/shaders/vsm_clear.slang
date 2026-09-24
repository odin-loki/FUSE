// WP-3.1 "vsm.clear". Slang twin of vsm_clear.comp (see there).
#include "vsm_includes.slang"

[[vk::binding(0, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> fuse_vsm_pool[];

[numthreads(16, 16, 1)]
void main(uint3 group: SV_GroupID, uint3 lid: SV_GroupThreadID) {
    VsmConstants* C = fuseVsmConstants();
    const uint entry = group.x;
    const uint p = fuse_vsm_words[fuseVsmHandleIndex(C.workHandle)][C.offRenderList + entry * 2u + 1u];
    const uint2 page = uint2(p % C.poolPagesX, p / C.poolPagesX);
    const uint2 origin = page * kFuseVsmPageTexels + lid.xy * 8u;
    const uint pool = fuseVsmHandleIndex(C.poolHandle);
    const uint value = C.clearValue;
    for (uint y = 0u; y < 8u; ++y) {
        for (uint x = 0u; x < 8u; ++x) {
            fuse_vsm_pool[pool][origin + uint2(x, y)] = value;
        }
    }
}
