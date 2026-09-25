// FUSE GPU scene (WP-1.1): shader side of include/fuse/renderer/gpu_scene/gpu_scene_types.hpp.
// Keep in sync with that header and with gpu_scene.slang (fuse_rp_gpu_scene checks all three with a
// bit-exact typed readback).
//
// Include after shaders/common/bindless.glsl (needs fuse_buffer_address). A pass gets one 32-bit
// storage-buffer handle (GpuScene::headerHandle()) and reaches every table through BDA:
//
//   FuseGpuSceneHeaderRef scene = fuse_gpu_scene(pc.scene);
//   FuseGpuInstance inst = fuse_gpu_scene_instances(scene).v[i];
//   FuseGpuTransform xf = fuse_gpu_scene_transforms(scene).v[i];
//
// Records only use 4-byte scalars, scalar arrays (std430 stride 4), vec4 and uint64_t, so std430 and
// the C++ structs agree byte for byte. Material rows are Material::GPUMaterial: cast
// fuse_gpu_scene_address(scene, FUSE_GPU_SCENE_MATERIALS) to FuseGpuMaterial (shaders/common/material.glsl).
#ifndef FUSE_GPU_SCENE_GLSL
#define FUSE_GPU_SCENE_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#define FUSE_GPU_SCENE_INSTANCES 0
#define FUSE_GPU_SCENE_TRANSFORMS 1
#define FUSE_GPU_SCENE_PREV_TRANSFORMS 2
#define FUSE_GPU_SCENE_MESHES 3
#define FUSE_GPU_SCENE_MATERIALS 4
#define FUSE_GPU_SCENE_LIGHTS 5
#define FUSE_GPU_SCENE_TABLE_COUNT 6
#define FUSE_GPU_SCENE_MAGIC 0x43534746u
#define FUSE_GPU_SCENE_INVALID 0xFFFFFFFFu

#define FUSE_INSTANCE_VALID 1u
#define FUSE_INSTANCE_VISIBLE 2u
#define FUSE_INSTANCE_CAST_SHADOW 4u
#define FUSE_INSTANCE_RECEIVE_SHADOW 8u
#define FUSE_INSTANCE_STATIC 16u
#define FUSE_INSTANCE_TRANSPARENT 32u

// FuseGpuMaterial::flags bit: layered material, FuseGpuMaterial::padding = its layered-table index (kGpuMaterialLayered).
#define FUSE_GPU_MATERIAL_LAYERED 0x80000u

#define FUSE_LIGHT_NONE 0u
#define FUSE_LIGHT_DIRECTIONAL 1u
#define FUSE_LIGHT_POINT 2u
#define FUSE_LIGHT_SPOT 3u

struct FuseGpuInstance {
    uint mesh;
    uint material;
    uint flags;
    uint generation;
    uint entityIndex;
    uint entityGeneration;
    uint userData;
    uint reserved;
};

// Row-major 3x4 object -> world: world = vec3(dot(rows[0], p), dot(rows[1], p), dot(rows[2], p)), p.w = 1.
struct FuseGpuTransform {
    vec4 rows[3];
};

struct FuseGpuMeshlet { // == .fusemeshlet MSHL element
    uint vertexOffset;
    uint triangleOffset;
    uint counts; // vertex_count | triangle_count << 8 | submesh << 16
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

struct FuseGpuSubmesh {
    uint meshletOffset;
    uint meshletCount;
    uint materialIndex;
    uint triangleCount;
};

struct FuseGpuMesh {
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
    uint firstIndex;  // draw range in the scene index buffer (gpu_scene_types.hpp "Index layout")
    uint indexCount;  // 0 = none: implicit {3 x triangleCount, 0, 0}
    int vertexOffset;
    uint reserved;
};

struct FuseGpuLight {
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

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseGpuSceneHeaderRef {
    uint64_t addresses[FUSE_GPU_SCENE_TABLE_COUNT];
    uint counts[FUSE_GPU_SCENE_TABLE_COUNT];
    uint handles[FUSE_GPU_SCENE_TABLE_COUNT];
    uint capacities[FUSE_GPU_SCENE_TABLE_COUNT];
    uint liveInstances;
    uint liveLights;
    uint magic;
    uint version;
    uint64_t indexAddress; // scene index buffer (u32), 0 = none
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseGpuInstancesRef { FuseGpuInstance v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseGpuTransformsRef { FuseGpuTransform v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseGpuMeshesRef { FuseGpuMesh v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseGpuLightsRef { FuseGpuLight v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseGpuMeshletsRef { FuseGpuMeshlet v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer FuseGpuSceneWordsRef { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer FuseGpuSceneIndicesRef { uint v[]; };

FuseGpuSceneHeaderRef fuse_gpu_scene(uint sceneHandle) { return FuseGpuSceneHeaderRef(fuse_buffer_address(sceneHandle)); }
uint64_t fuse_gpu_scene_address(FuseGpuSceneHeaderRef scene, uint table) { return scene.addresses[table]; }
FuseGpuInstancesRef fuse_gpu_scene_instances(FuseGpuSceneHeaderRef scene) { return FuseGpuInstancesRef(scene.addresses[FUSE_GPU_SCENE_INSTANCES]); }
FuseGpuTransformsRef fuse_gpu_scene_transforms(FuseGpuSceneHeaderRef scene) { return FuseGpuTransformsRef(scene.addresses[FUSE_GPU_SCENE_TRANSFORMS]); }
FuseGpuTransformsRef fuse_gpu_scene_prev_transforms(FuseGpuSceneHeaderRef scene) { return FuseGpuTransformsRef(scene.addresses[FUSE_GPU_SCENE_PREV_TRANSFORMS]); }
FuseGpuMeshesRef fuse_gpu_scene_meshes(FuseGpuSceneHeaderRef scene) { return FuseGpuMeshesRef(scene.addresses[FUSE_GPU_SCENE_MESHES]); }
FuseGpuLightsRef fuse_gpu_scene_lights(FuseGpuSceneHeaderRef scene) { return FuseGpuLightsRef(scene.addresses[FUSE_GPU_SCENE_LIGHTS]); }
FuseGpuSceneIndicesRef fuse_gpu_scene_indices(FuseGpuSceneHeaderRef scene) { return FuseGpuSceneIndicesRef(scene.indexAddress); }
uint fuse_gpu_mesh_draw_index_count(FuseGpuMesh m) { return m.indexCount != 0u ? m.indexCount : m.triangleCount * 3u; }

vec3 fuse_gpu_transform_point(FuseGpuTransform t, vec3 p) {
    const vec4 h = vec4(p, 1.0);
    return vec3(dot(t.rows[0], h), dot(t.rows[1], h), dot(t.rows[2], h));
}

#endif // FUSE_GPU_SCENE_GLSL
