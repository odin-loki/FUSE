// FUSE forward transparency (WP-2.3): Slang twin of fw_common.glsl (see there). #include after
// gpu_scene.slang, vis_format.slang, vis_common.slang, mr_gbuffer.slang, mr_common.slang and
// lc_common.slang and lc_ltc.slang. Compile with -fp-mode precise.
#pragma once

// ForwardFrameConstants, 128 bytes.
static const uint kFuseFwFlagAerial = 1u; // ForwardFlag
static const uint kFuseFwFlagFog = 2u;

struct FwFrame {
    float4 viewProj[4];
    uint64_t lighting;
    uint64_t draws;
    uint64_t dump;
    uint scene;
    uint sampler_;
    uint width;
    uint height;
    uint drawCount;
    uint flags;
    uint64_t atmosphere;
    uint64_t fog;
};

// ForwardDraw, 16 bytes.
struct FwDraw {
    uint slot;
    float opacity;
    uint reserved0;
    uint reserved1;
};

// ForwardDumpTexel, 96 bytes.
struct FwDumpTexel {
    float4 radiance;
    float4 rt0;
    float4 rt1;
    float4 rt2;
    float4 rt5;
    float depth;
    uint covered;
    uint slot;
    uint cluster;
};

float4 fuseFwHalf4(float4 v) {
    return float4(fuseUnpackHalf2x16(fusePackHalf2x16(v.xy)), fuseUnpackHalf2x16(fusePackHalf2x16(v.zw)));
}
// The attachment's UNORM8 -> float conversion is q / 255 correctly rounded; a float divide may be 2.5 ulp
// off on Vulkan (NVIDIA uses the slack, which broke "forward surface == the twin's G-buffer" bit for bit),
// so the quotient is formed in integers: with s = 31 - msb(q), N = q << s lies in [2^31, 2^32) and
// q / 255 = (N / 255) * 2^-s, where M = round(N / 255) = (N + 127) / 255 (255 is odd: no ties) has 24 bits.
float fuseFwUnorm8Value(uint q) {
    if (q == 0u || q >= 255u) {
        return q == 0u ? 0.0 : 1.0;
    }
    const uint sh = 31u - firstbithigh(q);
    const uint m = ((q << sh) + 127u) / 255u;
    return float(m) * asfloat((127u - sh) << 23u); // exact: m < 2^24, 2^-sh normal
}
float4 fuseFwUnorm8(float4 v) {
    const uint4 q = uint4(floor(clamp(v, 0.0, 1.0) * 255.0 + 0.5));
    return float4(fuseFwUnorm8Value(q.x), fuseFwUnorm8Value(q.y), fuseFwUnorm8Value(q.z), fuseFwUnorm8Value(q.w));
}

// The very loop of lc_shade.slang (fuseLcShadeLights, shared in lc_ltc.slang).
float3 fuseFwShade(LcFrame F, LcSurface s, float3 v, uint cluster) { return fuseLcShadeLights(F, s, v, cluster); }
