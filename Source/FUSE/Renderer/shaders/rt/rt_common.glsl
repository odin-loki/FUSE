// WP-6.0 acceleration structures: shared GLSL declarations (twin: rt_common.slang). Layouts mirror
// include/fuse/renderer/rt/rt_types.hpp and the WP-1.1 GPU-scene records (gpu_scene_types.hpp); every
// kernel is pure BDA + push constants (no descriptor sets).
#ifndef FUSE_RT_COMMON_GLSL
#define FUSE_RT_COMMON_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#define FUSE_RT_WORKGROUP 64
#define FUSE_RT_INSTANCE_VALID 1u
#define FUSE_RT_INSTANCE_VISIBLE 2u
#define FUSE_RT_INSTANCE_CAST_SHADOW 4u
#define FUSE_RT_INSTANCE_TRANSPARENT 32u
#define FUSE_RT_MASK_VISIBLE 1u
#define FUSE_RT_MASK_SHADOW 2u
#define FUSE_RT_MASK_TRANSPARENT 4u
#define FUSE_RT_MASK_DEAD 128u
#define FUSE_RT_INSTANCE_FLAGS 5u // TRIANGLE_FACING_CULL_DISABLE | FORCE_OPAQUE
#define FUSE_RT_HIT 1u
#define FUSE_RT_FRONT_FACE 2u
#define FUSE_RT_INVALID 0xFFFFFFFFu

struct FuseRtSceneInstance { // gpu_scene::GpuInstance (32 bytes)
    uint mesh;
    uint material;
    uint flags;
    uint generation;
    uint entityIndex;
    uint entityGeneration;
    uint userData;
    uint reserved;
};

struct FuseRtAsInstance { // VkAccelerationStructureInstanceKHR (64 bytes)
    vec4 rows[3];
    uint customIndexMask;
    uint sbtOffsetFlags;
    uint64_t blas;
};

struct FuseRtRay { // RtProbeRay (32 bytes)
    vec4 originTMin;
    vec4 directionTMax;
};

struct FuseRtHit { // RtProbeHit (32 bytes)
    float t;
    uint instance;
    uint customIndex;
    uint primitive;
    float u;
    float v;
    uint flags;
    uint geometry;
};

// rt_types.hpp rtInstanceMask().
uint fuse_rt_instance_mask(uint flags, uint64_t blas) {
    if ((flags & FUSE_RT_INSTANCE_VALID) == 0u || blas == 0ul) {
        return 0u;
    }
    uint mask = 0u;
    if ((flags & FUSE_RT_INSTANCE_VISIBLE) != 0u) {
        mask |= FUSE_RT_MASK_VISIBLE;
    }
    if ((flags & FUSE_RT_INSTANCE_CAST_SHADOW) != 0u) {
        mask |= FUSE_RT_MASK_SHADOW;
    }
    if (mask != 0u && (flags & FUSE_RT_INSTANCE_TRANSPARENT) != 0u) {
        mask |= FUSE_RT_MASK_TRANSPARENT;
    }
    return mask;
}

#endif // FUSE_RT_COMMON_GLSL
