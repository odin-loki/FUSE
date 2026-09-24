// WP-6.1 DDGI: shared GLSL declarations of the DDGI kernels (twin: ddgi_common.slang). Layouts mirror
// include/fuse/renderer/gi/gpu/ddgi_gpu_types.hpp; every kernel is pure BDA + push constants (DdgiPush).
#ifndef FUSE_DDGI_COMMON_GLSL
#define FUSE_DDGI_COMMON_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_GOOGLE_include_directive : require

#include "ddgi_sample.glsl"

#define FUSE_DDGI_WORKGROUP 64
#define FUSE_DDGI_INV_PI (1.0 / 3.14159265358979323846)
#define FUSE_DDGI_FLAG_MULTI_BOUNCE 1u
#define FUSE_DDGI_FLAG_FRONT_CCW 2u
#define FUSE_DDGI_FLAG_SUN 4u
#define FUSE_DDGI_MAX_IRRADIANCE_RES 16
#define FUSE_DDGI_MAX_DEPTH_RES 32

struct FuseDdgiFrame { // DdgiFrameConstants, 352 bytes
    FuseDdgiVolume volume;
    vec4 rotation[3];
    float sunDirection[3];
    float maxRayDistance;
    float sunIrradiance[3];
    float backfaceDistanceScale;
    float skyRadiance[3];
    float rayEpsilon;
    uint raysPerProbe;
    uint scheduled;
    uint frameIndex;
    uint flags;
    float hysteresis;
    float probeChangeHysteresis;
    float probeChangeThreshold;
    float changeThreshold;
    float changeHysteresisDrop;
    float changeFloor;
    float distancePower;
    float distanceMinCos;
    float sdfMinDistance;
    uint sdfMaxSteps;
    uint sdfCount;
    float sdfShadowBias;
    uint traceMask;
    uint shadowMask;
    float initialIrradiance[3];
    uint sdfSurfaceCount;
    uint64_t schedule;
    uint64_t rayDirs;
    uint64_t rays;
    uint64_t updateCounts;
    uint64_t irradianceTexelDirs;
    uint64_t distanceTexelDirs;
    uint64_t tlas;
    uint64_t scene;
    uint64_t sdfObjects;
    uint64_t sdfSurfaces;
    uint64_t slotStats;
};

struct FuseDdgiSdfObject { // DdgiSdfObject == compute::SdfObject, 40 bytes
    float position[3];
    float params[3];
    uint type;
    float alpha;
    uint materialId;
    float rounding;
};

struct FuseDdgiSurface { // DdgiSurface, 32 bytes
    vec4 albedo;
    vec4 emissive;
};

struct FuseDdgiPoint { // DdgiProbePoint, 32 bytes
    vec4 position;
    vec4 normal;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseDdgiFrameRef { FuseDdgiFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer FuseDdgiWordsRef { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer FuseDdgiVec4Ref { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer FuseDdgiSdfObjectsRef { FuseDdgiSdfObject v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseDdgiSurfacesRef { FuseDdgiSurface v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseDdgiPointsRef { FuseDdgiPoint v[]; };

layout(push_constant) uniform FuseDdgiPush { // DdgiPush
    uint64_t frame;
    uint64_t aux;
    uint64_t out_;
    uint count;
    uint pad;
} pc;

FuseDdgiFrame fuse_ddgi_frame() { return FuseDdgiFrameRef(pc.frame).f; }

vec3 fuse_ddgi_vec3(float v[3]) { return vec3(v[0], v[1], v[2]); }

float fuse_ddgi_max_component(vec3 v) { return max(v.x, max(v.y, v.z)); }

#endif // FUSE_DDGI_COMMON_GLSL
