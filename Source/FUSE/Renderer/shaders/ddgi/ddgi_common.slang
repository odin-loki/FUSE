// WP-6.1 DDGI: shared Slang declarations of the DDGI kernels (twin of ddgi_common.glsl; layouts mirror
// include/fuse/renderer/gi/gpu/ddgi_gpu_types.hpp). Pure BDA + push constants. -fp-mode precise.
#pragma once
#include "ddgi_sample.slang"

static const uint kFuseDdgiWorkgroup = 64;
static const float kFuseDdgiInvPi = 1.0 / 3.14159265358979323846;
static const uint kFuseDdgiFlagMultiBounce = 1u;
static const uint kFuseDdgiFlagFrontCcw = 2u;
static const uint kFuseDdgiFlagSun = 4u;
static const uint kFuseDdgiFlagRelocation = 8u;
static const uint kFuseDdgiFlagClassification = 16u;
static const uint kFuseDdgiMaxIrradianceRes = 16;
static const uint kFuseDdgiMaxDepthRes = 32;

struct DdgiFrame { // DdgiFrameConstants, 400 bytes
    DdgiVolume volume;
    float4 rotation[3];
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
    float distanceClamp;
    float probeMinFrontfaceDistance;
    float probeBackfaceThreshold;
    float probeMaxOffset;
    float probeRelocationStep;
    uint statePad[3];
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

struct DdgiSdfObject { // == compute::SdfObject, 40 bytes
    float position[3];
    float params[3];
    uint type;
    float alpha;
    uint materialId;
    float rounding;
};

struct DdgiSurfaceRow { // DdgiSurface, 32 bytes
    float4 albedo;
    float4 emissive;
};

struct DdgiPoint { // DdgiProbePoint, 48 bytes
    float4 position;
    float4 normal;
    float4 view;
};

struct DdgiPush { // DdgiPush, 32 bytes
    uint64_t frame;
    uint64_t aux;
    uint64_t out_;
    uint count;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<DdgiPush> pc;

DdgiFrame fuseDdgiFrame() { return ((DdgiFrame*)pc.frame)[0]; }

float3 fuseDdgiVec3(float v[3]) { return float3(v[0], v[1], v[2]); }

float fuseDdgiMaxComponent(float3 v) { return max(v.x, max(v.y, v.z)); }
