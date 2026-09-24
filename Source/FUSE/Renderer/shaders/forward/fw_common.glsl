// FUSE forward transparency (WP-2.3): records of include/fuse/renderer/forward/forward_types.hpp and the
// fragment's lighting. The lighting is the WP-2.1 deferred shade's (shaders/lighting/lc_common.glsl:
// position and cluster from the device depth, fuse_lc_light per light, same order: directional list,
// then the cluster's list, ascending slot); only where the surface comes from differs (the
// rasterised fragment instead of the G-buffer texels). Slang twin: fw_common.slang.
// #include after bindless.glsl, gpu_scene.glsl, vis_format.glsl, vis_common.glsl, material.glsl,
// gbuffer.glsl, mr_common.glsl, lc_common.glsl and lc_ltc.glsl.
#ifndef FUSE_FW_COMMON_GLSL
#define FUSE_FW_COMMON_GLSL

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
    uint reserved0;
    uint reserved1;
    uint reserved2;
    uint reserved3;
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
vec4 fuse_fw_unorm8(vec4 v) { return floor(clamp(v, 0.0, 1.0) * 255.0 + 0.5) / 255.0; }

// Radiance of a decoded surface: emissive + ambient x albedo x AO + the directional list + the
// cluster's lights: the very loop of lc_shade.comp (fuse_lc_shade_lights, shared in lc_ltc.glsl;
// ascending slot; with the WP-2.2 BRDF LUT the compensated fuse_lc_light_ms, else fuse_lc_light).
vec3 fuse_fw_shade(FuseLcFrame F, FuseLcSurface s, vec3 v, uint cluster) {
    return fuse_lc_shade_lights(F, s, v, cluster); // lc_ltc.glsl: the loop light.shade runs
}

#endif // FUSE_FW_COMMON_GLSL
