// WP-4.4 "fg.convert" (Slang primary; GLSL twin fg_convert.comp): the FUSE temporal inputs -> the textures the
// vendored FSR 3.1 frame-interpolation passes read. Per render pixel: linear view depth (f32 buffer, <= 0 = sky) ->
// reverse-Z device depth of a finite [near, far] perspective (fg_device_depth in fg_types.hpp; sky -> 0), UV motion
// (f32x2 buffer, current - previous) copied unchanged (the host sets cbFI.fMotionVectorScale = -1). Compiled with
// -fp-mode precise (no contraction, like the CPU twin).

struct FgConvertPush
{
    uint renderW;
    uint renderH;
    float nearPlane;
    float farPlane;
};

[[vk::binding(0, 0)]] StructuredBuffer<float> fgSourceDepth;
[[vk::binding(1, 0)]] StructuredBuffer<float2> fgSourceMotion;
[[vk::binding(2, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> fgOutDepth;
[[vk::binding(3, 0)]] [[vk::image_format("rg32f")]] RWTexture2D<float2> fgOutMotion;

[[vk::push_constant]] ConstantBuffer<FgConvertPush> pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 p = id.xy;
    if (p.x >= pc.renderW || p.y >= pc.renderH)
    {
        return;
    }
    const uint i = p.y * pc.renderW + p.x;
    const float linearDepth = fgSourceDepth[i];
    float z = 0.0;
    if (linearDepth > 0.0)
    {
        const float num = pc.nearPlane * (pc.farPlane - linearDepth);
        const float den = linearDepth * (pc.farPlane - pc.nearPlane);
        z = clamp(num / den, 0.0, 1.0);
    }
    fgOutDepth[p] = z;
    fgOutMotion[p] = fgSourceMotion[i];
}
