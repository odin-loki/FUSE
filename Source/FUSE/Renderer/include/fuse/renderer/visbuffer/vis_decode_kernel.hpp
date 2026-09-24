#pragma once

// WP-1.4 visibility decode, CPU reference as a single-source kernel (docs/compute-kernels.md), one
// pixel per item. shaders/visbuffer/vis_common.{glsl,slang} are line-for-line twins of the functions
// below (same operations in the same order; GLSL `precise`, Slang -fp-mode precise); the GPU kernel
// vis_decode.{comp,slang} is checked against this one by fuse_rp_visbuffer.
//
// A visibility sample (instance, triangle) decodes through the GPU scene only:
//   instance -> GpuInstance.mesh -> GpuMesh draw range -> indices[firstIndex + 3t + k] + vertexOffset
//   -> VPOS (exact dequantisation) -> transforms[instance] -> viewProj -> clip positions c0, c1, c2.
// The pixel's perspective-correct barycentrics come from the homogeneous clip positions without a
// per-vertex divide: P = sum(b_i c_i) with sum(b_i) = 1 projects to the pixel centre's NDC (x, y),
// i.e. sum(b_i u_i) = 0 with u_i = c_i.xy - (x, y) c_i.w, so b is the normalised cross product of
// the rows of [u0 u1 u2] (valid for vertices behind the camera). depth = P.z / P.w, which is what
// the rasteriser interpolates (z / w is affine in screen space).
//
// GPU parity: the adds and multiplies are correctly rounded on both sides (no contraction); the
// three divides are not on Vulkan, so GPU and CPU agree to a few ulps (fuse_rp_visbuffer uses a
// relative tolerance). WP-1.5's attribute reconstruction starts from the same barycentrics.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/visbuffer/vis_format.hpp>
#include <fuse/renderer/visbuffer/vis_types.hpp>

namespace fuse::renderer::visbuffer::decode_kernel {

using gpu_scene::GpuInstance;
using gpu_scene::GpuMesh;
using gpu_scene::GpuTransform;

inline constexpr const char* kName = "visbuffer_decode";
inline constexpr u32 kWorkgroupX = 8u;
inline constexpr u32 kWorkgroupY = 8u;

struct Clip {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
    f32 w = 0.f;
};

/// CPU view of one mesh's VPOS stream (u16 x 4 per vertex, the GpuMesh::positions stream).
struct MeshPositions {
    const u16* vpos = nullptr;
    u32 vertexCount = 0;
};

/// Exact dequantisation of vertex `v` (vis_common.glsl fuse_vis_mesh_position).
FUSE_HOST_DEVICE inline void mesh_position(const GpuMesh& m, const u16* vpos, u32 v, f32 out[3]) {
    out[0] = m.quantOffset[0] + static_cast<f32>(vpos[v * 4u + 0u]) * m.quantStep[0];
    out[1] = m.quantOffset[1] + static_cast<f32>(vpos[v * 4u + 1u]) * m.quantStep[1];
    out[2] = m.quantOffset[2] + static_cast<f32>(vpos[v * 4u + 2u]) * m.quantStep[2];
}

/// Object -> clip (vis_common.glsl fuse_vis_clip): world = rows . (p, 1), clip = viewProj * world.
FUSE_HOST_DEVICE inline Clip clip_position(const GpuTransform& t, const f32 viewProj[16], const f32 p[3]) {
    f32 w[3];
    for (u32 r = 0; r < 3u; ++r) {
        w[r] = t.rows[r][0] * p[0] + t.rows[r][1] * p[1] + t.rows[r][2] * p[2] + t.rows[r][3];
    }
    Clip c;
    c.x = viewProj[0] * w[0] + viewProj[4] * w[1] + viewProj[8] * w[2] + viewProj[12];
    c.y = viewProj[1] * w[0] + viewProj[5] * w[1] + viewProj[9] * w[2] + viewProj[13];
    c.z = viewProj[2] * w[0] + viewProj[6] * w[1] + viewProj[10] * w[2] + viewProj[14];
    c.w = viewProj[3] * w[0] + viewProj[7] * w[1] + viewProj[11] * w[2] + viewProj[15];
    return c;
}

/// NDC centre of pixel (x, y) (Vulkan viewport, y down).
FUSE_HOST_DEVICE inline void pixel_ndc(u32 x, u32 y, u32 width, u32 height, f32& nx, f32& ny) {
    nx = (static_cast<f32>(x) + 0.5f) * (2.f / static_cast<f32>(width)) - 1.f;
    ny = (static_cast<f32>(y) + 0.5f) * (2.f / static_cast<f32>(height)) - 1.f;
}

/// Barycentrics + depth of a clip-space triangle at an NDC point (fuse_vis_decode_clip).
FUSE_HOST_DEVICE inline VisDecodeTexel decode_clip(const Clip& c0, const Clip& c1, const Clip& c2, f32 nx, f32 ny) {
    VisDecodeTexel r{};
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
        r.flags = kVisDecodeDegenerate;
        return r;
    }
    const f32 b0 = e0 / s;
    const f32 b1 = e1 / s;
    const f32 b2 = e2 / s;
    const f32 z = b0 * c0.z + b1 * c1.z + b2 * c2.z;
    const f32 w = b0 * c0.w + b1 * c1.w + b2 * c2.w;
    r.depth = z / w;
    r.b1 = b1;
    r.b2 = b2;
    r.flags = kVisDecodeOk;
    return r;
}

