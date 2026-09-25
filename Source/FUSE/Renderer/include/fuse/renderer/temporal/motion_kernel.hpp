#pragma once

// WP-4.1 GPU motion vectors, CPU reference as a single-source kernel "temporal_motion" (docs/compute-kernels.md),
// one render pixel per item. shaders/temporal/tm_motion.{comp,slang} (+ tm_common.{glsl,slang}) are
// line-for-line twins (same operations in the same order; GLSL `precise`, Slang -fp-mode precise);
// fuse_rp_temporal checks the GPU buffers against this kernel. Conventions (jitter, UV motion, sky, depth):
// temporal_types.hpp.
//
// Per pixel: visibility sample (WP-1.4, R32G32 (instance, triangle)) -> instance -> mesh -> scene index buffer
// -> VPOS (exact dequantisation) -> clip positions under drawViewProj (the matrix the visibility buffer was
// drawn with) -> perspective-correct barycentrics at the pixel centre (the WP-1.4 decode's operations) ->
// object-space point P = b0 v0 + b1 v1 + b2 v2 -> current / previous clip under the UNJITTERED view-projections
// with the WP-1.1 current / previous transforms of the instance -> UV motion and linear depth.
//
// Why the object-space point: rigid motion is exact whatever the transform (rotation, scale, mirroring), and
// the jitter only decides WHICH surface point the pixel sees, never its motion.
//
// Skinned meshes: the GPU scene / visibility buffer draw static VPOS streams only and the renderer's
// BoneBuffer keeps no previous palette, so there is no skinned path to take motion from (execution doc
// WP-4.1 row, open issue). A skinned path needs per-vertex previous positions (or the previous palette);
// P would then be interpolated from prev-skinned positions exactly like the current ones here.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/temporal/temporal_types.hpp>
#include <fuse/renderer/visbuffer/vis_decode_kernel.hpp>
#include <fuse/renderer/visbuffer/vis_format.hpp>

