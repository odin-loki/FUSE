// WP-5.4 hardware path for the HW-classified clusters, vertex stage. Slang twin of swraster_hw.vert
// (see there). The draw's firstVertex / firstInstance are 0, so SV_VertexID / SV_InstanceID are
// gl_VertexIndex / gl_InstanceIndex. Compile with -fp-mode precise (the WP-1.4 vertex transform).
#include "gpu_scene.slang"
#include "vis_format.slang"
#include "vis_common.slang"
#include "swraster_common.slang"

[[vk::binding(3, 0)]] StructuredBuffer<uint> fuse_sw_words[];

struct Push {
    uint64_t constants;
    uint region;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

struct VSOut {
    float4 position : SV_Position;
    [[vk::location(0)]] nointerpolation uint instance : INSTANCE;
    [[vk::location(1)]] nointerpolation uint triangle : TRIANGLE;
};

[shader("vertex")]
VSOut main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    SwConstants* C = (SwConstants*)pc.constants;
    const uint cap = C.capacity;
    const uint ci = instanceId;
    VSOut o;
    o.position = float4(2.0, 2.0, 2.0, 1.0);
    o.instance = kFuseVisInvalid;
    o.triangle = kFuseVisInvalid;
    if ((C.flags & kFuseSwFlagSkipHardware) != 0u || ci >= cap) {
        return o;
    }
    const uint hw = fuseSwHandleIndex(C.hwBuffer);
    const uint r = (pc.region * cap + ci) * 2u;
    const uint instance = fuse_sw_words[hw][r];
    const uint meshlet = fuse_sw_words[hw][r + 1u];
    const uint t = vertexId / 3u;
    const uint k = vertexId % 3u;
    GpuSceneHeader* scene = fuseGpuScene(C.scene);
    const GpuInstance inst = fuseGpuSceneInstances(scene)[instance];
    const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    const GpuMeshlet ml = ((GpuMeshlet*)mesh.meshlets)[meshlet];
    if (t >= fuseSwMeshletTriangles(ml)) {
        return o;
    }
    const uint packed = ((uint*)mesh.meshletTriangles)[ml.triangleOffset + t];
    const uint local = (packed >> (8u * k)) & 0xFFu;
    const uint vertex = uint(int(((uint*)mesh.meshletVertices)[ml.vertexOffset + local]) + mesh.vertexOffset);
    float4 vp[4];
    fuseSwViewProj(C, vp);
    const GpuTransform xf = fuseGpuSceneTransforms(scene)[instance];
    o.position = fuseVisClip(xf, vp, fuseVisMeshPosition(mesh, vertex));
    o.instance = instance;
    o.triangle = ml.triangleOffset + t;
    return o;
}
