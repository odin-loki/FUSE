#version 460
// WP-5.4 hardware path, fragment stage: atomicMin of the WP-1.4 word fuse_vis64_pack(gl_FragCoord.z,
// instance, triangle) into the same 64-bit target the software rasteriser writes (FUSE_VIS64_IMAGE:
// R64_UINT storage image, else u64 storage buffer). GLSL twin of swraster_hw_fs.slang; the same
// packing as the WP-1.4 atomic fragment shader (vis_atomic.frag), with the triangle id from the
// vertex stage instead of gl_PrimitiveID (the cluster draw restarts primitive ids per instance).
#extension GL_GOOGLE_include_directive : require
#include "bindless.glsl"
#include "gpu_scene.glsl"
#include "vis_format.glsl"
#include "vis_common.glsl"
#include "swraster_common.glsl"

#if defined(FUSE_VIS64_IMAGE)
#extension GL_EXT_shader_image_int64 : require
FUSE_BINDLESS_STORAGE_IMAGE_LAYOUT(r64ui) uniform u64image2D fuse_vis64_images[];
#else
#extension GL_EXT_shader_atomic_int64 : require
FUSE_BINDLESS_SSBO_LAYOUT buffer FuseVis64Words { uint64_t v[]; } fuse_vis64_buffers[];
#endif

layout(push_constant) uniform Push {
    uint64_t constants;
    uint region;
    uint pad;
} pc;

layout(location = 0) flat in uint inInstance;
layout(location = 1) flat in uint inTriangle;

void main() {
    FuseSwConstantsRef C = FuseSwConstantsRef(pc.constants);
    const uint64_t value = fuse_vis64_pack(gl_FragCoord.z, inInstance, inTriangle);
    const ivec2 p = ivec2(gl_FragCoord.xy);
    const uint t = fuse_handle_index(C.target64);
#if defined(FUSE_VIS64_IMAGE)
    imageAtomicMin(fuse_vis64_images[t], p, value);
#else
    atomicMin(fuse_vis64_buffers[t].v[uint(p.y) * C.width + uint(p.x)], value);
#endif
}
