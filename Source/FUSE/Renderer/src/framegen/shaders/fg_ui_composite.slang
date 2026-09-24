// WP-4.4 "fg.ui_composite" (Slang primary; GLSL twin fg_ui_composite.comp): the UI composited separately over a frame —
// the interpolated HUD-less frame of the FSR 3.1 passes or the real HUD-less frame — like the FidelityFX
// frame-interpolation swapchain's UI composition: out = source * (1 - ui.a) + ui.rgb (premultiplied UI), alpha 1;
// without a UI texture the source is copied. Point loads only; compiled with -fp-mode precise (separate multiply and
// add, like the CPU twin fg_composite).

struct FgCompositePush
{
    uint width;
    uint height;
    uint hasUi;
    uint pad;
};

[[vk::binding(0, 0)]] Texture2D<float4> fgCompositeSource;
[[vk::binding(1, 0)]] Texture2D<float4> fgCompositeUi;
[[vk::binding(2, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> fgCompositeOut;

[[vk::push_constant]] ConstantBuffer<FgCompositePush> pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 p = id.xy;
    if (p.x >= pc.width || p.y >= pc.height)
    {
        return;
    }
    const float4 src = fgCompositeSource.Load(int3(int2(p), 0));
    float4 outColor = float4(src.rgb, 1.0);
    if (pc.hasUi != 0)
    {
        const float4 ui = fgCompositeUi.Load(int3(int2(p), 0));
        const float k = 1.0 - ui.a;
        const float3 scaled = src.rgb * k;
        const float3 sum = scaled + ui.rgb;
        outColor = float4(sum, 1.0);
    }
    fgCompositeOut[p] = outColor;
}
