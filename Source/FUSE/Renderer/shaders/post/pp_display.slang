// WP-4.5 "post.display". Slang twin of pp_display.comp (see there).
#include "pp_common.slang"

[[vk::binding(0, 0)]] [format("rgba16f")] RWTexture2D<float4> fuse_pp_images[];

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const PpFrame F = ppFrame();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const uint index = p.y * F.width + p.x;
    float3 c = ppTexels(pc.src)[index].xyz;
    float scale = F.exposureScale;
    if ((F.flags & kPpFlagAutoExposure) != 0u) {
        scale = exp2(F.exposureEv - ppFloats(F.exposureState)[kPpStateCurrentEv]);
    }
    c = float3(c.x * scale, c.y * scale, c.z * scale);
    if ((F.flags & kPpFlagCurve) != 0u) {
        c = float3(ppCurveChannel(c.x, F), ppCurveChannel(c.y, F), ppCurveChannel(c.z, F));
    }
    c = ppTonemap(c, F.toneMapper);
    if ((F.flags & kPpFlagGradeClamp) != 0u) {
        c = float3(ppSaturate(c.x), ppSaturate(c.y), ppSaturate(c.z));
    }
    if ((F.flags & kPpFlagGradeLiftContrast) != 0u) {
        const float3 lifted = float3(c.x + F.lift[0], c.y + F.lift[1], c.z + F.lift[2]) - float3(0.5, 0.5, 0.5);
        c = lifted * F.contrast + float3(0.5, 0.5, 0.5);
    }
    if ((F.flags & kPpFlagGradeSaturation) != 0u) {
        const float lum = ppLuminance(c);
        const float3 grey = float3(lum, lum, lum);
        c = grey + (c - grey) * F.saturation;
    }
    if ((F.flags & kPpFlagGradeGammaGain) != 0u) {
        c = float3(pow(max(c.x, 0.0), F.invGamma[0]) * F.gain[0], pow(max(c.y, 0.0), F.invGamma[1]) * F.gain[1],
                   pow(max(c.z, 0.0), F.invGamma[2]) * F.gain[2]);
    }
    if ((F.flags & kPpFlagLut) != 0u) {
        const float3 v = max(c, float3(0.0, 0.0, 0.0));
        const float3 base = min(v, float3(1.0, 1.0, 1.0));
        const float3 e = float3(ppSrgbEncode(base.x), ppSrgbEncode(base.y), ppSrgbEncode(base.z));
        const float3 s = ppLutTetrahedral(F.lut, F.lutSize, e);
        const float3 graded = float3(ppSrgbDecode(ppSaturate(s.x)), ppSrgbDecode(ppSaturate(s.y)), ppSrgbDecode(ppSaturate(s.z)));
        c = graded + (v - base);
    }
    if ((F.flags & kPpFlagVignetteStack) != 0u) {
        const float u = (float(p.x) + 0.5) / float(F.width) * 2.0 - 1.0;
        const float v = (float(p.y) + 0.5) / float(F.height) * 2.0 - 1.0;
        const float r2 = 0.5 * (u * u + v * v);
        c = c * max(1.0 - F.vignette * r2, 0.0);
    } else if ((F.flags & kPpFlagVignetteLook) != 0u) {
        const float u = (float(p.x) + 0.5) / float(F.width) * 2.0 - 1.0;
        const float v = (float(p.y) + 0.5) / float(F.height) * 2.0 - 1.0;
        const float aspect = float(F.width) / float(F.height);
        const float xs = 1.0 + (aspect - 1.0) * F.vignetteRoundness;
        const float x = u * xs;
        const float r = sqrt((x * x + v * v) / (xs * xs + 1.0));
        const float g = max(1.0 - F.vignette * pow(r, F.vignetteFalloff), 0.0);
        c = c * g + float3(F.vignetteTint[0], F.vignetteTint[1], F.vignetteTint[2]) * (1.0 - g);
    }
    if ((F.flags & kPpFlagGrainStack) != 0u) {
        const float g = ppGrainNoise(F, p.x, p.y) * F.grain;
        c = c + float3(g, g, g);
    } else if ((F.flags & kPpFlagGrainLook) != 0u) {
        const float amp = F.grain * (1.0 - F.grainResponse * ppSaturate(ppLuminance(c)));
        const float g = ppGrainNoise(F, p.x, p.y) * amp;
        c = c + float3(g, g, g);
    }
    float3 outColor = float3(ppSaturate(c.x), ppSaturate(c.y), ppSaturate(c.z));
    if ((F.flags & kPpFlagSrgb) != 0u) {
        outColor = float3(ppSrgbEncode(outColor.x), ppSrgbEncode(outColor.y), ppSrgbEncode(outColor.z));
    }
    const float4 result = float4(outColor, 1.0);
    fuse_pp_images[F.output_ & 0xFFFFFu][p] = result;
    if (F.dump != 0ULL) {
        ppTexels(F.dump)[index] = result;
    }
}
