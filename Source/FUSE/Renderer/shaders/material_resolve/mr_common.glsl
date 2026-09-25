// FUSE material resolve (WP-1.5): attribute reconstruction from the visibility buffer, material
// evaluation and G-buffer packing, shared by the classify / resolve / attribute-dump kernels and by the
// forward G-buffer reference. GLSL twin of mr_common.slang; the attribute math is a line-for-line twin
// of include/fuse/renderer/material_resolve/resolve_kernel.hpp (the CPU reference, see there for the
// derivation of the analytic barycentric derivatives).
//
// Include after bindless.glsl, gpu_scene.glsl, vis_format.glsl, vis_common.glsl, material.glsl and
// gbuffer.glsl. Material textures are always sampled with explicit gradients (textureGrad): the
// resolve passes the analytic UV derivatives, the forward reference the hardware ones (dFdx / dFdy
// taken in uniform control flow), so the two paths differ only in where the derivatives come from.
#ifndef FUSE_MR_COMMON_GLSL
#define FUSE_MR_COMMON_GLSL

#define FUSE_MR_BIN_EMPTY 0u
#define FUSE_MR_BIN_FLAT 1u
#define FUSE_MR_BIN_TEXTURED 2u
#define FUSE_MR_BIN_NORMAL_MAPPED 3u
#define FUSE_MR_BIN_COUNT 4u
#define FUSE_MR_BIN_UBER 4u
#define FUSE_MR_BIN_LAYERED 5u // asset W0.7 layered materials (resolve_types.hpp kBinLayered)
#define FUSE_MR_TILE 8u
#define FUSE_MR_FEATURE_TEXTURES 1u
#define FUSE_MR_FEATURE_NORMAL_MAP 2u
#define FUSE_MR_FEATURE_LAYERED 4u
#define FUSE_MR_ATTR_EMPTY 0u
#define FUSE_MR_ATTR_OK 1u
#define FUSE_MR_ATTR_BAD_ID 2u
#define FUSE_MR_ATTR_DEGENERATE 3u
#define FUSE_MR_NO_MATERIAL 0xFFFFFFFFu
#define FUSE_MR_BIN_LIST_OFFSET_WORDS 20u // ResolveBinLayout::kListOffset / 4 (args + draw count)

