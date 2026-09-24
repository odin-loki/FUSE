// WP-2.3 forward transparency, vertex stage. Slang twin of fw_forward.vert (see there).
// SV_InstanceID / SV_VertexID exclude the draw's base values: the draw index is SV_InstanceID +
// SV_StartInstanceLocation, the mesh-local vertex SV_VertexID + SV_StartVertexLocation.
#include "fw_includes.slang"

struct VSOut {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : UV;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float3 tangent : TANGENT;
    [[vk::location(3)]] nointerpolation float sign_ : SIGN;
    [[vk::location(4)]] nointerpolation uint draw : DRAW;
};

[shader("vertex")]
VSOut main(uint vertexId : SV_VertexID, uint baseVertex : SV_StartVertexLocation, uint instanceId : SV_InstanceID,
           uint baseInstance : SV_StartInstanceLocation) {
    const FwFrame f = fuseFwFrame();
    const uint d = instanceId + baseInstance;
    const FwDraw draw = ((FwDraw*)f.draws)[d];
    GpuSceneHeader* scene = fuseGpuScene(f.scene);
    const GpuInstance inst = fuseGpuSceneInstances(scene)[draw.slot];
    const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    const GpuTransform t = fuseGpuSceneTransforms(scene)[draw.slot];
    const MrVertex v = fuseMrVertex(mesh, vertexId + baseVertex);
    VSOut o;
    o.position = fuseVisClip(t, f.viewProj, v.position);
    o.uv = v.uv;
    o.normal = fuseMrTransformNormal(t, v.normal);
    o.tangent = fuseMrTransformVector(t, v.tangent);
    o.sign_ = v.sign_ * fuseMrDetSign(t);
    o.draw = d;
    return o;
}
