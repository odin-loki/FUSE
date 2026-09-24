// WP-1.5: the include chain every material-resolve GLSL kernel uses (bindless heap, GPU scene,
// visibility formats + decode, material rows, G-buffer packing, the resolve itself).
#ifndef FUSE_MR_INCLUDES_GLSL
#define FUSE_MR_INCLUDES_GLSL
#extension GL_GOOGLE_include_directive : require
#include "bindless.glsl"
#include "gpu_scene.glsl"
#include "vis_format.glsl"
#include "vis_common.glsl"
#include "material.glsl"
#include "gbuffer.glsl"
#include "mr_common.glsl"

layout(push_constant) uniform FuseMrPush {
    uint64_t frame; // BDA of ResolveFrameConstants
    uint64_t out_;  // attribute dump output (ResolveAttributeTexel[]), else 0
} pc;
#endif
