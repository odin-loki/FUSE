// WP-2.1: the include chain every clustered-lighting GLSL kernel uses (bindless heap, GPU scene, the
// lighting records and math) + the push constants (LightingPush).
#ifndef FUSE_LC_INCLUDES_GLSL
#define FUSE_LC_INCLUDES_GLSL
#extension GL_GOOGLE_include_directive : require
#include "bindless.glsl"
#include "gpu_scene.glsl"
#include "lc_common.glsl"

layout(push_constant) uniform FuseLcPush {
    uint64_t frame; // BDA of LightingFrameConstants
    uint64_t out_;  // light.shade: f32x4 radiance dump, else 0
} pc;

FuseLcFrame fuse_lc_frame() { return FuseLcFrameRef(pc.frame).f; }
#endif
