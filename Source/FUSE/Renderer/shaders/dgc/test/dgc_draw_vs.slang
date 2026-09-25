// WP-9.3 test raster (fuse_rp_dgc), Slang twin of dgc_draw.vert. SV_InstanceID does not include the
// base instance, so the slot is SV_InstanceID + SV_StartInstanceLocation.
#include "gpu_scene.slang"

struct Push {
    float4 viewProj[4]; // columns
    uint scene;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

struct VSOut {
    float4 position : SV_Position;
    [[vk::location(0)]] nointerpolation uint instance : INSTANCE;
};

[shader("vertex")]
VSOut main([[vk::location(0)]] float3 position : POSITION, uint instanceId : SV_InstanceID,
           uint baseInstance : SV_StartInstanceLocation) {
    const uint slot = instanceId + baseInstance;
    const GpuTransform t = fuseGpuSceneTransforms(fuseGpuScene(pc.scene))[slot];
    const float3 w = fuseGpuTransformPoint(t, position);
    VSOut o;
    o.position = pc.viewProj[0] * w.x + pc.viewProj[1] * w.y + pc.viewProj[2] * w.z + pc.viewProj[3];
    o.instance = slot;
    return o;
}
