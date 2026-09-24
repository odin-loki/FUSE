// WP-1.5 material resolve, fragment stage. Slang twin of mr_resolve.frag (see there).
#include "mr_includes.slang"

[[vk::constant_id(0)]] const uint kFuseMrBinId = 4u;

[[vk::binding(0, 0)]] [format("rg32ui")] RWTexture2D<uint2> fuse_vis_images[];

struct FSOut {
    float4 rt0 : SV_Target0;
    float4 rt1 : SV_Target1;
    float4 rt2 : SV_Target2;
    float4 rt3 : SV_Target3;
    float4 rt4 : SV_Target4;
    float4 rt5 : SV_Target5;
    uint material : SV_Target6;
};

[shader("fragment")]
FSOut main(float4 fragCoord : SV_Position) {
    const MrFrame f = fuseMrFrame();
    const uint2 p = uint2(fragCoord.xy);
    const uint2 v = fuse_vis_images[f.vis & 0xFFFFFu][p];
    GpuSceneHeader* scene = fuseGpuScene(f.scene);
    const MrAttributes a = fuseMrAttributes(scene, f, p, v);
    MrGBuffer g = fuseMrGBufferEmpty();
    if (a.flags == kFuseMrAttrOk) {
        g = fuseMrShade(scene, a.material, f.sampler_, fuseMrBinFeatures(kFuseMrBinId), a.uv, a.duvdx, a.duvdy, a.normal,
                        a.tangent, a.tangentSign, a.depth, a.velocity);
    }
    FSOut o;
    o.rt0 = g.rt0;
    o.rt1 = g.rt1;
    o.rt2 = g.rt2;
    o.rt3 = g.rt3;
    o.rt4 = g.rt4;
    o.rt5 = g.rt5;
    o.material = g.material;
    return o;
}
