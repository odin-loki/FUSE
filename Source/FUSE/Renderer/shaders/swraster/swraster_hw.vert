#version 460
// WP-5.4 hardware path for the HW-classified clusters, vertex stage. GLSL twin of swraster_hw_vs.slang.
// One instanced, non-indexed draw per region (vkCmdDrawIndirect: vertexCount = 372, instanceCount =
// the region's HW list length): instance = HW cluster, vertex 3t + k = corner k of the meshlet's
// triangle t through the WP-1.4 vertex transform (vis_common fuse_vis_clip of the same VPOS), flat
// (instance, mesh triangle = meshlet.triangleOffset + t) for the fragment stage. Triangle slots past
// the meshlet's count (and everything under FUSE_SW_FLAG_SKIP_HARDWARE) emit a point outside the clip
// volume: the triangle is clipped away.
#extension GL_GOOGLE_include_directive : require
#include "bindless.glsl"
#include "gpu_scene.glsl"
#include "vis_format.glsl"
#include "vis_common.glsl"
#include "swraster_common.glsl"

FUSE_BINDLESS_SSBO_LAYOUT readonly buffer FuseSwWords { uint w[]; } fuse_sw_words[];

layout(push_constant) uniform Push {
    uint64_t constants;
    uint region;
    uint pad;
} pc;

layout(location = 0) flat out uint outInstance;
layout(location = 1) flat out uint outTriangle;

void main() {
    FuseSwConstantsRef C = FuseSwConstantsRef(pc.constants);
    const uint cap = C.capacity;
    const uint ci = uint(gl_InstanceIndex);
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    outInstance = FUSE_VIS_INVALID;
    outTriangle = FUSE_VIS_INVALID;
    if ((C.flags & FUSE_SW_FLAG_SKIP_HARDWARE) != 0u || ci >= cap) {
        return;
    }
    const uint hw = fuse_handle_index(C.hwBuffer);
    const uint r = (pc.region * cap + ci) * 2u;
    const uint instance = fuse_sw_words[hw].w[r];
    const uint meshlet = fuse_sw_words[hw].w[r + 1u];
    const uint t = uint(gl_VertexIndex) / 3u;
    const uint k = uint(gl_VertexIndex) % 3u;
    FuseGpuSceneHeaderRef scene = fuse_gpu_scene(C.scene);
    const FuseGpuInstance inst = fuse_gpu_scene_instances(scene).v[instance];
    const FuseGpuMesh mesh = fuse_gpu_scene_meshes(scene).v[inst.mesh];
    const FuseGpuMeshlet ml = FuseGpuMeshletsRef(mesh.meshlets).v[meshlet];
    if (t >= fuse_sw_meshlet_triangles(ml)) {
        return;
    }
    const uint packed = FuseGpuSceneWordsRef(mesh.meshletTriangles).v[ml.triangleOffset + t];
    const uint local = (packed >> (8u * k)) & 0xFFu;
    const uint vertex = uint(int(FuseGpuSceneWordsRef(mesh.meshletVertices).v[ml.vertexOffset + local]) + mesh.vertexOffset);
    vec4 vp[4];
    fuse_sw_view_proj(C, vp);
    const FuseGpuTransform xf = fuse_gpu_scene_transforms(scene).v[instance];
    gl_Position = fuse_vis_clip(xf, vp, fuse_vis_mesh_position(mesh, vertex));
    outInstance = instance;
    outTriangle = ml.triangleOffset + t;
}
