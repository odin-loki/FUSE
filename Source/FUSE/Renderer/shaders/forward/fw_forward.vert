#version 460
// WP-2.3 forward transparency, vertex stage: one vkCmdDrawIndexed per sorted transparent instance over
// the scene index buffer, firstInstance = the draw's index in the frame's ForwardDraw table (which
// names the instance slot), gl_VertexIndex = the mesh-local vertex; VPOS / VNRM / VTAN / VUV0 pulled
// through BDA (WP-1.2 decode) and positions through fuse_vis_clip, like the visibility raster (so a
// transparent instance covers exactly the pixels its opaque twin would). GLSL twin of fw_forward_vs.slang.
#extension GL_GOOGLE_include_directive : require
#include "fw_includes.glsl"

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec3 outTangent;
layout(location = 3) flat out float outSign;
layout(location = 4) flat out uint outDraw;

void main() {
    const FuseFwFrame f = fuse_fw_frame();
    const uint d = uint(gl_InstanceIndex);
    const FuseFwDraw draw = FuseFwDrawsRef(f.draws).v[d];
    FuseGpuSceneHeaderRef scene = fuse_gpu_scene(f.scene);
    const FuseGpuInstance inst = fuse_gpu_scene_instances(scene).v[draw.slot];
    const FuseGpuMesh mesh = fuse_gpu_scene_meshes(scene).v[inst.mesh];
    const FuseGpuTransform t = fuse_gpu_scene_transforms(scene).v[draw.slot];
    const FuseMrVertex v = fuse_mr_vertex(mesh, uint(gl_VertexIndex));
    gl_Position = fuse_vis_clip(t, f.viewProj, v.position);
    outUv = v.uv;
    outNormal = fuse_mr_transform_normal(t, v.normal);
    outTangent = fuse_mr_transform_vector(t, v.tangent);
    outSign = v.sign_ * fuse_mr_det_sign(t);
    outDraw = d;
}
