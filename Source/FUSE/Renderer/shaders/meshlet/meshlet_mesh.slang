// WP-5.1 mesh shader, Slang twin of meshlet.mesh (see there). Compile with -fp-mode precise (the
// vertex transform is the WP-1.4 vertex shader's, vis_common.slang fuseVisClip).
#include "gpu_scene.slang"
#include "vis_format.slang"
#include "vis_common.slang"
#include "cull_common.slang"
#include "meshlet_common.slang"

struct Push {
    float4 viewProj[4];
    uint scene;
    uint target64;
    uint width;
    uint height;
    uint64_t constants;
    uint mode;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

struct VertexOut {
    float4 position : SV_Position;
    [[vk::location(0)]] nointerpolation uint instance : INSTANCE;
};

struct PrimitiveOut {
    uint primitive : SV_PrimitiveID;
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void main(uint3 localId: SV_GroupThreadID, uint3 groupId: SV_GroupID, in payload MeshletPayload p,
          out vertices VertexOut verts[64], out indices uint3 tris[124], out primitives PrimitiveOut prims[124]) {
    const uint lane = localId.x;
    const uint instance = p.instance;
    const uint m = p.meshlets[groupId.x];
    GpuSceneHeader* scene = fuseGpuScene(pc.scene);
    const GpuInstance inst = fuseGpuSceneInstances(scene)[instance];
    const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    const GpuMeshlet ml = ((GpuMeshlet*)mesh.meshlets)[m];
    const GpuTransform xf = fuseGpuSceneTransforms(scene)[instance];
    const uint vertexCount = ml.counts & 0xFFu;
    const uint triangleCount = (ml.counts >> 8u) & 0xFFu;
    SetMeshOutputCounts(vertexCount, triangleCount);
    uint* mvrt = (uint*)mesh.meshletVertices;
    uint* mtri = (uint*)mesh.meshletTriangles;
    for (uint v = lane; v < vertexCount; v += kFuseMeshletMeshThreads) {
        const uint vertex = uint(int(mvrt[ml.vertexOffset + v]) + mesh.vertexOffset);
        VertexOut o;
        o.position = fuseVisClip(xf, pc.viewProj, fuseVisMeshPosition(mesh, vertex));
        o.instance = instance;
        verts[v] = o;
    }
    for (uint t = lane; t < triangleCount; t += kFuseMeshletMeshThreads) {
        const uint packed = mtri[ml.triangleOffset + t];
        tris[t] = uint3(packed & 0xFFu, (packed >> 8u) & 0xFFu, (packed >> 16u) & 0xFFu);
        PrimitiveOut o;
        o.primitive = ml.triangleOffset + t;
        prims[t] = o;
    }
}
