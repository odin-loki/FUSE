// WP-1.4 64-bit visibility target clear / export. Slang twin of vis64.comp (see there).
#include "vis_format.slang"

#if defined(FUSE_VIS64_IMAGE)
[[vk::binding(0, 0)]] [format("r64ui")] RWTexture2D<uint64_t> fuse_vis64_images[];
#else
[[vk::binding(3, 0)]] RWStructuredBuffer<uint64_t> fuse_vis64_buffers[];
#endif
[[vk::binding(0, 0)]] [format("rg32ui")] RWTexture2D<uint2> fuse_vis_images[];
[[vk::binding(0, 0)]] [format("r32f")] RWTexture2D<float> fuse_depth_images[];

struct Push {
    uint target64;
    uint vis;
    uint depth;
    uint width;
    uint height;
    uint mode;
    uint pad0;
    uint pad1;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint2 p = tid.xy;
    if (p.x >= pc.width || p.y >= pc.height) {
        return;
    }
    const uint t = pc.target64 & 0xFFFFFu;
    if (pc.mode == 0u) {
#if defined(FUSE_VIS64_IMAGE)
        fuse_vis64_images[t][p] = kFuseVis64Clear;
#else
        fuse_vis64_buffers[t][p.y * pc.width + p.x] = kFuseVis64Clear;
#endif
        return;
    }
#if defined(FUSE_VIS64_IMAGE)
    const uint64_t v = fuse_vis64_images[t][p];
#else
    const uint64_t v = fuse_vis64_buffers[t][p.y * pc.width + p.x];
#endif
    fuse_vis_images[pc.vis & 0xFFFFFu][p] = fuseVis64ToRaster(v);
    fuse_depth_images[pc.depth & 0xFFFFFu][p] = fuseVis64ExportDepth(v);
}
