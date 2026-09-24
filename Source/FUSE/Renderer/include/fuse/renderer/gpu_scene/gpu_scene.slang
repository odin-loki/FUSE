// FUSE GPU scene (WP-1.1): Slang side of include/fuse/renderer/gpu_scene/gpu_scene_types.hpp.
// Twin of gpu_scene.glsl; keep all three in sync (fuse_rp_gpu_scene checks them with a bit-exact
// typed readback on both shader languages).
//
// #include it. It declares the bindless buffer-address table (set 0, binding 5, WP-0.4) unless the
// includer defines FUSE_BINDLESS_ADDRESS_TABLE_DECLARED (no Slang twin of bindless.glsl exists yet).
//
//   GpuSceneHeader* scene = fuseGpuScene(pc.scene);
//   GpuInstance inst = fuseGpuSceneInstances(scene)[i];
//
// Pointer loads use the natural (std430-like) layout; records only use 4-byte scalars, scalar
// arrays, float4 and uint64_t, so it matches the C++ structs byte for byte.
#pragma once

#ifndef FUSE_BINDLESS_ADDRESS_TABLE_DECLARED
#define FUSE_BINDLESS_ADDRESS_TABLE_DECLARED 1
[[vk::binding(5, 0)]] StructuredBuffer<uint64_t> fuse_buffer_addresses;
uint64_t fuse_buffer_address(uint handle) { return fuse_buffer_addresses[handle & 0xFFFFFu]; }
#endif

static const uint kFuseGpuSceneInstances = 0;
static const uint kFuseGpuSceneTransforms = 1;
static const uint kFuseGpuScenePrevTransforms = 2;
static const uint kFuseGpuSceneMeshes = 3;
static const uint kFuseGpuSceneMaterials = 4;
static const uint kFuseGpuSceneLights = 5;
static const uint kFuseGpuSceneTableCount = 6;
static const uint kFuseGpuSceneMagic = 0x43534746u;
static const uint kFuseGpuSceneInvalid = 0xFFFFFFFFu;

static const uint kFuseInstanceValid = 1u;
static const uint kFuseInstanceVisible = 2u;
static const uint kFuseInstanceCastShadow = 4u;
static const uint kFuseInstanceReceiveShadow = 8u;
static const uint kFuseInstanceStatic = 16u;

struct GpuInstance {
    uint mesh;
    uint material;
    uint flags;
    uint generation;
    uint entityIndex;
    uint entityGeneration;
    uint userData;
    uint reserved;
};

struct GpuTransform {
    float4 rows[3]; // row-major 3x4 object -> world
};

struct GpuMeshlet { // == .fusemeshlet MSHL element
    uint vertexOffset;
    uint triangleOffset;
    uint counts;
    float center[3];
    float radius;
    float coneApex[3];
    float coneAxis[3];
    float coneCutoff;
    uint coneS8;
    float aabbMin[3];
    float aabbMax[3];
    uint reserved[3];
};

struct GpuSubmesh {
    uint meshletOffset;
    uint meshletCount;
    uint materialIndex;
    uint triangleCount;
};

struct GpuMesh {
    uint64_t meshlets;
    uint64_t meshletVertices;
    uint64_t meshletTriangles;
    uint64_t positions;
    uint64_t normals;
    uint64_t tangents;
    uint64_t uvs;
    uint64_t submeshes;
    uint meshletCount;
    uint submeshCount;
    uint vertexCount;
    uint triangleCount;
    float quantOffset[3];
    float quantStep[3];
    float boundsCenter[3];
    float boundsRadius;
    uint geometryHandle;
    uint flags;
};

struct GpuLight {
    float position[3];
    uint type;
    float direction[3];
    float range;
    float color[3];
    float intensity;
    float cosInner;
    float cosOuter;
    uint flags;
    uint entityIndex;
};

struct GpuSceneHeader {
    uint64_t addresses[6];
    uint counts[6];
    uint handles[6];
    uint capacities[6];
    uint liveInstances;
    uint liveLights;
    uint magic;
    uint version;
    uint reserved[2];
};

GpuSceneHeader* fuseGpuScene(uint sceneHandle) { return (GpuSceneHeader*)fuse_buffer_address(sceneHandle); }
GpuInstance* fuseGpuSceneInstances(GpuSceneHeader* scene) { return (GpuInstance*)scene.addresses[kFuseGpuSceneInstances]; }
GpuTransform* fuseGpuSceneTransforms(GpuSceneHeader* scene) { return (GpuTransform*)scene.addresses[kFuseGpuSceneTransforms]; }
GpuTransform* fuseGpuScenePrevTransforms(GpuSceneHeader* scene) { return (GpuTransform*)scene.addresses[kFuseGpuScenePrevTransforms]; }
GpuMesh* fuseGpuSceneMeshes(GpuSceneHeader* scene) { return (GpuMesh*)scene.addresses[kFuseGpuSceneMeshes]; }
GpuLight* fuseGpuSceneLights(GpuSceneHeader* scene) { return (GpuLight*)scene.addresses[kFuseGpuSceneLights]; }

float3 fuseGpuTransformPoint(GpuTransform t, float3 p) {
    const float4 h = float4(p, 1.0);
    return float3(dot(t.rows[0], h), dot(t.rows[1], h), dot(t.rows[2], h));
}
