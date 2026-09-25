// WP-3.1 "vsm.mark". Slang twin of vsm_mark.comp (see there).
#include "vsm_includes.slang"

[[vk::binding(1, 0)]] Texture2D<float> fuse_textures_2d[];

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    VsmConstants* C = fuseVsmConstants();
    if (tid.x >= C.depthWidth || tid.y >= C.depthHeight) {
        return;
    }
    const float depth = fuse_textures_2d[fuseVsmHandleIndex(C.depthTexture)].Load(int3(int(tid.x), int(tid.y), 0));
    uint pages[4];
    const uint n = fuseVsmMarkPixel(C, tid.x, tid.y, depth, pages);
    const uint offRequest = C.offRequest;
    const uint work = fuseVsmHandleIndex(C.workHandle);
    for (uint i = 0u; i < n; ++i) {
        const uint v = pages[i];
        InterlockedOr(fuse_vsm_words[work][offRequest + v / 32u], 1u << (v % 32u));
    }
}
