#version 460
// WP-1.3 test raster (fuse_rp_culling): draws GPU-scene instances from the indirect-count args.
// gl_InstanceIndex includes firstInstance, which the cull wrote as the instance slot; the transform
// comes from the scene through BDA. GLSL twin of cull_draw_vs.slang.
#extension GL_GOOGLE_include_directive : require
#include "bindless.glsl"
#include "gpu_scene.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 0) flat out uint outInstance;

layout(push_constant) uniform Push {
    vec4 viewProj[4]; // columns
    uint scene;
} pc;

void main() {
    const FuseGpuTransform t = fuse_gpu_scene_transforms(fuse_gpu_scene(pc.scene)).v[gl_InstanceIndex];
    const vec3 w = fuse_gpu_transform_point(t, inPosition);
    gl_Position = pc.viewProj[0] * w.x + pc.viewProj[1] * w.y + pc.viewProj[2] * w.z + pc.viewProj[3];
    outInstance = uint(gl_InstanceIndex);
}
