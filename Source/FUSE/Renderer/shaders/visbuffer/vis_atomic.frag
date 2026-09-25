#version 460
// WP-1.4 visibility-buffer raster, 64-bit atomic target (no attachments): atomicMin of
// fuse_vis64_pack(depth, instance, triangle) into an R64_UINT storage image (FUSE_VIS64_IMAGE,
// VK_EXT_shader_image_atomic_int64) or a u64 storage buffer (shaderBufferInt64Atomics), row-major
// width x height. Order-independent: the nearest (then smallest id) sample wins. The WP-5.x
// software raster writes the same words. GLSL twin of vis_atomic_fs.slang.
#extension GL_GOOGLE_include_directive : require
#include "bindless.glsl"
#include "vis_format.glsl"

#if defined(FUSE_VIS64_IMAGE)
#extension GL_EXT_shader_image_int64 : require
FUSE_BINDLESS_STORAGE_IMAGE_LAYOUT(r64ui) uniform u64image2D fuse_vis64_images[];
#else
#extension GL_EXT_shader_atomic_int64 : require
FUSE_BINDLESS_SSBO_LAYOUT buffer FuseVis64Words { uint64_t v[]; } fuse_vis64_buffers[];
#endif

layout(push_constant) uniform Push {
    vec4 viewProj[4];
    uint scene;
    uint target64;
    uint width;
    uint height;
    uint pad[4];
} pc;

layout(location = 0) flat in uint inInstance;

void main() {
    const uint64_t value = fuse_vis64_pack(gl_FragCoord.z, inInstance, uint(gl_PrimitiveID));
    const ivec2 p = ivec2(gl_FragCoord.xy);
#if defined(FUSE_VIS64_IMAGE)
    imageAtomicMin(fuse_vis64_images[fuse_handle_index(pc.target64)], p, value);
#else
    atomicMin(fuse_vis64_buffers[fuse_handle_index(pc.target64)].v[uint(p.y) * pc.width + uint(p.x)], value);
#endif
}
