// WP-1.4 full-frame visibility decode. Slang twin of vis_decode.comp (see there).
#include "gpu_scene.slang"
#include "vis_format.slang"
#include "vis_common.slang"

[[vk::binding(0, 0)]] [format("rg32ui")] RWTexture2D<uint2> fuse_vis_images[];

struct VisDecodeTexel {
    float depth;
    float b1;
    float b2;
    uint flags;
};

struct Push {
    float4 viewProj[4];
    uint64_t out_;
    uint scene;
    uint vis;
    uint width;
    uint height;
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
    const uint2 v = fuse_vis_images[pc.vis & 0xFFFFFu][p];
    const VisDecoded d = fuseVisDecode(fuseGpuScene(pc.scene), pc.viewProj, v.x, v.y, fuseVisPixelNdc(p, pc.width, pc.height));
    VisDecodeTexel o;
    o.depth = d.depth;
    o.b1 = d.b1;
    o.b2 = d.b2;
    o.flags = d.flags;
    VisDecodeTexel* out = (VisDecodeTexel*)pc.out_;
    out[p.y * pc.width + p.x] = o;
}
