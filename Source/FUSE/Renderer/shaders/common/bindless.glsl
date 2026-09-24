// FUSE bindless heap: shader side of include/fuse/renderer/vk/bindless.hpp (WP-0.4).
// Keep the constants below in sync with that header; fuse_rp_bindless checks them at build time
// (the C++ test packs handles, this file decodes them on the GPU).
//
// One descriptor set (FUSE_BINDLESS_SET, default 0) with:
//   binding 0  storage images    (declare typed arrays with FUSE_BINDLESS_STORAGE_IMAGE_LAYOUT)
//   binding 1  sampled images    fuse_textures_2d[] / _2d_array[] / _cube[] / _3d[]
//   binding 2  samplers          fuse_samplers[]
//   binding 3  storage buffers   (declare block arrays with FUSE_BINDLESS_SSBO_LAYOUT)
//   binding 4  uniform buffers   (declare block arrays with FUSE_BINDLESS_UBO_LAYOUT)
//   binding 5  buffer-address table: fuse_buffer_addresses[slot] = device address of the buffer
//              registered at that buffer slot (define FUSE_BINDLESS_NO_ADDRESS_TABLE to omit)
//
// 32-bit handle:  bits 0..19 slot index | bits 20..23 resource type | bits 24..31 generation (low 8)
// Type 0 is "invalid", so a zero handle never names a resource. Shaders index with the low 20 bits;
// the generation is for CPU-side / debug stale checks only. Index with nonuniformEXT() whenever the
// handle is not dynamically uniform (per-pixel material, per-instance data).
//
// Works on both backends (descriptor indexing and VK_EXT_descriptor_buffer): the SPIR-V is identical.
#ifndef FUSE_BINDLESS_GLSL
#define FUSE_BINDLESS_GLSL

#extension GL_EXT_nonuniform_qualifier : require

#ifndef FUSE_BINDLESS_SET
#define FUSE_BINDLESS_SET 0
#endif

#define FUSE_BINDLESS_BINDING_STORAGE_IMAGES 0
#define FUSE_BINDLESS_BINDING_SAMPLED_IMAGES 1
#define FUSE_BINDLESS_BINDING_SAMPLERS 2
#define FUSE_BINDLESS_BINDING_STORAGE_BUFFERS 3
#define FUSE_BINDLESS_BINDING_UNIFORM_BUFFERS 4
#define FUSE_BINDLESS_BINDING_BUFFER_ADDRESS_TABLE 5

#define FUSE_HANDLE_INDEX_MASK 0xFFFFFu
#define FUSE_HANDLE_TYPE_SHIFT 20u
#define FUSE_HANDLE_TYPE_MASK 0xFu
#define FUSE_HANDLE_GENERATION_SHIFT 24u
#define FUSE_HANDLE_INVALID 0u

#define FUSE_HANDLE_TYPE_INVALID 0u
#define FUSE_HANDLE_TYPE_SAMPLED_IMAGE 1u
#define FUSE_HANDLE_TYPE_STORAGE_IMAGE 2u
#define FUSE_HANDLE_TYPE_SAMPLER 3u
#define FUSE_HANDLE_TYPE_STORAGE_BUFFER 4u
#define FUSE_HANDLE_TYPE_UNIFORM_BUFFER 5u

uint fuse_handle_index(uint handle) { return handle & FUSE_HANDLE_INDEX_MASK; }
uint fuse_handle_type(uint handle) { return (handle >> FUSE_HANDLE_TYPE_SHIFT) & FUSE_HANDLE_TYPE_MASK; }
uint fuse_handle_generation(uint handle) { return handle >> FUSE_HANDLE_GENERATION_SHIFT; }
bool fuse_handle_valid(uint handle) { return fuse_handle_type(handle) != FUSE_HANDLE_TYPE_INVALID; }
bool fuse_handle_is(uint handle, uint type) { return fuse_handle_type(handle) == type; }

// Layout qualifiers for typed arrays (a binding may be declared several times with different
// image formats / block layouts; each declaration aliases the same descriptor array):
//   FUSE_BINDLESS_STORAGE_IMAGE_LAYOUT(r32ui) uniform uimage2D my_uimages[];
//   FUSE_BINDLESS_SSBO_LAYOUT buffer MyBlock { uint words[]; } my_buffers[];
//   FUSE_BINDLESS_UBO_LAYOUT uniform MyParams { vec4 v; } my_params[];
#define FUSE_BINDLESS_STORAGE_IMAGE_LAYOUT(fmt) \
    layout(set = FUSE_BINDLESS_SET, binding = FUSE_BINDLESS_BINDING_STORAGE_IMAGES, fmt)
#define FUSE_BINDLESS_SSBO_LAYOUT layout(set = FUSE_BINDLESS_SET, binding = FUSE_BINDLESS_BINDING_STORAGE_BUFFERS, std430)
#define FUSE_BINDLESS_UBO_LAYOUT layout(set = FUSE_BINDLESS_SET, binding = FUSE_BINDLESS_BINDING_UNIFORM_BUFFERS, std140)

layout(set = FUSE_BINDLESS_SET, binding = FUSE_BINDLESS_BINDING_SAMPLED_IMAGES) uniform texture2D fuse_textures_2d[];
layout(set = FUSE_BINDLESS_SET, binding = FUSE_BINDLESS_BINDING_SAMPLED_IMAGES) uniform texture2DArray fuse_textures_2d_array[];
layout(set = FUSE_BINDLESS_SET, binding = FUSE_BINDLESS_BINDING_SAMPLED_IMAGES) uniform textureCube fuse_textures_cube[];
layout(set = FUSE_BINDLESS_SET, binding = FUSE_BINDLESS_BINDING_SAMPLED_IMAGES) uniform texture3D fuse_textures_3d[];
layout(set = FUSE_BINDLESS_SET, binding = FUSE_BINDLESS_BINDING_SAMPLERS) uniform sampler fuse_samplers[];

#define FUSE_TEXTURE_2D(tex, smp) \
    nonuniformEXT(sampler2D(fuse_textures_2d[nonuniformEXT(fuse_handle_index(tex))], fuse_samplers[nonuniformEXT(fuse_handle_index(smp))]))

vec4 fuse_sample_2d(uint tex, uint smp, vec2 uv) { return texture(FUSE_TEXTURE_2D(tex, smp), uv); }
vec4 fuse_sample_2d_lod(uint tex, uint smp, vec2 uv, float lod) { return textureLod(FUSE_TEXTURE_2D(tex, smp), uv, lod); }
vec4 fuse_fetch_2d(uint tex, uint smp, ivec2 texel, int lod) { return texelFetch(FUSE_TEXTURE_2D(tex, smp), texel, lod); }

#ifndef FUSE_BINDLESS_NO_ADDRESS_TABLE
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(set = FUSE_BINDLESS_SET, binding = FUSE_BINDLESS_BINDING_BUFFER_ADDRESS_TABLE, std430)
    readonly buffer FuseBindlessAddressTable { uint64_t fuse_buffer_addresses[]; };

// Device address of the buffer behind a storage/uniform buffer handle (0 when none). Cast to a
// buffer_reference type: MyRef ref = MyRef(fuse_buffer_address(handle));
uint64_t fuse_buffer_address(uint handle) { return fuse_buffer_addresses[fuse_handle_index(handle)]; }
#endif

#endif // FUSE_BINDLESS_GLSL
