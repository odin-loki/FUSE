// FUSE visibility buffer (WP-1.4): vertex pulling and triangle decode shared by the raster, decode
// and (WP-1.5) material-resolve shaders. GLSL twin of vis_common.slang; the decode math is a
// line-for-line twin of include/fuse/renderer/visbuffer/vis_decode_kernel.hpp (the CPU reference).
//
// Include after bindless.glsl, gpu_scene.glsl and vis_format.glsl.
//
// Index layout (gpu_scene_types.hpp): triangle t of an instance's mesh has the vertices
//   indices[mesh.firstIndex + 3t + k] + mesh.vertexOffset,  k = 0, 1, 2
// in the scene index buffer (header.indexAddress); positions are VPOS u16x4 at mesh.positions,
// decoded exactly as quantOffset + float(q) * quantStep.
#ifndef FUSE_VIS_COMMON_GLSL
#define FUSE_VIS_COMMON_GLSL

#define FUSE_VIS_DECODE_EMPTY 0u      // pixel has no geometry
#define FUSE_VIS_DECODE_OK 1u
#define FUSE_VIS_DECODE_BAD_ID 2u     // instance / mesh / triangle id out of range
#define FUSE_VIS_DECODE_DEGENERATE 3u // zero-area triangle as seen from the pixel

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer FuseVisWordsRef { uint v[]; };

// Quantised VPOS of `vertex` (mesh-local), decoded bit-exactly.
vec3 fuse_vis_mesh_position(FuseGpuMesh mesh, uint vertex) {
    FuseVisWordsRef words = FuseVisWordsRef(mesh.positions);
    const uint xy = words.v[vertex * 2u];
    const uint zw = words.v[vertex * 2u + 1u];
    precise vec3 p;
    p.x = mesh.quantOffset[0] + float(xy & 0xFFFFu) * mesh.quantStep[0];
    p.y = mesh.quantOffset[1] + float(xy >> 16u) * mesh.quantStep[1];
    p.z = mesh.quantOffset[2] + float(zw & 0xFFFFu) * mesh.quantStep[2];
    return p;
}

// Clip-space position of an object-space point (column-major viewProj columns).
vec4 fuse_vis_clip(FuseGpuTransform t, vec4 viewProj[4], vec3 p) {
    precise vec3 w;
    const vec4 h = vec4(p, 1.0);
    w.x = t.rows[0].x * h.x + t.rows[0].y * h.y + t.rows[0].z * h.z + t.rows[0].w;
    w.y = t.rows[1].x * h.x + t.rows[1].y * h.y + t.rows[1].z * h.z + t.rows[1].w;
    w.z = t.rows[2].x * h.x + t.rows[2].y * h.y + t.rows[2].z * h.z + t.rows[2].w;
    precise vec4 c = viewProj[0] * w.x + viewProj[1] * w.y + viewProj[2] * w.z + viewProj[3];
    return c;
}

struct FuseVisDecoded {
    float depth; // z / w of the triangle's plane at the pixel centre (what the rasteriser stores)
    float b1;    // perspective-correct barycentrics of vertices 1 and 2 (b0 = 1 - b1 - b2)
    float b2;
    uint flags;  // FUSE_VIS_DECODE_*
};

// Perspective-correct barycentrics of the pixel whose NDC centre is `ndc`, from the homogeneous
// clip positions: the point P = sum(b_i c_i) with sum(b_i) = 1 projects to ndc, i.e. sum(b_i u_i) = 0
// with u_i = c_i.xy - ndc * c_i.w, so b is the normalised cross product of the rows of [u0 u1 u2].
// No per-vertex divide, valid for vertices behind the camera.
FuseVisDecoded fuse_vis_decode_clip(vec4 c0, vec4 c1, vec4 c2, vec2 ndc) {
    FuseVisDecoded r;
    precise float u0x = c0.x - ndc.x * c0.w;
    precise float u0y = c0.y - ndc.y * c0.w;
    precise float u1x = c1.x - ndc.x * c1.w;
    precise float u1y = c1.y - ndc.y * c1.w;
    precise float u2x = c2.x - ndc.x * c2.w;
    precise float u2y = c2.y - ndc.y * c2.w;
    precise float e0 = u1x * u2y - u1y * u2x;
    precise float e1 = u2x * u0y - u2y * u0x;
    precise float e2 = u0x * u1y - u0y * u1x;
    precise float s = e0 + e1 + e2;
    if (s == 0.0) {
        r.depth = 1.0;
        r.b1 = 0.0;
        r.b2 = 0.0;
        r.flags = FUSE_VIS_DECODE_DEGENERATE;
        return r;
    }
    precise float b0 = e0 / s;
    precise float b1 = e1 / s;
    precise float b2 = e2 / s;
    precise float z = b0 * c0.z + b1 * c1.z + b2 * c2.z;
    precise float w = b0 * c0.w + b1 * c1.w + b2 * c2.w;
    r.depth = z / w;
    r.b1 = b1;
    r.b2 = b2;
    r.flags = FUSE_VIS_DECODE_OK;
    return r;
}

// NDC centre of pixel (x, y) of a width x height target (Vulkan viewport, y down).
vec2 fuse_vis_pixel_ndc(uvec2 pixel, uint width, uint height) {
    precise vec2 ndc;
    ndc.x = (float(pixel.x) + 0.5) * (2.0 / float(width)) - 1.0;
    ndc.y = (float(pixel.y) + 0.5) * (2.0 / float(height)) - 1.0;
    return ndc;
}

// Full decode of a visibility sample: instance -> mesh -> index range -> positions -> clip -> pixel.
FuseVisDecoded fuse_vis_decode(FuseGpuSceneHeaderRef scene, vec4 viewProj[4], uint instance, uint triangle, vec2 ndc) {
    FuseVisDecoded r;
    r.depth = 1.0;
    r.b1 = 0.0;
    r.b2 = 0.0;
    r.flags = FUSE_VIS_DECODE_EMPTY;
    if (instance == FUSE_VIS_INVALID) {
        return r;
    }
    r.flags = FUSE_VIS_DECODE_BAD_ID;
    if (instance >= scene.counts[FUSE_GPU_SCENE_INSTANCES]) {
        return r;
    }
    const FuseGpuInstance inst = fuse_gpu_scene_instances(scene).v[instance];
    if ((inst.flags & FUSE_INSTANCE_VALID) == 0u || inst.mesh >= scene.counts[FUSE_GPU_SCENE_MESHES]) {
        return r;
    }
    const FuseGpuMesh mesh = fuse_gpu_scene_meshes(scene).v[inst.mesh];
    if (mesh.indexCount == 0u || scene.indexAddress == 0ul || triangle >= mesh.indexCount / 3u) {
        return r;
    }
    FuseGpuSceneIndicesRef indices = fuse_gpu_scene_indices(scene);
    const FuseGpuTransform t = fuse_gpu_scene_transforms(scene).v[instance];
    const uint base = mesh.firstIndex + triangle * 3u;
    const vec4 c0 = fuse_vis_clip(t, viewProj, fuse_vis_mesh_position(mesh, uint(int(indices.v[base]) + mesh.vertexOffset)));
    const vec4 c1 = fuse_vis_clip(t, viewProj, fuse_vis_mesh_position(mesh, uint(int(indices.v[base + 1u]) + mesh.vertexOffset)));
    const vec4 c2 = fuse_vis_clip(t, viewProj, fuse_vis_mesh_position(mesh, uint(int(indices.v[base + 2u]) + mesh.vertexOffset)));
    return fuse_vis_decode_clip(c0, c1, c2, ndc);
}

#endif // FUSE_VIS_COMMON_GLSL
