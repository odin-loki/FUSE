#pragma once

// WP-1.5 material resolve, CPU reference as single-source kernels (docs/compute-kernels.md):
//
//   "material_resolve_attributes"  one pixel per item: visibility sample -> triangle -> barycentrics
//                                  and their analytic screen-space derivatives -> interpolated UV0
//                                  (+ d/dx, d/dy), world normal, world tangent, velocity, material
//                                  row and resolve bin (ResolveAttributeTexel)
//   "material_resolve_classify"    one 8 x 8 tile per item: the tile's bin = the most demanding
//                                  pixel bin in it (ResolveBin)
//
// shaders/material_resolve/mr_common.{glsl,slang} are line-for-line twins of the functions below
// (same operations in the same order, GLSL `precise`, Slang -fp-mode precise); fuse_rp_material_resolve
// checks the GPU attribute dump and the GPU tile lists against these kernels.
//
// Barycentrics (the visbuffer decode, vis_decode_kernel.hpp): with u_i = c_i.xy - ndc * c_i.w,
// e_i = u_j x u_k (cyclic j, k), s = e0 + e1 + e2, b_i = e_i / s. Each e_i is affine in the pixel's
// NDC position (the ndc.x * ndc.y terms cancel):
//   de_i/dndc.x = B_i = c_j.y c_k.w - c_j.w c_k.y,   de_i/dndc.y = C_i = c_j.w c_k.x - c_j.x c_k.w,
// so the exact derivatives of the perspective-correct barycentrics are
//   db_i/dx = (B_i - b_i * sum B) / s * (2 / width),   db_i/dy = (C_i - b_i * sum C) / s * (2 / height)
// (per pixel; no hardware derivatives, no finite differences, valid for vertices behind the camera).
// Any attribute A = sum b_i A_i then has dA/dx = sum (db_i/dx) A_i: that is what the resolve hands
// to textureGrad / SampleGrad (texture LOD and anisotropic footprint).
//
// Layered bin (asset W0.7): material rows with gpu_scene::kGpuMaterialLayered are binned as kBinLayered;
// world_position (the surface point + its per-pixel derivatives, for triplanar projection and texture
// footprints) and camera_centre (the view distance of the detail fade) are twins of mr_common /
// mr_layered; resolve_reference.hpp builds the layered surface record from them.
//
// GPU parity: adds and multiplies are correctly rounded on both sides (no contraction); divides,
// sqrt and the per-vertex normalisation are not on Vulkan, so GPU and CPU agree to a few ulps
// (fuse_rp_material_resolve uses relative tolerances scaled by each quantity's magnitude).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/geometry/vertex_codec_kernel.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/material_resolve/resolve_types.hpp>
#include <fuse/renderer/visbuffer/vis_decode_kernel.hpp>
#include <fuse/renderer/visbuffer/vis_format.hpp>

#include <cmath>

