// WP-1.5 material classification. Slang twin of mr_classify.comp (see there).
#include "mr_includes.slang"

[[vk::binding(0, 0)]] [format("rg32ui")] RWTexture2D<uint2> fuse_vis_images[];
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> fuse_mr_bin_buffers[];

groupshared uint s_bin;

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID, uint3 group: SV_GroupID, uint local: SV_GroupIndex) {
    const MrFrame f = fuseMrFrame();
    if (local == 0u) {
        s_bin = kFuseMrBinEmpty;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint2 p = tid.xy;
    if (p.x < f.width && p.y < f.height) {
        const uint2 v = fuse_vis_images[f.vis & 0xFFFFFu][p];
        const uint bin = fuseMrPixelBin(fuseGpuScene(f.scene), v);
        if (bin != kFuseMrBinEmpty) {
            InterlockedMax(s_bin, bin);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (local == 0u) {
        const uint bin = s_bin;
        const uint h = f.binsHandle & 0xFFFFFu;
        uint slot = 0u;
        InterlockedAdd(fuse_mr_bin_buffers[h][bin * 4u + 1u], 1u, slot);
        fuse_mr_bin_buffers[h][kFuseMrBinListOffsetWords + bin * f.tileCapacity + slot] = group.x | (group.y << 16u);
    }
}