// ResolveFrameConstants (resolve_types.hpp), 176 bytes.
struct FuseMrFrame {
    vec4 viewProj[4];     // columns
    vec4 prevViewProj[4];
    uint width;
    uint height;
    uint tilesX;
    uint tilesY;
    uint scene;
    uint vis;
    uint sampler_;
    uint tileCapacity;
    uint64_t bins;
    uint binsHandle;
    uint layered; // bindless storage-buffer handle of the layered-material table (MlResolveTable), 0 = none
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseMrFrameRef { FuseMrFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseMrBinsRef { uint words[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseMrMaterialsRef { FuseGpuMaterial v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseMrSubmeshesRef { FuseGpuSubmesh v[]; };

uint fuse_mr_bin_features(uint bin) {
    return bin >= FUSE_MR_BIN_NORMAL_MAPPED ? (FUSE_MR_FEATURE_TEXTURES | FUSE_MR_FEATURE_NORMAL_MAP)
                                            : (bin == FUSE_MR_BIN_TEXTURED ? FUSE_MR_FEATURE_TEXTURES : 0u);
}

// resolve_types.hpp resolve_features: + the layered evaluation for the layered bin and the uber path.
uint fuse_mr_resolve_features(uint bin) {
    return bin >= FUSE_MR_BIN_UBER ? (FUSE_MR_FEATURE_TEXTURES | FUSE_MR_FEATURE_NORMAL_MAP | FUSE_MR_FEATURE_LAYERED)
                                   : fuse_mr_bin_features(bin);
}

// Bin-buffer words of bin `bin`'s VkDrawIndirectCommand and tile list (ResolveBinLayout::argsOffset / listOffset:
// the layered bin is appended after the four feature-bin lists).
uint fuse_mr_bin_args_word(uint bin, uint tileCapacity) {
    return bin == FUSE_MR_BIN_LAYERED ? FUSE_MR_BIN_LIST_OFFSET_WORDS + 4u * tileCapacity : bin * 4u;
}
uint fuse_mr_bin_list_word(uint bin, uint tileCapacity) {
    return bin == FUSE_MR_BIN_LAYERED ? FUSE_MR_BIN_LIST_OFFSET_WORDS + 4u * tileCapacity + 4u
                                      : FUSE_MR_BIN_LIST_OFFSET_WORDS + bin * tileCapacity;
}

// --- vertices ------------------------------------------------------------------------------------
struct FuseMrVertex {
    vec3 position;
    vec3 normal;
    vec3 tangent;
    float sign_;
    vec2 uv;
};

float fuse_mr_snorm16(uint bits16) {
    const int q = int(bits16 << 16) >> 16;
    precise float v = float(q) * uintBitsToFloat(0x38000100u); // 1 / 32767 (vertex_codec kOctInvScale)
    return clamp(v, -1.0, 1.0);
}

// WP-1.2 oct decode (exact part), then x / |x| (resolve_kernel::oct_decode).
vec3 fuse_mr_oct_decode(uint packed) {
    precise float x = fuse_mr_snorm16(packed & 0xFFFFu);
    precise float y = fuse_mr_snorm16(packed >> 16u);
    precise float z = 1.0 - abs(x) - abs(y);
    const float t = z < 0.0 ? -z : 0.0;
    x += x >= 0.0 ? -t : t;
    y += y >= 0.0 ? -t : t;
    precise float len = sqrt(x * x + y * y + z * z);
    precise float inv = len > 0.0 ? 1.0 / len : 0.0;
    precise vec3 r;
    r.x = x * inv;
    r.y = y * inv;
    r.z = z * inv;
    return r;
}

FuseMrVertex fuse_mr_vertex(FuseGpuMesh mesh, uint v) {
    FuseMrVertex r;
    r.position = fuse_vis_mesh_position(mesh, v);
    FuseGpuSceneWordsRef vpos = FuseGpuSceneWordsRef(mesh.positions);
    r.sign_ = ((vpos.v[v * 2u + 1u] >> 16u) & 1u) != 0u ? -1.0 : 1.0;
    r.normal = vec3(0.0, 0.0, 1.0);
    r.tangent = vec3(1.0, 0.0, 0.0);
    r.uv = vec2(0.0);
    if (mesh.normals != 0ul) {
        r.normal = fuse_mr_oct_decode(FuseGpuSceneWordsRef(mesh.normals).v[v]);
    }
    if (mesh.tangents != 0ul) {
        r.tangent = fuse_mr_oct_decode(FuseGpuSceneWordsRef(mesh.tangents).v[v]);
    }
    if (mesh.uvs != 0ul) {
        r.uv = unpackHalf2x16(FuseGpuSceneWordsRef(mesh.uvs).v[v]);
    }
    return r;
}

// --- transforms ----------------------------------------------------------------------------------
vec3 fuse_mr_transform_point(FuseGpuTransform t, vec3 p) {
    precise vec3 r;
    r.x = t.rows[0].x * p.x + t.rows[0].y * p.y + t.rows[0].z * p.z + t.rows[0].w;
    r.y = t.rows[1].x * p.x + t.rows[1].y * p.y + t.rows[1].z * p.z + t.rows[1].w;
    r.z = t.rows[2].x * p.x + t.rows[2].y * p.y + t.rows[2].z * p.z + t.rows[2].w;
    return r;
}

vec3 fuse_mr_transform_vector(FuseGpuTransform t, vec3 v) {
    precise vec3 r;
    r.x = t.rows[0].x * v.x + t.rows[0].y * v.y + t.rows[0].z * v.z;
    r.y = t.rows[1].x * v.x + t.rows[1].y * v.y + t.rows[1].z * v.z;
    r.z = t.rows[2].x * v.x + t.rows[2].y * v.y + t.rows[2].z * v.z;
    return r;
}

float fuse_mr_det_sign(FuseGpuTransform t) {
    const vec3 a = t.rows[0].xyz;
    const vec3 b = t.rows[1].xyz;
    const vec3 c = t.rows[2].xyz;
    precise float d = a.x * (b.y * c.z - b.z * c.y) + a.y * (b.z * c.x - b.x * c.z) + a.z * (b.x * c.y - b.y * c.x);
    return d < 0.0 ? -1.0 : 1.0;
}

// cofactor(M) n, orientation kept under mirroring (resolve_kernel::transform_normal). Not normalised.
vec3 fuse_mr_transform_normal(FuseGpuTransform t, vec3 n) {
    const vec3 a = t.rows[0].xyz;
    const vec3 b = t.rows[1].xyz;
    const vec3 c = t.rows[2].xyz;
    const float sg = fuse_mr_det_sign(t);
    precise vec3 bc = vec3(b.y * c.z - b.z * c.y, b.z * c.x - b.x * c.z, b.x * c.y - b.y * c.x);
    precise vec3 ca = vec3(c.y * a.z - c.z * a.y, c.z * a.x - c.x * a.z, c.x * a.y - c.y * a.x);
    precise vec3 ab = vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    precise vec3 r;
    r.x = (bc.x * n.x + bc.y * n.y + bc.z * n.z) * sg;
    r.y = (ca.x * n.x + ca.y * n.y + ca.z * n.z) * sg;
    r.z = (ab.x * n.x + ab.y * n.y + ab.z * n.z) * sg;
    return r;
}

// --- barycentrics and their analytic derivatives -------------------------------------------------
struct FuseMrBary {
    vec3 b;
    vec3 dbdx;
    vec3 dbdy;
    float depth;
    uint flags;
};

// ndc: the pixel centre (fuse_vis_pixel_ndc); s: NDC units per pixel (2 / width, 2 / height).
// The b / depth operations are exactly fuse_vis_decode_clip's.
FuseMrBary fuse_mr_bary(vec4 c0, vec4 c1, vec4 c2, vec2 ndc, vec2 s) {
    FuseMrBary r;
    r.b = vec3(0.0);
    r.dbdx = vec3(0.0);
    r.dbdy = vec3(0.0);
    r.depth = 1.0;
    precise float u0x = c0.x - ndc.x * c0.w;
    precise float u0y = c0.y - ndc.y * c0.w;
    precise float u1x = c1.x - ndc.x * c1.w;
    precise float u1y = c1.y - ndc.y * c1.w;
    precise float u2x = c2.x - ndc.x * c2.w;
    precise float u2y = c2.y - ndc.y * c2.w;
    precise float e0 = u1x * u2y - u1y * u2x;
    precise float e1 = u2x * u0y - u2y * u0x;
    precise float e2 = u0x * u1y - u0y * u1x;
    precise float sum = e0 + e1 + e2;
    if (sum == 0.0) {
        r.flags = FUSE_MR_ATTR_DEGENERATE;
        return r;
    }
    precise float b0 = e0 / sum;
    precise float b1 = e1 / sum;
    precise float b2 = e2 / sum;
    precise float z = b0 * c0.z + b1 * c1.z + b2 * c2.z;
    precise float w = b0 * c0.w + b1 * c1.w + b2 * c2.w;
    r.depth = z / w;
    r.b = vec3(b0, b1, b2);
    precise float bx0 = c1.y * c2.w - c1.w * c2.y;
    precise float bx1 = c2.y * c0.w - c2.w * c0.y;
    precise float bx2 = c0.y * c1.w - c0.w * c1.y;
    precise float cy0 = c1.w * c2.x - c1.x * c2.w;
    precise float cy1 = c2.w * c0.x - c2.x * c0.w;
    precise float cy2 = c0.w * c1.x - c0.x * c1.w;
    precise float sbx = bx0 + bx1 + bx2;
    precise float scy = cy0 + cy1 + cy2;
    precise vec3 dx;
    dx.x = (bx0 - b0 * sbx) / sum * s.x;
    dx.y = (bx1 - b1 * sbx) / sum * s.x;
    dx.z = (bx2 - b2 * sbx) / sum * s.x;
    precise vec3 dy;
    dy.x = (cy0 - b0 * scy) / sum * s.y;
    dy.y = (cy1 - b1 * scy) / sum * s.y;
    dy.z = (cy2 - b2 * scy) / sum * s.y;
    r.dbdx = dx;
    r.dbdy = dy;
    r.flags = FUSE_MR_ATTR_OK;
    return r;
}

float fuse_mr_interp(vec3 b, float a0, float a1, float a2) {
    precise float r = b.x * a0 + b.y * a1 + b.z * a2;
    return r;
}

// --- materials -----------------------------------------------------------------------------------
uint fuse_mr_material_bin(FuseGpuMaterial m) {
    if ((m.flags & FUSE_GPU_MATERIAL_LAYERED) != 0u) {
        return FUSE_MR_BIN_LAYERED;
    }
    if (m.normal_tex != FUSE_INVALID_TEXTURE) {
        return FUSE_MR_BIN_NORMAL_MAPPED;
    }
    if (m.base_color_tex != FUSE_INVALID_TEXTURE || m.roughness_tex != FUSE_INVALID_TEXTURE ||
        m.metallic_tex != FUSE_INVALID_TEXTURE || m.ao_tex != FUSE_INVALID_TEXTURE || m.emissive_tex != FUSE_INVALID_TEXTURE) {
        return FUSE_MR_BIN_TEXTURED;
    }
    return FUSE_MR_BIN_FLAT;
}

// Material row of a mesh triangle: instance base row + the material index of the submesh whose MTRI
// range holds the triangle (resolve_kernel::triangle_material). FUSE_MR_NO_MATERIAL for none.
uint fuse_mr_triangle_material(FuseGpuSceneHeaderRef scene, FuseGpuInstance inst, FuseGpuMesh mesh, uint triangle) {
    if (inst.material == FUSE_GPU_SCENE_INVALID) {
        return FUSE_MR_NO_MATERIAL;
    }
    uint local = 0u;
    if (mesh.submeshes != 0ul && mesh.meshlets != 0ul) {
        FuseMrSubmeshesRef subs = FuseMrSubmeshesRef(mesh.submeshes);
        FuseGpuMeshletsRef meshlets = FuseGpuMeshletsRef(mesh.meshlets);
        for (uint i = 0u; i < mesh.submeshCount; ++i) {
            const FuseGpuSubmesh sub = subs.v[i];
            if (sub.meshletCount == 0u || sub.meshletOffset >= mesh.meshletCount) {
                continue;
            }
            const uint first = meshlets.v[sub.meshletOffset].triangleOffset;
            if (triangle >= first && triangle - first < sub.triangleCount) {
                local = sub.materialIndex;
                break;
            }
        }
    }
    const uint m = inst.material + local;
    return m < scene.counts[FUSE_GPU_SCENE_MATERIALS] ? m : FUSE_MR_NO_MATERIAL;
}

FuseGpuMaterial fuse_mr_material(FuseGpuSceneHeaderRef scene, uint index) {
    return FuseMrMaterialsRef(fuse_gpu_scene_address(scene, FUSE_GPU_SCENE_MATERIALS)).v[index];
}

// The visbuffer decode's id checks (fuse_vis_decode / resolve_kernel::ids_valid).
bool fuse_mr_ids_valid(FuseGpuSceneHeaderRef scene, uint instance, uint triangle) {
    if (instance >= scene.counts[FUSE_GPU_SCENE_INSTANCES]) {
        return false;
    }
    const FuseGpuInstance inst = fuse_gpu_scene_instances(scene).v[instance];
    if ((inst.flags & FUSE_INSTANCE_VALID) == 0u || inst.mesh >= scene.counts[FUSE_GPU_SCENE_MESHES]) {
        return false;
    }
    const FuseGpuMesh mesh = fuse_gpu_scene_meshes(scene).v[inst.mesh];
    return mesh.indexCount != 0u && scene.indexAddress != 0ul && triangle < mesh.indexCount / 3u;
}

// Resolve bin of a visibility sample (resolve_kernel::pixel_bin).
uint fuse_mr_pixel_bin(FuseGpuSceneHeaderRef scene, uvec2 vis) {
    if (vis.x == FUSE_VIS_INVALID || !fuse_mr_ids_valid(scene, vis.x, vis.y)) {
        return FUSE_MR_BIN_EMPTY;
    }
    const FuseGpuInstance inst = fuse_gpu_scene_instances(scene).v[vis.x];
    const FuseGpuMesh mesh = fuse_gpu_scene_meshes(scene).v[inst.mesh];
    const uint m = fuse_mr_triangle_material(scene, inst, mesh, vis.y);
    return m == FUSE_MR_NO_MATERIAL ? FUSE_MR_BIN_FLAT : fuse_mr_material_bin(fuse_mr_material(scene, m));
}

// --- attribute reconstruction (resolve_kernel::attributes) ---------------------------------------
struct FuseMrAttributes {
    uint flags;
    float depth;
    FuseMrBary bary;
    vec2 uv;
    vec2 duvdx;
    vec2 duvdy;
    vec3 normal;  // interpolated world normal (not renormalised)
    vec3 tangent; // interpolated world tangent (not renormalised)
    float tangentSign;
    vec2 velocity; // pixels, current - previous
    uint material;
    uint bin;
    vec3 position; // world position of the surface point + per-pixel derivatives (layered bin)
    vec3 dPdx;
    vec3 dPdy;
};

FuseMrAttributes fuse_mr_attributes(FuseGpuSceneHeaderRef scene, FuseMrFrame f, uvec2 pixel, uvec2 vis) {
    FuseMrAttributes r;
    r.flags = FUSE_MR_ATTR_EMPTY;
    r.depth = 1.0;
    r.bary.b = vec3(0.0);
    r.bary.dbdx = vec3(0.0);
    r.bary.dbdy = vec3(0.0);
    r.bary.depth = 1.0;
    r.bary.flags = FUSE_MR_ATTR_EMPTY;
    r.uv = vec2(0.0);
    r.duvdx = vec2(0.0);
    r.duvdy = vec2(0.0);
    r.normal = vec3(0.0);
    r.tangent = vec3(0.0);
    r.tangentSign = 1.0;
    r.velocity = vec2(0.0);
    r.material = FUSE_MR_NO_MATERIAL;
    r.bin = FUSE_MR_BIN_EMPTY;
    r.position = vec3(0.0);
    r.dPdx = vec3(0.0);
    r.dPdy = vec3(0.0);
    if (vis.x == FUSE_VIS_INVALID) {
        return r;
    }
    r.flags = FUSE_MR_ATTR_BAD_ID;
    if (!fuse_mr_ids_valid(scene, vis.x, vis.y)) {
        return r;
    }
    const FuseGpuInstance inst = fuse_gpu_scene_instances(scene).v[vis.x];
    const FuseGpuMesh mesh = fuse_gpu_scene_meshes(scene).v[inst.mesh];
    const FuseGpuTransform t = fuse_gpu_scene_transforms(scene).v[vis.x];
    const FuseGpuTransform pt = fuse_gpu_scene_prev_transforms(scene).v[vis.x];
    FuseGpuSceneIndicesRef indices = fuse_gpu_scene_indices(scene);
    const uint base = mesh.firstIndex + vis.y * 3u;
    FuseMrVertex v[3];
    vec4 c[3];
    vec4 pc[3];
    for (uint k = 0u; k < 3u; ++k) {
        const uint vi = uint(int(indices.v[base + k]) + mesh.vertexOffset);
        if (vi >= mesh.vertexCount) {
            return r;
        }
        v[k] = fuse_mr_vertex(mesh, vi);
        c[k] = fuse_vis_clip(t, f.viewProj, v[k].position);
        pc[k] = fuse_vis_clip(pt, f.prevViewProj, v[k].position);
    }
    r.material = fuse_mr_triangle_material(scene, inst, mesh, vis.y);
    r.bin = r.material == FUSE_MR_NO_MATERIAL ? FUSE_MR_BIN_FLAT : fuse_mr_material_bin(fuse_mr_material(scene, r.material));
    const vec2 ndc = fuse_vis_pixel_ndc(pixel, f.width, f.height);
    precise vec2 s;
    s.x = 2.0 / float(f.width);
    s.y = 2.0 / float(f.height);
    const FuseMrBary b = fuse_mr_bary(c[0], c[1], c[2], ndc, s);
    r.bary = b;
    r.flags = b.flags;
    if (b.flags != FUSE_MR_ATTR_OK) {
        return r;
    }
    r.depth = b.depth;
    r.uv = vec2(fuse_mr_interp(b.b, v[0].uv.x, v[1].uv.x, v[2].uv.x), fuse_mr_interp(b.b, v[0].uv.y, v[1].uv.y, v[2].uv.y));
    r.duvdx = vec2(fuse_mr_interp(b.dbdx, v[0].uv.x, v[1].uv.x, v[2].uv.x),
                   fuse_mr_interp(b.dbdx, v[0].uv.y, v[1].uv.y, v[2].uv.y));
    r.duvdy = vec2(fuse_mr_interp(b.dbdy, v[0].uv.x, v[1].uv.x, v[2].uv.x),
                   fuse_mr_interp(b.dbdy, v[0].uv.y, v[1].uv.y, v[2].uv.y));
    vec3 n[3];
    vec3 tg[3];
    for (uint k = 0u; k < 3u; ++k) {
        n[k] = fuse_mr_transform_normal(t, v[k].normal);
        tg[k] = fuse_mr_transform_vector(t, v[k].tangent);
    }
    r.normal = vec3(fuse_mr_interp(b.b, n[0].x, n[1].x, n[2].x), fuse_mr_interp(b.b, n[0].y, n[1].y, n[2].y),
                    fuse_mr_interp(b.b, n[0].z, n[1].z, n[2].z));
    r.tangent = vec3(fuse_mr_interp(b.b, tg[0].x, tg[1].x, tg[2].x), fuse_mr_interp(b.b, tg[0].y, tg[1].y, tg[2].y),
                     fuse_mr_interp(b.b, tg[0].z, tg[1].z, tg[2].z));
    r.tangentSign = v[0].sign_ * fuse_mr_det_sign(t);
    vec3 w[3];
    for (uint k = 0u; k < 3u; ++k) {
        w[k] = fuse_mr_transform_point(t, v[k].position);
    }
    r.position = vec3(fuse_mr_interp(b.b, w[0].x, w[1].x, w[2].x), fuse_mr_interp(b.b, w[0].y, w[1].y, w[2].y),
                      fuse_mr_interp(b.b, w[0].z, w[1].z, w[2].z));
    r.dPdx = vec3(fuse_mr_interp(b.dbdx, w[0].x, w[1].x, w[2].x), fuse_mr_interp(b.dbdx, w[0].y, w[1].y, w[2].y),
                  fuse_mr_interp(b.dbdx, w[0].z, w[1].z, w[2].z));
    r.dPdy = vec3(fuse_mr_interp(b.dbdy, w[0].x, w[1].x, w[2].x), fuse_mr_interp(b.dbdy, w[0].y, w[1].y, w[2].y),
                  fuse_mr_interp(b.dbdy, w[0].z, w[1].z, w[2].z));
    const float px = fuse_mr_interp(b.b, pc[0].x, pc[1].x, pc[2].x);
    const float py = fuse_mr_interp(b.b, pc[0].y, pc[1].y, pc[2].y);
    const float pw = fuse_mr_interp(b.b, pc[0].w, pc[1].w, pc[2].w);
    if (pw > 0.0) {
        precise float prevX = (px / pw * 0.5 + 0.5) * float(f.width);
        precise float prevY = (py / pw * 0.5 + 0.5) * float(f.height);
        precise vec2 vel;
        vel.x = (float(pixel.x) + 0.5) - prevX;
        vel.y = (float(pixel.y) + 0.5) - prevY;
        r.velocity = vel;
    }
    return r;
}

// --- material evaluation + G-buffer packing ------------------------------------------------------
vec4 fuse_mr_sample(uint tex, uint smp, vec2 uv, vec2 dx, vec2 dy) {
    return textureGrad(FUSE_TEXTURE_2D(tex, smp), uv, dx, dy);
}

struct FuseMrGBuffer {
    vec4 rt0;
    vec4 rt1;
    vec4 rt2;
    vec4 rt3;
    vec4 rt4;
    vec4 rt5;
    uint material;
};

FuseMrGBuffer fuse_mr_gbuffer_empty() {
    FuseMrGBuffer g;
    g.rt0 = vec4(0.0);
    g.rt1 = vec4(0.0);
    g.rt2 = vec4(0.0);
    g.rt3 = vec4(0.0);
    g.rt4 = vec4(1.0, 0.0, 0.0, 0.0); // far (forward depth)
    g.rt5 = vec4(0.0);
    g.material = FUSE_MR_NO_MATERIAL;
    return g;
}

// Evaluates the material row `material` (FUSE_MR_NO_MATERIAL: the default surface, GBufferPackedData's
// defaults) at the surface point and packs the G-buffer with write_gbuffer (shaders/common/gbuffer.glsl).
// Texture handles are bindless sampled-image slots (MaterialSystem: Texture::bindlessIndex; a full
// handle works too, only the index bits are used); 0xFFFFFFFF = no texture. `features` (FUSE_MR_FEATURE_*)
// is a compile-time constant in the binned pipelines: a pixel whose material needs no more than
// `features` gets exactly the uber result.
FuseMrGBuffer fuse_mr_shade(FuseGpuSceneHeaderRef scene, uint material, uint smp, uint features, vec2 uv, vec2 dx,
                            vec2 dy, vec3 nInterp, vec3 tInterp, float tSign, float depth, vec2 velocity) {
    vec3 albedo = vec3(1.0);
    float roughness = 0.5;
    float metallic = 0.0;
    float ao = 1.0;
    vec3 emissive = vec3(0.0);
    uint shading = 0u;
    precise float nl = nInterp.x * nInterp.x + nInterp.y * nInterp.y + nInterp.z * nInterp.z;
    vec3 N = nl > 0.0 ? nInterp / sqrt(nl) : vec3(0.0, 0.0, 1.0);
    if (material != FUSE_MR_NO_MATERIAL) {
        const FuseGpuMaterial m = fuse_mr_material(scene, material);
        albedo = m.base_color.rgb;
        roughness = m.roughness_emissive.x;
        metallic = m.base_color.w;
        emissive = fuse_material_emissive_radiance(m);
        shading = m.shading_model;
        if ((features & FUSE_MR_FEATURE_TEXTURES) != 0u) {
            if (m.base_color_tex != FUSE_INVALID_TEXTURE) {
                albedo *= fuse_mr_sample(m.base_color_tex, smp, uv, dx, dy).rgb;
            }
            if (m.roughness_tex != FUSE_INVALID_TEXTURE) {
                roughness *= fuse_mr_sample(m.roughness_tex, smp, uv, dx, dy).r;
            }
            if (m.metallic_tex != FUSE_INVALID_TEXTURE) {
                metallic *= fuse_mr_sample(m.metallic_tex, smp, uv, dx, dy).r;
            }
            if (m.ao_tex != FUSE_INVALID_TEXTURE) {
                ao = fuse_mr_sample(m.ao_tex, smp, uv, dx, dy).r;
            }
            if (m.emissive_tex != FUSE_INVALID_TEXTURE) {
                emissive *= fuse_mr_sample(m.emissive_tex, smp, uv, dx, dy).rgb;
            }
        }
        if ((features & FUSE_MR_FEATURE_NORMAL_MAP) != 0u && m.normal_tex != FUSE_INVALID_TEXTURE) {
            precise float nt = N.x * tInterp.x + N.y * tInterp.y + N.z * tInterp.z;
            precise vec3 T = tInterp - N * nt;
            precise float tl = T.x * T.x + T.y * T.y + T.z * T.z;
            if (tl > 0.0) {
                T = T / sqrt(tl);
                precise vec3 B = vec3(N.y * T.z - N.z * T.y, N.z * T.x - N.x * T.z, N.x * T.y - N.y * T.x) * tSign;
                precise vec3 ts = fuse_mr_sample(m.normal_tex, smp, uv, dx, dy).xyz * 2.0 - 1.0;
                ts.xy *= m.normal_strength;
                precise vec3 mapped = T * ts.x + B * ts.y + N * ts.z;
                precise float ml = mapped.x * mapped.x + mapped.y * mapped.y + mapped.z * mapped.z;
                if (ml > 0.0) {
                    N = mapped / sqrt(ml);
                }
            }
        }
    }
    FuseMrGBuffer g;
    write_gbuffer(N, albedo, roughness, metallic, emissive, ao, velocity, shading, g.rt0, g.rt1, g.rt2, g.rt3, g.rt5);
    g.rt4 = vec4(depth, 0.0, 0.0, 0.0);
    g.material = material;
    return g;
}

#endif // FUSE_MR_COMMON_GLSL