namespace fuse::renderer::material_resolve::resolve_kernel {

using gpu_scene::GpuInstance;
using gpu_scene::GpuMesh;
using gpu_scene::GpuSubmesh;
using gpu_scene::GpuTransform;
using visbuffer::decode_kernel::Clip;
using GpuMaterial = Material::GPUMaterial;

inline constexpr const char* kAttributesName = "material_resolve_attributes";
inline constexpr const char* kClassifyName = "material_resolve_classify";
inline constexpr u32 kInvalid = 0xFFFFFFFFu;

/// CPU view of one mesh's vertex streams (null = the stream's address is 0 in GpuMesh).
struct MeshStreams {
    const u16* vpos = nullptr;     ///< VPOS u16 x 4 per vertex
    const u32* normals = nullptr;  ///< VNRM oct snorm16 x 2
    const u32* tangents = nullptr; ///< VTAN oct snorm16 x 2
    const u32* uvs = nullptr;      ///< VUV0 half x 2
    u32 vertexCount = 0;
    const GpuSubmesh* submeshes = nullptr;          ///< GpuMesh::submeshCount entries
    const u32* meshletTriangleOffsets = nullptr;    ///< GpuMeshlet::triangleOffset per meshlet (GpuMesh::meshletCount)
};

/// One decoded vertex (mr_common.glsl FuseMrVertex).
struct Vertex {
    f32 position[3] = {0.f, 0.f, 0.f};
    f32 normal[3] = {0.f, 0.f, 1.f};
    f32 tangent[3] = {1.f, 0.f, 0.f};
    f32 sign = 1.f;
    f32 uv[2] = {0.f, 0.f};
};

/// Oct decode + normalisation (fuse_mr_oct_decode): the WP-1.2 contract, then x / |x|.
FUSE_HOST_DEVICE inline void oct_decode(u32 packed, f32 out[3]) {
    geometry::vertex_codec::oct_decode_raw(packed, out);
    const f32 len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    const f32 inv = len > 0.f ? 1.f / len : 0.f;
    out[0] = out[0] * inv;
    out[1] = out[1] * inv;
    out[2] = out[2] * inv;
}

FUSE_HOST_DEVICE inline Vertex fetch_vertex(const GpuMesh& mesh, const MeshStreams& s, u32 v) {
    Vertex r{};
    visbuffer::decode_kernel::mesh_position(mesh, s.vpos, v, r.position);
    r.sign = (s.vpos[v * 4u + 3u] & geometry::kVposTangentNegative) != 0u ? -1.f : 1.f;
    if (s.normals != nullptr) {
        oct_decode(s.normals[v], r.normal);
    }
    if (s.tangents != nullptr) {
        oct_decode(s.tangents[v], r.tangent);
    }
    if (s.uvs != nullptr) {
        r.uv[0] = geometry::vertex_codec::half_to_float(static_cast<u16>(s.uvs[v] & 0xFFFFu));
        r.uv[1] = geometry::vertex_codec::half_to_float(static_cast<u16>(s.uvs[v] >> 16));
    }
    return r;
}

/// World-space position of an object-space point (rows . (p, 1); fuse_mr_transform_point).
FUSE_HOST_DEVICE inline void transform_point(const GpuTransform& t, const f32 p[3], f32 out[3]) {
    for (u32 r = 0; r < 3u; ++r) {
        out[r] = t.rows[r][0] * p[0] + t.rows[r][1] * p[1] + t.rows[r][2] * p[2] + t.rows[r][3];
    }
}

/// World-space direction of an object-space vector (rows . v).
FUSE_HOST_DEVICE inline void transform_vector(const GpuTransform& t, const f32 v[3], f32 out[3]) {
    for (u32 r = 0; r < 3u; ++r) {
        out[r] = t.rows[r][0] * v[0] + t.rows[r][1] * v[1] + t.rows[r][2] * v[2];
    }
}

/// Sign of det(rows 3x3) (a . (b x c)).
FUSE_HOST_DEVICE inline f32 det_sign(const GpuTransform& t) {
    const f32* a = t.rows[0];
    const f32* b = t.rows[1];
    const f32* c = t.rows[2];
    const f32 d = a[0] * (b[1] * c[2] - b[2] * c[1]) + a[1] * (b[2] * c[0] - b[0] * c[2]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
    return d < 0.f ? -1.f : 1.f;
}

/// World normal of an object-space normal: cofactor(M) n (= det * M^-T n), sign-corrected so it keeps
/// its orientation under mirroring transforms. Not normalised.
FUSE_HOST_DEVICE inline void transform_normal(const GpuTransform& t, const f32 n[3], f32 out[3]) {
    const f32* a = t.rows[0];
    const f32* b = t.rows[1];
    const f32* c = t.rows[2];
    const f32 sg = det_sign(t);
    const f32 bc[3] = {b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0]};
    const f32 ca[3] = {c[1] * a[2] - c[2] * a[1], c[2] * a[0] - c[0] * a[2], c[0] * a[1] - c[1] * a[0]};
    const f32 ab[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    out[0] = (bc[0] * n[0] + bc[1] * n[1] + bc[2] * n[2]) * sg;
    out[1] = (ca[0] * n[0] + ca[1] * n[1] + ca[2] * n[2]) * sg;
    out[2] = (ab[0] * n[0] + ab[1] * n[1] + ab[2] * n[2]) * sg;
}

/// Barycentrics, their per-pixel derivatives and the depth (fuse_mr_bary).
struct Bary {
    f32 b[3] = {0.f, 0.f, 0.f};
    f32 dbdx[3] = {0.f, 0.f, 0.f};
    f32 dbdy[3] = {0.f, 0.f, 0.f};
    f32 depth = 1.f;
    u32 flags = kAttrEmpty;
};

/// `nx, ny`: the pixel centre in NDC (decode_kernel::pixel_ndc); `sx, sy`: NDC units per pixel
/// (2 / width, 2 / height). The b / depth operations are exactly decode_kernel::decode_clip's.
FUSE_HOST_DEVICE inline Bary bary(const Clip& c0, const Clip& c1, const Clip& c2, f32 nx, f32 ny, f32 sx, f32 sy) {
    Bary r{};
    const f32 u0x = c0.x - nx * c0.w;
    const f32 u0y = c0.y - ny * c0.w;
    const f32 u1x = c1.x - nx * c1.w;
    const f32 u1y = c1.y - ny * c1.w;
    const f32 u2x = c2.x - nx * c2.w;
    const f32 u2y = c2.y - ny * c2.w;
    const f32 e0 = u1x * u2y - u1y * u2x;
    const f32 e1 = u2x * u0y - u2y * u0x;
    const f32 e2 = u0x * u1y - u0y * u1x;
    const f32 s = e0 + e1 + e2;
    if (s == 0.f) {
        r.flags = kAttrDegenerate;
        return r;
    }
    const f32 b0 = e0 / s;
    const f32 b1 = e1 / s;
    const f32 b2 = e2 / s;
    const f32 z = b0 * c0.z + b1 * c1.z + b2 * c2.z;
    const f32 w = b0 * c0.w + b1 * c1.w + b2 * c2.w;
    r.depth = z / w;
    r.b[0] = b0;
    r.b[1] = b1;
    r.b[2] = b2;
    const f32 bx0 = c1.y * c2.w - c1.w * c2.y;
    const f32 bx1 = c2.y * c0.w - c2.w * c0.y;
    const f32 bx2 = c0.y * c1.w - c0.w * c1.y;
    const f32 cy0 = c1.w * c2.x - c1.x * c2.w;
    const f32 cy1 = c2.w * c0.x - c2.x * c0.w;
    const f32 cy2 = c0.w * c1.x - c0.x * c1.w;
    const f32 sbx = bx0 + bx1 + bx2;
    const f32 scy = cy0 + cy1 + cy2;
    r.dbdx[0] = (bx0 - b0 * sbx) / s * sx;
    r.dbdx[1] = (bx1 - b1 * sbx) / s * sx;
    r.dbdx[2] = (bx2 - b2 * sbx) / s * sx;
    r.dbdy[0] = (cy0 - b0 * scy) / s * sy;
    r.dbdy[1] = (cy1 - b1 * scy) / s * sy;
    r.dbdy[2] = (cy2 - b2 * scy) / s * sy;
    r.flags = kAttrOk;
    return r;
}

/// b0 a0 + b1 a1 + b2 a2 (the interpolation order every twin uses).
FUSE_HOST_DEVICE inline f32 interp(const f32 b[3], f32 a0, f32 a1, f32 a2) { return b[0] * a0 + b[1] * a1 + b[2] * a2; }

/// Resolve bin of a material row (fuse_mr_material_bin).
FUSE_HOST_DEVICE inline u32 material_bin(const GpuMaterial& m) {
    if ((m.flags & gpu_scene::kGpuMaterialLayered) != 0u) {
        return kBinLayered;
    }
    if (m.normalTexIdx != kInvalid) {
        return kBinNormalMapped;
    }
    if (m.baseColorTexIdx != kInvalid || m.roughnessTexIdx != kInvalid || m.metallicTexIdx != kInvalid ||
        m.aoTexIdx != kInvalid || m.emissiveTexIdx != kInvalid) {
        return kBinTextured;
    }
    return kBinFlat;
}

struct Params {
    kernel::Span<const u32> vis;                       ///< R32G32 texels: 2 words per pixel, row-major
    kernel::Span<const GpuInstance> instances;         ///< [0, header instance count)
    kernel::Span<const GpuTransform> transforms;
    kernel::Span<const GpuTransform> prevTransforms;
    kernel::Span<const GpuMesh> meshes;
    kernel::Span<const u32> indices;                   ///< scene index buffer
    kernel::Span<const MeshStreams> streams;           ///< per mesh (same order as meshes)
    kernel::Span<const GpuMaterial> materials;         ///< [0, header material count)
    f32 viewProj[16] = {};
    f32 prevViewProj[16] = {};
    u32 width = 0;
    u32 height = 0;
    u32 tilesX = 0;
    u32 tilesY = 0;
    kernel::Span<ResolveAttributeTexel> out; ///< attributes: one per pixel
    kernel::Span<u32> tileBins;              ///< classify: one per tile (row-major tiles)
};

/// Material row of triangle `triangle` (fuse_mr_triangle_material): the instance's base row plus
/// the material index of the submesh whose MTRI range holds the triangle; kInvalid for none.
FUSE_HOST_DEVICE inline u32 triangle_material(const Params& p, const GpuInstance& inst, const GpuMesh& mesh,
                                              const MeshStreams& s, u32 triangle) {
    if (inst.material == kInvalid) {
        return kInvalid;
    }
    u32 local = 0u;
    if (s.submeshes != nullptr && s.meshletTriangleOffsets != nullptr) {
        for (u32 i = 0; i < mesh.submeshCount; ++i) {
            const GpuSubmesh sub = s.submeshes[i];
            if (sub.meshletCount == 0u || sub.meshletOffset >= mesh.meshletCount) {
                continue;
            }
            const u32 first = s.meshletTriangleOffsets[sub.meshletOffset];
            if (triangle >= first && triangle - first < sub.triangleCount) {
                local = sub.materialIndex;
                break;
            }
        }
    }
    const u32 m = inst.material + local;
    return m < p.materials.size ? m : kInvalid;
}

/// Validity checks of the visbuffer decode (decode_kernel::triangle_clip). True: the ids name a
/// drawable triangle.
FUSE_HOST_DEVICE inline bool ids_valid(const Params& p, u32 instance, u32 triangle) {
    if (instance >= p.instances.size) {
        return false;
    }
    const GpuInstance& inst = p.instances[instance];
    if ((inst.flags & gpu_scene::kInstanceValid) == 0u || inst.mesh >= p.meshes.size || inst.mesh >= p.streams.size) {
        return false;
    }
    const GpuMesh& mesh = p.meshes[inst.mesh];
    return mesh.indexCount != 0u && p.indices.size != 0u && triangle < mesh.indexCount / 3u;
}

/// Resolve bin of a visibility sample (fuse_mr_pixel_bin): Empty for no geometry or bad ids, else the
/// bin of the triangle's material (Flat for "no material": the default surface).
FUSE_HOST_DEVICE inline u32 pixel_bin(const Params& p, u32 instance, u32 triangle, u32* materialOut = nullptr) {
    if (materialOut != nullptr) {
        *materialOut = kInvalid;
    }
    if (instance == visbuffer::kVisInvalid || !ids_valid(p, instance, triangle)) {
        return kBinEmpty;
    }
    const GpuInstance& inst = p.instances[instance];
    const u32 m = triangle_material(p, inst, p.meshes[inst.mesh], p.streams[inst.mesh], triangle);
    if (materialOut != nullptr) {
        *materialOut = m;
    }
    return m == kInvalid ? kBinFlat : material_bin(p.materials[m]);
}

/// Everything the resolve reconstructs for one pixel (fuse_mr_attributes).
FUSE_HOST_DEVICE inline ResolveAttributeTexel attributes(const Params& p, u32 x, u32 y, u32 instance, u32 triangle) {
    ResolveAttributeTexel r{};
    if (instance == visbuffer::kVisInvalid) {
        return r;
    }
    r.flags = kAttrBadId;
    if (!ids_valid(p, instance, triangle)) {
        return r;
    }
    const GpuInstance& inst = p.instances[instance];
    const GpuMesh& mesh = p.meshes[inst.mesh];
    const MeshStreams& s = p.streams[inst.mesh];
    const u32 base = mesh.firstIndex + triangle * 3u;
    if (static_cast<u64>(base) + 3u > p.indices.size) {
        return r;
    }
    Vertex v[3];
    Clip c[3];
    Clip pc[3];
    const GpuTransform& t = p.transforms[instance];
    const GpuTransform& pt = p.prevTransforms[instance];
    for (u32 k = 0; k < 3u; ++k) {
        const u32 vi = static_cast<u32>(static_cast<s32>(p.indices[base + k]) + mesh.vertexOffset);
        if (vi >= s.vertexCount) {
            return r;
        }
        v[k] = fetch_vertex(mesh, s, vi);
        c[k] = visbuffer::decode_kernel::clip_position(t, p.viewProj, v[k].position);
        pc[k] = visbuffer::decode_kernel::clip_position(pt, p.prevViewProj, v[k].position);
    }
    r.material = triangle_material(p, inst, mesh, s, triangle);
    r.bin = r.material == kInvalid ? kBinFlat : material_bin(p.materials[r.material]);
    f32 nx = 0.f;
    f32 ny = 0.f;
    visbuffer::decode_kernel::pixel_ndc(x, y, p.width, p.height, nx, ny);
    const f32 sx = 2.f / static_cast<f32>(p.width);
    const f32 sy = 2.f / static_cast<f32>(p.height);
    const Bary b = bary(c[0], c[1], c[2], nx, ny, sx, sy);
    r.flags = b.flags;
    if (b.flags != kAttrOk) {
        return r;
    }
    r.b1 = b.b[1];
    r.b2 = b.b[2];
    r.depth = b.depth;
    r.db1dx = b.dbdx[1];
    r.db1dy = b.dbdy[1];
    r.db2dx = b.dbdx[2];
    r.db2dy = b.dbdy[2];
    for (u32 a = 0; a < 2u; ++a) {
        r.uv[a] = interp(b.b, v[0].uv[a], v[1].uv[a], v[2].uv[a]);
        r.duvdx[a] = interp(b.dbdx, v[0].uv[a], v[1].uv[a], v[2].uv[a]);
        r.duvdy[a] = interp(b.dbdy, v[0].uv[a], v[1].uv[a], v[2].uv[a]);
    }
    f32 n[3][3];
    f32 tg[3][3];
    for (u32 k = 0; k < 3u; ++k) {
        transform_normal(t, v[k].normal, n[k]);
        transform_vector(t, v[k].tangent, tg[k]);
    }
    for (u32 a = 0; a < 3u; ++a) {
        r.normal[a] = interp(b.b, n[0][a], n[1][a], n[2][a]);
        r.tangent[a] = interp(b.b, tg[0][a], tg[1][a], tg[2][a]);
    }
    r.tangentSign = v[0].sign * det_sign(t);
    const f32 px = interp(b.b, pc[0].x, pc[1].x, pc[2].x);
    const f32 py = interp(b.b, pc[0].y, pc[1].y, pc[2].y);
    const f32 pw = interp(b.b, pc[0].w, pc[1].w, pc[2].w);
    if (pw > 0.f) {
        const f32 prevX = (px / pw * 0.5f + 0.5f) * static_cast<f32>(p.width);
        const f32 prevY = (py / pw * 0.5f + 0.5f) * static_cast<f32>(p.height);
        r.velocity[0] = (static_cast<f32>(x) + 0.5f) - prevX;
        r.velocity[1] = (static_cast<f32>(y) + 0.5f) - prevY;
    }
    return r;
}

/// Camera centre of a perspective view-projection (column-major): the point every clip-space x, y and w row maps to
/// zero, i.e. the solution of rows {0, 1, 3} . (C, 1) = 0 by Cramer's rule (fuse_mr_camera_centre). False (and 0)
/// when those rows are singular (an orthographic projection has no finite centre). The layered bin's detail fade uses
/// |P - C| as the view distance.
FUSE_HOST_DEVICE inline bool camera_centre(const f32 m[16], f32 out[3]) {
    const f32 a0[3] = {m[0], m[4], m[8]};
    const f32 a1[3] = {m[1], m[5], m[9]};
    const f32 a2[3] = {m[3], m[7], m[11]};
    const f32 c12[3] = {a1[1] * a2[2] - a1[2] * a2[1], a1[2] * a2[0] - a1[0] * a2[2], a1[0] * a2[1] - a1[1] * a2[0]};
    const f32 c20[3] = {a2[1] * a0[2] - a2[2] * a0[1], a2[2] * a0[0] - a2[0] * a0[2], a2[0] * a0[1] - a2[1] * a0[0]};
    const f32 c01[3] = {a0[1] * a1[2] - a0[2] * a1[1], a0[2] * a1[0] - a0[0] * a1[2], a0[0] * a1[1] - a0[1] * a1[0]};
    const f32 det = a0[0] * c12[0] + a0[1] * c12[1] + a0[2] * c12[2];
    out[0] = out[1] = out[2] = 0.f;
    if (det == 0.f) {
        return false;
    }
    const f32 b0 = m[12];
    const f32 b1 = m[13];
    const f32 b2 = m[15];
    for (u32 k = 0; k < 3u; ++k) {
        out[k] = -(b0 * c12[k] + b1 * c20[k] + b2 * c01[k]) / det;
    }
    return true;
}

/// World position of the pixel's surface point and its per-pixel derivatives (fuse_mr_attributes' position /
/// dPdx / dPdy): the three vertices transformed to world space, interpolated with b, db/dx, db/dy. False when the
/// pixel does not reconstruct (attributes() flags != kAttrOk).
FUSE_HOST_DEVICE inline bool world_position(const Params& p, u32 x, u32 y, u32 instance, u32 triangle, f32 P[3],
                                            f32 dPdx[3], f32 dPdy[3]) {
    if (instance == visbuffer::kVisInvalid || !ids_valid(p, instance, triangle)) {
        return false;
    }
    const GpuInstance& inst = p.instances[instance];
    const GpuMesh& mesh = p.meshes[inst.mesh];
    const MeshStreams& s = p.streams[inst.mesh];
    const u32 base = mesh.firstIndex + triangle * 3u;
    if (static_cast<u64>(base) + 3u > p.indices.size) {
        return false;
    }
    const GpuTransform& t = p.transforms[instance];
    f32 w[3][3];
    Clip c[3];
    for (u32 k = 0; k < 3u; ++k) {
        const u32 vi = static_cast<u32>(static_cast<s32>(p.indices[base + k]) + mesh.vertexOffset);
        if (vi >= s.vertexCount) {
            return false;
        }
        const Vertex v = fetch_vertex(mesh, s, vi);
        c[k] = visbuffer::decode_kernel::clip_position(t, p.viewProj, v.position);
        transform_point(t, v.position, w[k]);
    }
    f32 nx = 0.f;
    f32 ny = 0.f;
    visbuffer::decode_kernel::pixel_ndc(x, y, p.width, p.height, nx, ny);
    const Bary b = bary(c[0], c[1], c[2], nx, ny, 2.f / static_cast<f32>(p.width), 2.f / static_cast<f32>(p.height));
    if (b.flags != kAttrOk) {
        return false;
    }
    for (u32 a = 0; a < 3u; ++a) {
        P[a] = interp(b.b, w[0][a], w[1][a], w[2][a]);
        dPdx[a] = interp(b.dbdx, w[0][a], w[1][a], w[2][a]);
        dPdy[a] = interp(b.dbdy, w[0][a], w[1][a], w[2][a]);
    }
    return true;
}

struct AttributesKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const u32 x = idx.global.x;
        const u32 y = idx.global.y;
        const u32 pixel = y * p.width + x;
        p.out[pixel] = attributes(p, x, y, p.vis[pixel * 2u], p.vis[pixel * 2u + 1u]);
    }
};

