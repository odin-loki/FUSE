// WP-5.4 hardware path, fragment stage. Slang twin of swraster_hw.frag (see there); FUSE_VIS64_IMAGE
// selects the R64_UINT storage image, else the u64 storage buffer.
#include "gpu_scene.slang"
#include "vis_format.slang"
#include "vis_common.slang"
#include "swraster_common.slang"

#if defined(FUSE_VIS64_IMAGE)
[[vk::binding(0, 0)]] [format("r64ui")] RWTexture2D<uint64_t> fuse_vis64_images[];
#else
[[vk::binding(3, 0)]] RWStructuredBuffer<uint64_t> fuse_vis64_buffers[];
#endif

struct Push {
    uint64_t constants;
    uint region;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

struct FSIn {
    [[vk::location(0)]] nointerpolation uint instance : INSTANCE;
    [[vk::location(1)]] nointerpolation uint triangle : TRIANGLE;
};

[shader("fragment")]
void main(FSIn input, float4 fragCoord : SV_Position) {
    SwConstants* C = (SwConstants*)pc.constants;
    const uint64_t value = fuseVis64Pack(fragCoord.z, input.instance, input.triangle);
    const uint2 p = uint2(fragCoord.xy);
    const uint t = fuseSwHandleIndex(C.target64);
#if defined(FUSE_VIS64_IMAGE)
    InterlockedMin(fuse_vis64_images[t][p], value);
#else
    InterlockedMin(fuse_vis64_buffers[t][p.y * C.width + p.x], value);
#endif
}
