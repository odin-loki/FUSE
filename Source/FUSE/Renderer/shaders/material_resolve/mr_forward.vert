#version 460
// WP-1.5 forward G-buffer reference, vertex stage: the WP-1.3 culler's indexed indirect draws over the
// scene index buffer (as the visibility raster: firstInstance = slot, gl_VertexIndex = mesh-local
// vertex, VPOS / VNRM / VTAN / VUV0 pulled through BDA). Positions use fuse_vis_clip, so coverage and
// depth equal the visibility buffer's; the attributes are interpolated by the rasteriser. GLSL only
// (a test oracle / fallback, like the legacy shaders/raster/gbuffer.* it extends to the GPU scene).
#extension GL_GOOGLE_include_directive : require
#include "mr_includes.glsl"

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec3 outTangent;
layout(location = 3) flat out float outSign;
layout(location = 4) out vec4 outPrevClip;
layout(location = 5) flat out uint outInstance;

void main() {
    const FuseMrFrame f = FuseMrFrameRef(pc.frame).f;
    const uint slot = uint(gl_InstanceIndex);
    FuseGpuSceneHeaderRef scene = fuse_gpu_scene(f.scene);
    const FuseGpuInstance inst = fuse_gpu_scene_instances(scene).v[slot];
    const FuseGpuMesh mesh = fuse_gpu_scene_meshes(scene).v[inst.mesh];
    const FuseGpuTransform t = fuse_gpu_scene_transforms(scene).v[slot];
    const FuseGpuTransform pt = fuse_gpu_scene_prev_transforms(scene).v[slot];
    const FuseMrVertex v = fuse_mr_vertex(mesh, uint(gl_VertexIndex));
    gl_Position = fuse_vis_clip(t, f.viewProj, v.position);
    outPrevClip = fuse_vis_clip(pt, f.prevViewProj, v.position);
    outUv = v.uv;
    outNormal = fuse_mr_transform_normal(t, v.normal);
    outTangent = fuse_mr_transform_vector(t, v.tangent);
    outSign = v.sign_ * fuse_mr_det_sign(t); // flat: the provoking (first) vertex, as the resolve's v[0]
    outInstance = slot;
}