namespace fuse::renderer::temporal::motion_kernel {

using gpu_scene::GpuInstance;
using gpu_scene::GpuMesh;
using gpu_scene::GpuTransform;
using visbuffer::decode_kernel::Clip;
using visbuffer::decode_kernel::MeshPositions;

inline constexpr const char* kName = "temporal_motion";

/// Status of one pixel (the GPU kernel does not write it; the CPU gates use it).
enum MotionStatus : u32 {
    kStatusSky = 0u,        ///< no geometry: rotation-only sky motion (or 0 without kMotionSkyValid)
    kStatusOk = 1u,
    kStatusBadId = 2u,      ///< ids out of range: zero motion, depth 0
    kStatusDegenerate = 3u, ///< zero-area triangle as seen from the pixel
    kStatusBehind = 4u,     ///< the point is behind the current or previous camera: zero motion
};

struct Params {
    kernel::Span<const u32> vis; ///< R32G32 texels: 2 words per pixel, row-major
    kernel::Span<const GpuInstance> instances;
    kernel::Span<const GpuTransform> transforms;
    kernel::Span<const GpuTransform> prevTransforms;
    kernel::Span<const GpuMesh> meshes;
    kernel::Span<const u32> indices;             ///< scene index buffer
    kernel::Span<const MeshPositions> positions; ///< per mesh (same order as meshes)
    MotionFrameConstants frame{};                ///< addresses / handles unused on the CPU
    kernel::Span<math::Vec2> motion;             ///< out: UV motion per pixel
    kernel::Span<f32> depth;                     ///< out: linear view depth per pixel (0 = sky)
    kernel::Span<u32> status;                    ///< optional out: MotionStatus per pixel
};

struct Result {
    f32 motion[2] = {0.f, 0.f};
    f32 depth = 0.f;
    u32 status = kStatusSky;
};

/// Clip position (x, y, w) of an object-space point: world = rows . (p, 1), clip = m * world
/// (visbuffer::decode_kernel::clip_position without z).
FUSE_HOST_DEVICE inline Clip clip_of(const GpuTransform& t, const f32 m[16], const f32 p[3]) {
    return visbuffer::decode_kernel::clip_position(t, m, p);
}

/// UV of a clip position (Vulkan NDC y down, uv (0, 0) top-left): ndc * 0.5 + 0.5.
FUSE_HOST_DEVICE inline void clip_uv(const Clip& c, f32& u, f32& v) {
    u = c.x / c.w * 0.5f + 0.5f;
    v = c.y / c.w * 0.5f + 0.5f;
}

/// Sky motion of pixel (x, y) (fuse_tm_sky_motion).
FUSE_HOST_DEVICE inline Result sky(const MotionFrameConstants& f, u32 x, u32 y) {
    Result r{};
    if ((f.flags & kMotionSkyValid) == 0u) {
        return r;
    }
    const f32 sx = static_cast<f32>(x) + 0.5f + f.jitterX;
    const f32 sy = static_cast<f32>(y) + 0.5f + f.jitterY;
    const f32 nx = sx * (2.f / static_cast<f32>(f.width)) - 1.f;
    const f32 ny = sy * (2.f / static_cast<f32>(f.height)) - 1.f;
    const f32* h = f.skyReproj;
    const f32 px = h[0] * nx + h[4] * ny + h[8];
    const f32 py = h[1] * nx + h[5] * ny + h[9];
    const f32 pw = h[2] * nx + h[6] * ny + h[10];
    if (pw > 0.f) {
        const f32 cu = sx / static_cast<f32>(f.width);
        const f32 cv = sy / static_cast<f32>(f.height);
        r.motion[0] = cu - (px / pw * 0.5f + 0.5f);
        r.motion[1] = cv - (py / pw * 0.5f + 0.5f);
    }
    return r;
}

/// Motion + depth of the visibility sample (instance, triangle) at pixel (x, y) (fuse_tm_motion).
FUSE_HOST_DEVICE inline Result pixel(const Params& p, u32 x, u32 y, u32 instance, u32 triangle) {
    const MotionFrameConstants& f = p.frame;
    if (instance == visbuffer::kVisInvalid) {
        return sky(f, x, y);
    }
    Result r{};
    r.status = kStatusBadId;
    if (instance >= p.instances.size) {
        return r;
    }
    const GpuInstance& inst = p.instances[instance];
    if ((inst.flags & gpu_scene::kInstanceValid) == 0u || inst.mesh >= p.meshes.size || inst.mesh >= p.positions.size) {
        return r;
    }
    const GpuMesh& mesh = p.meshes[inst.mesh];
    if (mesh.indexCount == 0u || p.indices.size == 0u || triangle >= mesh.indexCount / 3u) {
        return r;
    }
    const u32 base = mesh.firstIndex + triangle * 3u;
    if (static_cast<u64>(base) + 3u > p.indices.size) {
        return r;
    }
    const MeshPositions& pos = p.positions[inst.mesh];
    const GpuTransform& t = p.transforms[instance];
    f32 v[3][3];
    Clip c[3];
    for (u32 k = 0; k < 3u; ++k) {
        const u32 vi = static_cast<u32>(static_cast<s32>(p.indices[base + k]) + mesh.vertexOffset);
        if (vi >= pos.vertexCount) {
            return r;
        }
        visbuffer::decode_kernel::mesh_position(mesh, pos.vpos, vi, v[k]);
        c[k] = clip_of(t, f.drawViewProj, v[k]);
    }
    f32 nx = 0.f;
    f32 ny = 0.f;
    visbuffer::decode_kernel::pixel_ndc(x, y, f.width, f.height, nx, ny);
    const f32 u0x = c[0].x - nx * c[0].w;
    const f32 u0y = c[0].y - ny * c[0].w;
    const f32 u1x = c[1].x - nx * c[1].w;
    const f32 u1y = c[1].y - ny * c[1].w;
    const f32 u2x = c[2].x - nx * c[2].w;
    const f32 u2y = c[2].y - ny * c[2].w;
    const f32 e0 = u1x * u2y - u1y * u2x;
    const f32 e1 = u2x * u0y - u2y * u0x;
    const f32 e2 = u0x * u1y - u0y * u1x;
    const f32 s = e0 + e1 + e2;
    if (s == 0.f) {
        r.status = kStatusDegenerate;
        return r;
    }
    const f32 b0 = e0 / s;
    const f32 b1 = e1 / s;
    const f32 b2 = e2 / s;
    f32 obj[3];
    for (u32 a = 0; a < 3u; ++a) {
        obj[a] = b0 * v[0][a] + b1 * v[1][a] + b2 * v[2][a];
    }
    const Clip cc = clip_of(t, f.viewProj, obj);
    const Clip pc = clip_of(p.prevTransforms[instance], f.prevViewProj, obj);
    r.status = kStatusBehind;
    if (cc.w > 0.f) {
        r.depth = cc.w;
        if (pc.w > 0.f) {
            f32 cu = 0.f;
            f32 cv = 0.f;
            f32 pu = 0.f;
            f32 pv = 0.f;
            clip_uv(cc, cu, cv);
            clip_uv(pc, pu, pv);
            r.motion[0] = cu - pu;
            r.motion[1] = cv - pv;
            r.status = kStatusOk;
        }
    }
    return r;
}

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const u32 x = idx.global.x;
        const u32 y = idx.global.y;
        const u32 i = y * p.frame.width + x;
        const Result r = pixel(p, x, y, p.vis[i * 2u], p.vis[i * 2u + 1u]);
        p.motion[i] = math::Vec2{r.motion[0], r.motion[1]};
        p.depth[i] = r.depth;
        if (!p.status.empty()) {
            p.status[i] = r.status;
        }
    }
};

inline kernel::KernelLaunch make_launch(u32 width, u32 height) {
    return kernel::KernelLaunch{kName, kernel::extent2(width, height), {kTileSize, kTileSize, 1u}};
}

} // namespace fuse::renderer::temporal::motion_kernel