struct Params {
    kernel::Span<const u32> vis; ///< R32G32 texels: 2 words per pixel, row-major
    kernel::Span<const GpuInstance> instances; ///< [0, header instance count)
    kernel::Span<const GpuTransform> transforms;
    kernel::Span<const GpuMesh> meshes;
    kernel::Span<const u32> indices;              ///< scene index buffer
    kernel::Span<const MeshPositions> positions;  ///< per mesh (same order as meshes)
    f32 viewProj[16] = {};
    u32 width = 0;
    u32 height = 0;
    kernel::Span<VisDecodeTexel> out; ///< one per pixel
};

/// Clip positions of triangle `triangle` of `instance`; false when an id is out of range
/// (fuse_vis_decode's checks).
FUSE_HOST_DEVICE inline bool triangle_clip(const Params& p, u32 instance, u32 triangle, Clip c[3]) {
    if (instance >= p.instances.size) {
        return false;
    }
    const GpuInstance& inst = p.instances[instance];
    if ((inst.flags & gpu_scene::kInstanceValid) == 0u || inst.mesh >= p.meshes.size) {
        return false;
    }
    const GpuMesh& mesh = p.meshes[inst.mesh];
    if (mesh.indexCount == 0u || p.indices.size == 0u || triangle >= mesh.indexCount / 3u ||
        inst.mesh >= p.positions.size) {
        return false;
    }
    const MeshPositions& pos = p.positions[inst.mesh];
    const u32 base = mesh.firstIndex + triangle * 3u;
    if (static_cast<u64>(base) + 3u > p.indices.size) {
        return false;
    }
    for (u32 k = 0; k < 3u; ++k) {
        const u32 v = static_cast<u32>(static_cast<s32>(p.indices[base + k]) + mesh.vertexOffset);
        if (v >= pos.vertexCount) {
            return false;
        }
        f32 obj[3];
        mesh_position(mesh, pos.vpos, v, obj);
        c[k] = clip_position(p.transforms[instance], p.viewProj, obj);
    }
    return true;
}

FUSE_HOST_DEVICE inline VisDecodeTexel decode_sample(const Params& p, u32 instance, u32 triangle, f32 nx, f32 ny) {
    VisDecodeTexel r{};
    if (instance == kVisInvalid) {
        return r;
    }
    Clip c[3];
    if (!triangle_clip(p, instance, triangle, c)) {
        r.flags = kVisDecodeBadId;
        return r;
    }
    return decode_clip(c[0], c[1], c[2], nx, ny);
}

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const u32 x = idx.global.x;
        const u32 y = idx.global.y;
        const u32 pixel = y * p.width + x;
        f32 nx = 0.f;
        f32 ny = 0.f;
        pixel_ndc(x, y, p.width, p.height, nx, ny);
        p.out[pixel] = decode_sample(p, p.vis[pixel * 2u], p.vis[pixel * 2u + 1u], nx, ny);
    }
};

inline kernel::KernelLaunch make_launch(u32 width, u32 height) {
    return kernel::KernelLaunch{kName, kernel::extent2(width, height), {kWorkgroupX, kWorkgroupY, 1u}};
}

} // namespace fuse::renderer::visbuffer::decode_kernel
