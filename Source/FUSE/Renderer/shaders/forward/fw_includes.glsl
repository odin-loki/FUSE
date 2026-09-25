// WP-2.3: the include chain of the forward-transparency GLSL shaders (bindless heap, GPU scene,
// visibility clip math, material rows + G-buffer packing + the resolve's material evaluation, the
// clustered lighting's records and shading, the forward records) + the push constants (ForwardPush).
#ifndef FUSE_FW_INCLUDES_GLSL
#define FUSE_FW_INCLUDES_GLSL
#extension GL_GOOGLE_include_directive : require
#include "bindless.glsl"
#include "gpu_scene.glsl"
#include "vis_format.glsl"
#include "vis_common.glsl"
#include "material.glsl"
#include "gbuffer.glsl"
#include "mr_common.glsl"
#include "lc_common.glsl"
#include "lc_ltc.glsl" // WP-2.2 compensated BRDF + area lights (light.shade uses them too)
#include "fw_common.glsl"

layout(push_constant) uniform FuseFwPush {
    uint64_t frame; // BDA of ForwardFrameConstants
    uint64_t reserved;
} pc;

FuseFwFrame fuse_fw_frame() { return FuseFwFrameRef(pc.frame).f; }
#endif
