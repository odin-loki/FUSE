// FUSE material resolve (WP-1.5): Slang twins of shaders/common/material.glsl (FuseGpuMaterial,
// Material::GPUMaterial, 128 bytes) and shaders/common/gbuffer.glsl (write_gbuffer and its RGBA16F
// normal encoding; CPU mirror GBufferPacking). No Slang twin of those files exists yet; keep all
// three in sync.
#pragma once

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

// encode_normal_rgba16f: signed oct, snapped to the half-representable neighbour with the smallest
// angular error.
float2 fuseEncodeNormalRgba16f(float3 n) {
    n = normalize(n);
    const float2 o = fuseOctEncodeSigned(n);
    const uint hx = fusePackHalf2x16(float2(o.x, 0.0)) & 0xFFFFu;
    const uint hy = fusePackHalf2x16(float2(o.y, 0.0)) & 0xFFFFu;
    const uint cx[3] = { hx, fuseHalfNextDown(hx), fuseHalfNextUp(hx) };
    const uint cy[3] = { hy, fuseHalfNextDown(hy), fuseHalfNextUp(hy) };
    float2 best = fuseUnpackHalf2x16(hx | (hy << 16));
    float bestDot = -2.0;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const float2 c = fuseUnpackHalf2x16(cx[i] | (cy[j] << 16));
            if (abs(c.x) > 1.0 || abs(c.y) > 1.0) continue;
            const float d = dot(fuseOctDecodeSigned(c), n);
            if (d > bestDot) {
                bestDot = d;
                best = c;
            }
        }
    }
    return best;
}

static const float kFuseGBufferMaxHalf = 65504.0;

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
    o.rt0 = float4(fuseEncodeNormalRgba16f(normal), 0.0, ao);
    o.rt1 = float4(albedo, 1.0);
    o.rt2 = float4(roughness, metallic, length(emissive) > 0.001 ? 1.0 : 0.0, float(shadingModel) / 255.0);
    o.rt3 = float4(velocity, 0.0, 0.0);
    o.rt5 = float4(min(emissive, float3(kFuseGBufferMaxHalf)), 0.0);
    return o;
}
