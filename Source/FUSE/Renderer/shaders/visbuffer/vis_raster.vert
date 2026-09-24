#version 460
// WP-1.4 visibility-buffer raster, vertex stage (both targets). GLSL twin of vis_raster_vs.slang.
// Draws come from the WP-1.3 culler (vkCmdDrawIndexedIndirectCount over the scene index buffer):
// firstInstance = instance slot, the index range = the mesh's draw range, so gl_VertexIndex (index +
// vertexOffset) is the mesh-local vertex, pulled from VPOS through BDA. No vertex buffers.
#extension GL_GOOGLE_include_directive : require
#include "bindless.glsl"
#include "gpu_scene.glsl"
#include "vis_format.glsl"
#include "vis_common.glsl"

layout(push_constant) uniform Push {
    vec4 viewProj[4]; // columns
    uint scene;
    uint target64;
    uint width;
    uint height;
    uint pad[4];
} pc;

layout(location = 0) flat out uint outInstance;

void main() {
    const uint slot = uint(gl_InstanceIndex);
    FuseGpuSceneHeaderRef scene = fuse_gpu_scene(pc.scene);
    const FuseGpuInstance inst = fuse_gpu_scene_instances(scene).v[slot];
    const FuseGpuMesh mesh = fuse_gpu_scene_meshes(scene).v[inst.mesh];
    const FuseGpuTransform t = fuse_gpu_scene_transforms(scene).v[slot];
    gl_Position = fuse_vis_clip(t, pc.viewProj, fuse_vis_mesh_position(mesh, uint(gl_VertexIndex)));
    outInstance = slot;
}
