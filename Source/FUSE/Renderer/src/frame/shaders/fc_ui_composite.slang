// E02 "frame.ui_composite" (Slang primary; GLSL twin fc_ui_composite.comp; see include/fuse/renderer/frame/frame_ui.hpp):
// premultiplied UI over the display-referred scene, scene alpha kept. CPU reference: frame_ui_composite_texel.
// Compiled with -fp-mode precise.

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> fc_ui_target;
[[vk::binding(1, 0)]] Texture2D<float4> fc_ui_scene;
[[vk::binding(2, 0)]] Texture2D<float4> fc_ui_image;

struct FcUiPush {
    uint2 extent;
    uint2 pad;
};
[[vk::push_constant]] ConstantBuffer<FcUiPush> pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id: SV_DispatchThreadID) {
    const uint2 p = id.xy;
    if (p.x >= pc.extent.x || p.y >= pc.extent.y) {
        return;
    }
    const float4 s = fc_ui_scene.Load(int3(int2(p), 0));
    const float4 u = fc_ui_image.Load(int3(int2(p), 0));
    const float k = 1.0 - u.a;
    const float3 t = k * s.rgb;
    const float3 rgb = u.rgb + t;
    fc_ui_target[p] = float4(rgb, s.a);
}
