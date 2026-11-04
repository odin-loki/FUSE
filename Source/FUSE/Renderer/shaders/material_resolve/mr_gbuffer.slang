// FUSE material resolve (WP-1.5): Slang twins of shaders/common/material.glsl (FuseGpuMaterial,
// Material::GPUMaterial, 128 bytes) and shaders/common/gbuffer.glsl (write_gbuffer and its RGBA16F
// normal encoding; CPU mirror GBufferPacking). No Slang twin of those files exists yet; keep all
// three in sync.
#pragma once

#include "../common/exact_math.slang" // fuseDivRn / fuseSqrtRn (no Int64 capability)

static const uint kFuseInvalidTexture = 0xFFFFFFFFu;

// GLSL packHalf2x16 / unpackHalf2x16 (GLSL.std.450), so the rounding matches the GLSL twin and no
// Float16 / Int16 capability is declared (f32tof16 / f16tof32 would need shaderFloat16 + shaderInt16).
uint fusePackHalf2x16(float2 v) { return packHalf2x16(v); }
float2 fuseUnpackHalf2x16(uint h) {
    return spirv_asm { result:$$float2 = OpExtInst glsl450 UnpackHalf2x16 $h };
}

struct GpuMaterial {
    float4 base_color;         // rgb = base colour, w = metallic
    float4 roughness_emissive; // x = roughness, yzw = emissive colour
    uint base_color_tex;
    uint roughness_tex;
    uint metallic_tex;
    uint normal_tex;
    uint ao_tex;
    uint emissive_tex;
    float emissive_intensity;
    float normal_strength;
    uint shading_model;
    uint flags;
    uint procedural_seed;
    uint padding;
    float4 subsurface;
    float4 clear_coat;
    float4 cloth;
};

float3 fuseMaterialEmissiveRadiance(GpuMaterial m) {
    return max(m.roughness_emissive.yzw, float3(0.0)) * max(m.emissive_intensity, 0.0);
}

float2 fuseSignNotZero(float2 v) { return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }

float2 fuseOctEncodeSigned(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * fuseSignNotZero(n.xy);
}

