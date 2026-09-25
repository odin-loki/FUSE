// WP-4.2 "fsr3.convert" (Slang primary; GLSL twin fsr3_convert.comp): the FUSE temporal inputs -> the textures the
// vendored FSR 3.1 passes read. Per render pixel: linear view depth (f32 buffer, <= 0 = sky) -> reverse-Z device
// depth of a finite [near, far] perspective (fsr3_device_depth in fsr3_types.hpp; sky -> 0), UV motion (f32x2
// buffer, current - previous) copied unchanged (the host sets fMotionVectorScale = -1); thread (0, 0) writes the
// exposure texture (exposure, 0). Compiled with -fp-mode precise (no contraction, like the CPU twin).

struct Fsr3ConvertPush
{
    uint renderW;
    uint renderH;
    float nearPlane;
    float farPlane;
    float exposure;
    uint pad0;
    uint pad1;
    uint pad2;
};

[[vk::binding(0, 0)]] StructuredBuffer<float> fsr3SourceDepth;
[[vk::binding(1, 0)]] StructuredBuffer<float2> fsr3SourceMotion;
[[vk::binding(2, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> fsr3OutDepth;
[[vk::binding(3, 0)]] [[vk::image_format("rg32f")]] RWTexture2D<float2> fsr3OutMotion;
[[vk::binding(4, 0)]] [[vk::image_format("rg32f")]] RWTexture2D<float2> fsr3OutExposure;

[[vk::push_constant]] ConstantBuffer<Fsr3ConvertPush> pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 p = id.xy;
    if (p.x == 0 && p.y == 0)
    {
        fsr3OutExposure[uint2(0, 0)] = float2(pc.exposure, 0.0);
    }
    if (p.x >= pc.renderW || p.y >= pc.renderH)
    {
        return;
    }
    const uint i = p.y * pc.renderW + p.x;
    const float linearDepth = fsr3SourceDepth[i];
    float z = 0.0;
    if (linearDepth > 0.0)
    {
        const float num = pc.nearPlane * (pc.farPlane - linearDepth);
        const float den = linearDepth * (pc.farPlane - pc.nearPlane);
        z = clamp(num / den, 0.0, 1.0);
    }
    fsr3OutDepth[p] = z;
    fsr3OutMotion[p] = fsr3SourceMotion[i];
}
