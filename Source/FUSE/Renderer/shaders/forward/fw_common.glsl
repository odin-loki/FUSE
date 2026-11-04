// FUSE forward transparency (WP-2.3): records of include/fuse/renderer/forward/forward_types.hpp and the
// fragment's lighting. The lighting is the WP-2.1 deferred shade's (shaders/lighting/lc_common.glsl:
// position and cluster from the device depth, fuse_lc_light per light, same order: directional list,
// then the cluster's list, ascending slot); only where the surface comes from differs (the
// rasterised fragment instead of the G-buffer texels). Slang twin: fw_common.slang.
// #include after bindless.glsl, gpu_scene.glsl, vis_format.glsl, vis_common.glsl, material.glsl,
// gbuffer.glsl, mr_common.glsl, lc_common.glsl and lc_ltc.glsl.
#ifndef FUSE_FW_COMMON_GLSL
#define FUSE_FW_COMMON_GLSL

#define FUSE_FW_FLAG_AERIAL 1u // ForwardFlag
#define FUSE_FW_FLAG_FOG 2u

// ForwardFrameConstants, 128 bytes.
struct FuseFwFrame {
    vec4 viewProj[4]; // columns
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
struct FuseFwDraw {
    uint slot;
    float opacity;
    uint reserved0;
    uint reserved1;
};

// ForwardDumpTexel, 96 bytes.
struct FuseFwDumpTexel {
    vec4 radiance; // rgb radiance, a = opacity
    vec4 rt0;
    vec4 rt1;
    vec4 rt2;
    vec4 rt5;
    float depth;
    uint covered;
    uint slot;
    uint cluster;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseFwFrameRef { FuseFwFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseFwDrawsRef { FuseFwDraw v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) writeonly buffer FuseFwDumpRef { FuseFwDumpTexel v[]; };

// The deferred G-buffer's attachment quantisation (GBufferLayout: RT0 / RT5 RGBA16F, RT1 / RT2 RGBA8
// UNORM), so the forward surface is decoded from the same values the deferred shade would read.
vec4 fuse_fw_half4(vec4 v) {
    return vec4(unpackHalf2x16(packHalf2x16(v.xy)), unpackHalf2x16(packHalf2x16(v.zw)));
}
// The attachment's UNORM8 -> float conversion is q / 255 correctly rounded; a float divide may be 2.5 ulp
// off on Vulkan (NVIDIA uses the slack, which broke "forward surface == the twin's G-buffer" bit for bit),
// so the quotient is formed in integers: with s = 31 - msb(q), N = q << s lies in [2^31, 2^32) and
// q / 255 = (N / 255) * 2^-s, where M = round(N / 255) = (N + 127) / 255 (255 is odd: no ties) has 24 bits.
float fuse_fw_unorm8_value(uint q) {
    if (q == 0u || q >= 255u) {
        return q == 0u ? 0.0 : 1.0;
    }
    const uint sh = 31u - uint(findMSB(q));
    const uint m = ((q << sh) + 127u) / 255u;
    return float(m) * uintBitsToFloat((127u - sh) << 23u); // exact: m < 2^24, 2^-sh normal
}
vec4 fuse_fw_unorm8(vec4 v) {
    const uvec4 q = uvec4(floor(clamp(v, 0.0, 1.0) * 255.0 + 0.5));
    return vec4(fuse_fw_unorm8_value(q.x), fuse_fw_unorm8_value(q.y), fuse_fw_unorm8_value(q.z), fuse_fw_unorm8_value(q.w));
}

// Radiance of a decoded surface: emissive + ambient x albedo x AO + the directional list + the
// cluster's lights: the very loop of lc_shade.comp (fuse_lc_shade_lights, shared in lc_ltc.glsl;
// ascending slot; with the WP-2.2 BRDF LUT the compensated fuse_lc_light_ms, else fuse_lc_light).
vec3 fuse_fw_shade(FuseLcFrame F, FuseLcSurface s, vec3 v, uint cluster) {
    return fuse_lc_shade_lights(F, s, v, cluster); // lc_ltc.glsl: the loop light.shade runs
}

#endif // FUSE_FW_COMMON_GLSL