float3 fuseOctDecodeSigned(float2 o) {
    float3 n = float3(o, 1.0 - abs(o.x) - abs(o.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * fuseSignNotZero(n.xy);
    }
    return normalize(n);
}

uint fuseHalfNextUp(uint h) {
    if ((h & 0x7FFFu) == 0u) return 1u;
    return (h & 0x8000u) != 0u ? h - 1u : h + 1u;
}

uint fuseHalfNextDown(uint h) {
    if ((h & 0x7FFFu) == 0u) return 0x8001u;
    return (h & 0x8000u) != 0u ? h + 1u : h - 1u;
}

// encode_normal_rgba16f (gbuffer.glsl, see there): signed oct, snapped to the half-representable neighbour with
// the smallest angular error, every step the CPU mirror's (GBufferEncoding::encodeNormalRgba16f) IEEE operation in
// its order (-fp-mode precise; fuseDivRn / fuseSqrtRn; integer half conversions): bit-identical on any device.
float fuseGBufferDot3(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

float3 fuseGBufferNormalized(float3 v) { // fuse::math::Vec3::normalized
    const float len = fuseSqrtRn(fuseGBufferDot3(v, v));
    if (len < 1e-8) {
        return float3(0.0);
    }
    const float inv = fuseDivRn(1.0, len);
    return float3(v.x * inv, v.y * inv, v.z * inv);
}

float fuseGBufferSignNotZero(float v) { return v >= 0.0 ? 1.0 : -1.0; }

float2 fuseGBufferOctEncode(float3 n) { // GBufferEncoding::encodeOctSigned
    const float sum = abs(n.x) + abs(n.y) + abs(n.z);
    if (sum < 1e-20) {
        return float2(0.0);
    }
    const float inv = fuseDivRn(1.0, sum);
    const float3 m = float3(n.x * inv, n.y * inv, n.z * inv);
    if (m.z >= 0.0) {
        return m.xy;
    }
    return float2((1.0 - abs(m.y)) * fuseGBufferSignNotZero(m.x), (1.0 - abs(m.x)) * fuseGBufferSignNotZero(m.y));
}

float3 fuseGBufferOctDecode(float2 o) { // GBufferEncoding::decodeOctSigned
    float3 n = float3(o.x, o.y, 1.0 - abs(o.x) - abs(o.y));
    if (n.z < 0.0) {
        n.xy = float2((1.0 - abs(n.y)) * fuseGBufferSignNotZero(n.x), (1.0 - abs(n.x)) * fuseGBufferSignNotZero(n.y));
    }
    return fuseGBufferNormalized(n);
}

// f32 -> nearest-even f16 bits (GBufferQuantize::floatToHalf; gbuffer.glsl fuse_gbuffer_half_bits).
uint fuseGBufferHalfBits(float v) {
    uint u = asuint(v);
    const uint sign = u & 0x80000000u;
    u ^= sign;
    uint h;
    if (u >= 0x47800000u) { // >= 65536 (or inf / NaN)
        h = u > 0x7F800000u ? 0x7E00u : 0x7C00u;
    } else if (u < 0x38800000u) { // below 2^-14: the f32 add rounds to the 2^-24 grid (nearest even)
        const float t = asfloat(u) + 0.5;
        h = asuint(t) - 0x3F000000u;
    } else { // rebias the exponent, round the 13 dropped mantissa bits to nearest even
        const uint odd = (u >> 13u) & 1u;
        u += 0xC8000FFFu;
        u += odd;
        h = u >> 13u;
    }
    return h | (sign >> 16u);
}

// f16 bits -> f32, exactly, half subnormals included (gbuffer.glsl fuse_gbuffer_half_value).
float fuseGBufferHalfValue(uint h) {
    const uint sign = (h & 0x8000u) << 16u;
    const uint e = (h >> 10u) & 0x1Fu;
    const uint m = h & 0x3FFu;
    if (e == 0u) {
        const float s = float(m) * 5.9604644775390625e-8; // m * 2^-24, exact
        return asfloat(asuint(s) | sign);
    }
    if (e == 0x1Fu) {
        return asfloat(sign | 0x7F800000u | (m << 13u));
    }
    return asfloat(sign | ((e + 112u) << 23u) | (m << 13u));
}

float2 fuseEncodeNormalRgba16f(float3 normal) {
    const float3 n = fuseGBufferNormalized(normal);
    const float2 o = fuseGBufferOctEncode(n);
    const uint hx = fuseGBufferHalfBits(o.x);
    const uint hy = fuseGBufferHalfBits(o.y);
    const uint cx[3] = { hx, fuseHalfNextDown(hx), fuseHalfNextUp(hx) };
    const uint cy[3] = { hy, fuseHalfNextDown(hy), fuseHalfNextUp(hy) };
    float2 best = float2(fuseGBufferHalfValue(hx), fuseGBufferHalfValue(hy));
    float bestDot = -2.0;
    for (int i = 0; i < 3; ++i) {
        const float x = fuseGBufferHalfValue(cx[i]);
        if (abs(x) > 1.0) {
            continue;
        }
        for (int j = 0; j < 3; ++j) {
            const float y = fuseGBufferHalfValue(cy[j]);
            if (abs(y) > 1.0) {
                continue;
            }
            const float d = fuseGBufferDot3(fuseGBufferOctDecode(float2(x, y)), n);
            if (d > bestDot) {
                bestDot = d;
                best = float2(x, y);
            }
        }
    }
    return best;
}

static const float kFuseGBufferMaxHalf = 65504.0;

// Device-independent attachment quantisation (gbuffer.glsl fuse_gbuffer_unorm8 / fuse_gbuffer_half, see there):
// UNORM8 channels are rounded to nearest even here and stored as k / 255, f16 channels as the nearest-even half,
// so the attachment conversion's implementation-defined midpoint rounding never decides a code.
float4 fuseGBufferUnorm8(float4 v) {
    const float4 scaled = clamp(v, float4(0.0), float4(1.0)) * 255.0;
    const float4 q = spirv_asm { result:$$float4 = OpExtInst glsl450 RoundEven $scaled };
    return q / 255.0;
}

float fuseGBufferHalf(float v) { return fuseGBufferHalfValue(fuseGBufferHalfBits(v)); }

float4 fuseGBufferHalf4(float4 v) {
    return float4(fuseGBufferHalf(v.x), fuseGBufferHalf(v.y), fuseGBufferHalf(v.z), fuseGBufferHalf(v.w));
}

struct GBufferOut {
    float4 rt0;
    float4 rt1;
    float4 rt2;
    float4 rt3;
    float4 rt5;
};

// write_gbuffer.
GBufferOut fuseWriteGBuffer(float3 normal, float3 albedo, float roughness, float metallic, float3 emissive, float ao,
                            float2 velocity, uint shadingModel) {
    GBufferOut o;
    o.rt0 = float4(fuseEncodeNormalRgba16f(normal), 0.0, fuseGBufferHalf(ao));
    o.rt1 = fuseGBufferUnorm8(float4(albedo, 1.0));
    o.rt2 = fuseGBufferUnorm8(
        float4(roughness, metallic, length(emissive) > 0.001 ? 1.0 : 0.0, float(shadingModel) / 255.0));
    o.rt3 = fuseGBufferHalf4(float4(velocity, 0.0, 0.0));
    o.rt5 = fuseGBufferHalf4(float4(min(emissive, float3(kFuseGBufferMaxHalf)), 0.0));
    return o;
}