/// Bin of tile (tx, ty): the maximum pixel bin over the tile's pixels inside the image.
FUSE_HOST_DEVICE inline u32 tile_bin(const Params& p, u32 tx, u32 ty) {
    u32 bin = kBinEmpty;
    for (u32 j = 0; j < kTileSize; ++j) {
        for (u32 i = 0; i < kTileSize; ++i) {
            const u32 x = tx * kTileSize + i;
            const u32 y = ty * kTileSize + j;
            if (x >= p.width || y >= p.height) {
                continue;
            }
            const u32 pixel = y * p.width + x;
            const u32 b = pixel_bin(p, p.vis[pixel * 2u], p.vis[pixel * 2u + 1u]);
            bin = b > bin ? b : bin;
        }
    }
    return bin;
}

struct ClassifyKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        p.tileBins[idx.global.y * p.tilesX + idx.global.x] = tile_bin(p, idx.global.x, idx.global.y);
    }
};

inline kernel::KernelLaunch make_attributes_launch(u32 width, u32 height) {
    return kernel::KernelLaunch{kAttributesName, kernel::extent2(width, height), {8u, 8u, 1u}};
}

inline kernel::KernelLaunch make_classify_launch(u32 tilesX, u32 tilesY) {
    return kernel::KernelLaunch{kClassifyName, kernel::extent2(tilesX, tilesY), {8u, 8u, 1u}};
}

/// Isotropic texture LOD of a footprint (log2 of the longer UV-derivative axis in texels), the value
/// a conforming implementation selects without anisotropy (Vulkan spec "LOD operation", rho_max).
inline f32 isotropic_lod(const f32 duvdx[2], const f32 duvdy[2], f32 texWidth, f32 texHeight) {
    const f32 ax = duvdx[0] * texWidth;
    const f32 ay = duvdx[1] * texHeight;
    const f32 bx = duvdy[0] * texWidth;
    const f32 by = duvdy[1] * texHeight;
    const f32 rx = std::sqrt(ax * ax + ay * ay);
    const f32 ry = std::sqrt(bx * bx + by * by);
    const f32 rho = rx > ry ? rx : ry;
    return rho > 0.f ? std::log2(rho) : -128.f;
}

} // namespace fuse::renderer::material_resolve::resolve_kernel
