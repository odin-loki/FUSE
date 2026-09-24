// WP-6.0 acceleration structures: shared Slang declarations (twin of rt_common.glsl; layouts mirror
// include/fuse/renderer/rt/rt_types.hpp and gpu_scene_types.hpp). Pure BDA + push constants.
#pragma once

static const uint kFuseRtWorkgroup = 64;
static const uint kFuseRtInstanceValid = 1u;
static const uint kFuseRtInstanceVisible = 2u;
static const uint kFuseRtInstanceCastShadow = 4u;
static const uint kFuseRtInstanceTransparent = 32u;
static const uint kFuseRtMaskVisible = 1u;
static const uint kFuseRtMaskShadow = 2u;
static const uint kFuseRtMaskTransparent = 4u;
static const uint kFuseRtMaskDead = 128u;
static const uint kFuseRtInstanceFlags = 5u; // TRIANGLE_FACING_CULL_DISABLE | FORCE_OPAQUE
static const uint kFuseRtHit = 1u;
static const uint kFuseRtFrontFace = 2u;
static const uint kFuseRtInvalid = 0xFFFFFFFFu;

struct RtSceneInstance { // gpu_scene::GpuInstance (32 bytes)
    uint mesh;
    uint material;
    uint flags;
    uint generation;
    uint entityIndex;
    uint entityGeneration;
    uint userData;
    uint reserved;
};

struct RtTransform { // gpu_scene::GpuTransform (48 bytes, row-major 3x4)
    float4 rows[3];
};

struct RtAsInstance { // VkAccelerationStructureInstanceKHR (64 bytes)
    float4 rows[3];
    uint customIndexMask;
    uint sbtOffsetFlags;
    uint64_t blas;
};

struct RtRay { // RtProbeRay (32 bytes)
    float4 originTMin;
    float4 directionTMax;
};

struct RtHit { // RtProbeHit (32 bytes)
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
uint fuseRtInstanceMask(uint flags, uint64_t blas) {
    if ((flags & kFuseRtInstanceValid) == 0u || blas == 0) {
        return 0u;
    }
    uint mask = 0u;
    if ((flags & kFuseRtInstanceVisible) != 0u) {
        mask |= kFuseRtMaskVisible;
    }
    if ((flags & kFuseRtInstanceCastShadow) != 0u) {
        mask |= kFuseRtMaskShadow;
    }
    if (mask != 0u && (flags & kFuseRtInstanceTransparent) != 0u) {
        mask |= kFuseRtMaskTransparent;
    }
    return mask;
}
