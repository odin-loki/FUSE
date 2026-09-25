// WP-1.5 attribute dump. Slang twin of mr_attributes.comp (see there).
#include "mr_includes.slang"

[[vk::binding(0, 0)]] [format("rg32ui")] RWTexture2D<uint2> fuse_vis_images[];

struct MrAttributeTexel {
    float b1;
    float b2;
    float depth;
    uint flags;
    float db1dx;
    float db1dy;
    float db2dx;
    float db2dy;
    float2 uv;
    float2 duvdx;
    float2 duvdy;
    uint material;
    uint bin;
    float3 normal;
    float tangentSign;
    float3 tangent;
    float pad0;
    float2 velocity;
    float2 pad1;
};

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const MrFrame f = fuseMrFrame();
    const uint2 p = tid.xy;
    if (p.x >= f.width || p.y >= f.height) {
        return;
    }
    const uint2 v = fuse_vis_images[f.vis & 0xFFFFFu][p];
    const MrAttributes a = fuseMrAttributes(fuseGpuScene(f.scene), f, p, v);
    MrAttributeTexel o;
    o.b1 = a.bary.b.y;
    o.b2 = a.bary.b.z;
    o.depth = a.depth;
    o.flags = a.flags;
    o.db1dx = a.bary.dbdx.y;
    o.db1dy = a.bary.dbdy.y;
    o.db2dx = a.bary.dbdx.z;
    o.db2dy = a.bary.dbdy.z;
    o.uv = a.uv;
    o.duvdx = a.duvdx;
    o.duvdy = a.duvdy;
    o.material = a.material;
    o.bin = a.bin;
    o.normal = a.normal;
    o.tangentSign = a.tangentSign;
    o.tangent = a.tangent;
    o.pad0 = 0.0;
    o.velocity = a.velocity;
    o.pad1 = float2(0.0);
    MrAttributeTexel* out = (MrAttributeTexel*)pc.out_;
    out[p.y * f.width + p.x] = o;
}
