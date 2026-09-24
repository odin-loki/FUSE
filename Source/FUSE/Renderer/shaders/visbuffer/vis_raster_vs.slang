// WP-1.4 visibility-buffer raster, vertex stage (both targets). Slang twin of vis_raster.vert.
// SV_InstanceID / SV_VertexID do not include the draw's base values, so the slot is
// SV_InstanceID + SV_StartInstanceLocation and the mesh-local vertex SV_VertexID +
// SV_StartVertexLocation (== gl_VertexIndex: index + vertexOffset).
#include "gpu_scene.slang"
#include "vis_format.slang"
#include "vis_common.slang"

struct Push {
    float4 viewProj[4]; // columns
    uint scene;
    uint target64;
    uint width;
    uint height;
    uint pad[4];
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

struct VSOut {
    float4 position : SV_Position;
    [[vk::location(0)]] nointerpolation uint instance : INSTANCE;
};

[shader("vertex")]
VSOut main(uint vertexId : SV_VertexID, uint baseVertex : SV_StartVertexLocation, uint instanceId : SV_InstanceID,
           uint baseInstance : SV_StartInstanceLocation) {
    const uint slot = instanceId + baseInstance;
    GpuSceneHeader* scene = fuseGpuScene(pc.scene);
    const GpuInstance inst = fuseGpuSceneInstances(scene)[slot];
    const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    const GpuTransform t = fuseGpuSceneTransforms(scene)[slot];
    VSOut o;
    o.position = fuseVisClip(t, pc.viewProj, fuseVisMeshPosition(mesh, vertexId + baseVertex));
    o.instance = slot;
    return o;
}
