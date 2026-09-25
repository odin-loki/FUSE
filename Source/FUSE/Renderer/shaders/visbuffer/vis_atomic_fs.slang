// WP-1.4 visibility-buffer raster, 64-bit atomic target. Slang twin of vis_atomic.frag (see there);
// FUSE_VIS64_IMAGE selects the R64_UINT storage image, else the u64 storage buffer.
#include "vis_format.slang"

#if defined(FUSE_VIS64_IMAGE)
[[vk::binding(0, 0)]] [format("r64ui")] RWTexture2D<uint64_t> fuse_vis64_images[];
#else
[[vk::binding(3, 0)]] RWStructuredBuffer<uint64_t> fuse_vis64_buffers[];
#endif

struct Push {
    float4 viewProj[4];
    uint scene;
    uint target64;
    uint width;
    uint height;
    uint pad[4];
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

struct FSIn {
    [[vk::location(0)]] nointerpolation uint instance : INSTANCE;
};

[shader("fragment")]
void main(FSIn input, float4 fragCoord : SV_Position, uint primitive : SV_PrimitiveID) {
    const uint64_t value = fuseVis64Pack(fragCoord.z, input.instance, primitive);
    const uint2 p = uint2(fragCoord.xy);
    const uint t = pc.target64 & 0xFFFFFu;
#if defined(FUSE_VIS64_IMAGE)
    InterlockedMin(fuse_vis64_images[t][p], value);
#else
    InterlockedMin(fuse_vis64_buffers[t][p.y * pc.width + p.x], value);
#endif
}
