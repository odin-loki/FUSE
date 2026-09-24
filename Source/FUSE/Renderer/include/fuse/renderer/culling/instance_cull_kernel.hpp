#pragma once

// WP-1.3 instance cull, CPU reference as a single-source kernel (docs/compute-kernels.md), one
// instance per item. shaders/culling/cull_common.{glsl,slang} are line-by-line twins of the
// functions below (same operations in the same order, `precise` so no FMA contraction); the GPU
// kernel instance_cull.{slang,comp} is checked against this one by fuse_rp_culling.
//
// Two-phase occlusion (renderer plan Phase 1):
//   phase 1  every valid, visible instance with a mesh: frustum test with THIS frame's planes, then
//            (occlusion on, history valid) its bounds under LAST frame's transform, projected with
//            LAST frame's view-projection, against LAST frame's Hi-Z. Visible -> Phase1Drawn
//            (draw now), otherwise -> Candidate. Occlusion off: every frustum-visible instance is
//            Phase1Drawn. No history (first frame, camera cut, resize): every frustum-visible
//            instance is a Candidate.
//   (caller draws phase 1, the Hi-Z is rebuilt from that depth)
//   phase 2  candidates only: this frame's bounds and view-projection against the NEW Hi-Z.
//            Visible -> Phase2Drawn (disoccluded, drawn now), otherwise Occluded.
// Phase 2 cannot lose a visible object: the new Hi-Z holds the farthest depth of the phase-1
// geometry, a subset of the final frame, and the test is conservative, so anything with a visible
// pixel in the final frame passes it. Phase 1 only decides how much work phase 2 gets.
//
// Occlusion test (hiz_visible): the world-space AABB of the bounding sphere is projected (8
// corners, x/w, y/w, z/w); any corner with w <= kMinW or z < 0 (crossing the near plane) means
// "visible". The screen rect in mip-0 texels picks the finest level where it spans at most 2x2
// texels (integer search), and the instance is occluded when its nearest depth is strictly
// farther than the max of those 4 texels. Monotone: a larger sphere is never "more occluded".
//
// GPU parity rule (as WP-1.2's meshlet_cull_kernel.hpp): only correctly rounded adds, multiplies,
// compares and floor-by-truncation, plus one sqrt (world radius) and the perspective divides,
// which Vulkan does not round exactly. A decision can therefore differ from the GPU only for an
// instance on a test boundary. The reference evaluates every instance at radius scale 1 - e, 1 and
// 1 + e (e = kParityEpsilon); because every test is monotone in the radius, an instance whose
// decision is the same at both ends is decided identically by any evaluation in between, and only
// the others are excluded from the bit-for-bit comparison (and counted).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/culling/cull_types.hpp>
#include <fuse/renderer/culling/hiz_build_kernel.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>

#include <cmath>

namespace fuse::renderer::culling::cull_kernel {

using gpu_scene::GpuInstance;
using gpu_scene::GpuMesh;
using gpu_scene::GpuTransform;

inline constexpr const char* kName = "culling_instance_cull";
inline constexpr u32 kWorkgroup = kCullWorkgroup;
/// Clip-space w below which a projected corner counts as crossing the camera plane.
inline constexpr f32 kMinW = 1.0e-6f;
inline constexpr f32 kParityEpsilon = 1.0e-4f;

/// CPU view of a Hi-Z pyramid (row-major levels, square, hiz_build_kernel.hpp layout).
struct HizLevels {
    const f32* level[kMaxHizMips] = {};
    u32 dim0 = 0;
    u32 mipCount = 0;

    FUSE_HOST_DEVICE f32 fetch(u32 mip, u32 x, u32 y) const {
        const u32 dim = hiz_kernel::mip_dim(dim0, mip);
        return level[mip][y * dim + x];
    }
};

struct Sphere {
    f32 c[3] = {0.f, 0.f, 0.f};
    f32 r = 0.f;
};

/// World-space bounding sphere: center through the 3x4 transform; radius times a bound on the
/// linear part's largest singular value: sqrt of the Gershgorin bound of A^T A (G_ii + sum |G_ij|,
/// G = column dot products). Exact (the largest column length) for rotation x scale, conservative
/// for any affine transform including shear.
FUSE_HOST_DEVICE inline f32 abs_f(f32 v) { return v < 0.f ? -v : v; }

FUSE_HOST_DEVICE inline Sphere world_sphere(const GpuTransform& t, const GpuMesh& m, f32 radiusScale) {
    Sphere s{};
    const f32* b = m.boundsCenter;
    for (u32 r = 0; r < 3u; ++r) {
        s.c[r] = t.rows[r][0] * b[0] + t.rows[r][1] * b[1] + t.rows[r][2] * b[2] + t.rows[r][3];
    }
    const f32 g00 = t.rows[0][0] * t.rows[0][0] + t.rows[1][0] * t.rows[1][0] + t.rows[2][0] * t.rows[2][0];
    const f32 g11 = t.rows[0][1] * t.rows[0][1] + t.rows[1][1] * t.rows[1][1] + t.rows[2][1] * t.rows[2][1];
    const f32 g22 = t.rows[0][2] * t.rows[0][2] + t.rows[1][2] * t.rows[1][2] + t.rows[2][2] * t.rows[2][2];
    const f32 g01 = abs_f(t.rows[0][0] * t.rows[0][1] + t.rows[1][0] * t.rows[1][1] + t.rows[2][0] * t.rows[2][1]);
    const f32 g02 = abs_f(t.rows[0][0] * t.rows[0][2] + t.rows[1][0] * t.rows[1][2] + t.rows[2][0] * t.rows[2][2]);
    const f32 g12 = abs_f(t.rows[0][1] * t.rows[0][2] + t.rows[1][1] * t.rows[1][2] + t.rows[2][1] * t.rows[2][2]);
    const f32 b0 = g00 + g01 + g02;
    const f32 b1 = g11 + g01 + g12;
    const f32 b2 = g22 + g02 + g12;
    const f32 b12 = b1 > b2 ? b1 : b2;
    const f32 s2 = b0 > b12 ? b0 : b12;
    s.r = m.boundsRadius * std::sqrt(s2) * radiusScale;
    return s;
}

/// Sphere against the 6 inward planes: false when fully outside one of them.
FUSE_HOST_DEVICE inline bool frustum_visible(const CullConstants& c, const Sphere& s) {
    for (u32 i = 0; i < 6u; ++i) {
        const f32* p = c.planes[i];
        const f32 d = p[0] * s.c[0] + p[1] * s.c[1] + p[2] * s.c[2] + p[3];
        if (d < -s.r) {
            return false;
        }
    }
    return true;
}

FUSE_HOST_DEVICE inline f32 clamp01(f32 v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

/// Hi-Z test of a sphere under view-projection `vp` (column-major). True = possibly visible.
template <typename HizFetch>
FUSE_HOST_DEVICE inline bool hiz_visible(const f32* vp, const Sphere& s, const CullConstants& c, const HizFetch& hiz) {
    f32 minX = 1.f, maxX = -1.f, minY = 1.f, maxY = -1.f, minZ = 1.f;
    for (u32 k = 0; k < 8u; ++k) {
        const f32 x = (k & 1u) != 0u ? s.c[0] + s.r : s.c[0] - s.r;
        const f32 y = (k & 2u) != 0u ? s.c[1] + s.r : s.c[1] - s.r;
        const f32 z = (k & 4u) != 0u ? s.c[2] + s.r : s.c[2] - s.r;
        const f32 cw = vp[3] * x + vp[7] * y + vp[11] * z + vp[15];
        if (!(cw > kMinW)) {
            return true; // crosses the camera plane: cannot be tested
        }
        const f32 cx = vp[0] * x + vp[4] * y + vp[8] * z + vp[12];
        const f32 cy = vp[1] * x + vp[5] * y + vp[9] * z + vp[13];
        const f32 cz = vp[2] * x + vp[6] * y + vp[10] * z + vp[14];
        const f32 nx = cx / cw;
        const f32 ny = cy / cw;
        const f32 nz = cz / cw;
        if (k == 0u) {
            minX = nx;
            maxX = nx;
            minY = ny;
            maxY = ny;
            minZ = nz;
        } else {
            minX = nx < minX ? nx : minX;
            maxX = nx > maxX ? nx : maxX;
            minY = ny < minY ? ny : minY;
            maxY = ny > maxY ? ny : maxY;
            minZ = nz < minZ ? nz : minZ;
        }
    }
    if (minZ < 0.f) {
        return true; // in front of the near plane
    }
    const u32 last = c.hizDim - 1u;
    const f32 fx0 = clamp01(minX * 0.5f + 0.5f) * c.hizScale[0];
    const f32 fx1 = clamp01(maxX * 0.5f + 0.5f) * c.hizScale[0];
    const f32 fy0 = clamp01(minY * 0.5f + 0.5f) * c.hizScale[1];
    const f32 fy1 = clamp01(maxY * 0.5f + 0.5f) * c.hizScale[1];
    u32 x0 = static_cast<u32>(fx0);
    u32 x1 = static_cast<u32>(fx1);
    u32 y0 = static_cast<u32>(fy0);
    u32 y1 = static_cast<u32>(fy1);
    x0 = x0 < last ? x0 : last;
    x1 = x1 < last ? x1 : last;
    y0 = y0 < last ? y0 : last;
    y1 = y1 < last ? y1 : last;
    u32 mip = 0u;
    while (mip + 1u < c.hizMipCount && (((x1 >> mip) - (x0 >> mip)) > 1u || ((y1 >> mip) - (y0 >> mip)) > 1u)) {
        ++mip;
    }
    const f32 farDepth = hiz_kernel::max4(hiz(mip, x0 >> mip, y0 >> mip), hiz(mip, x1 >> mip, y0 >> mip),
                                     hiz(mip, x0 >> mip, y1 >> mip), hiz(mip, x1 >> mip, y1 >> mip));
    return !(minZ > farDepth);
}

/// Phase-1 decision for one instance (kResultNone / FrustumCulled / Phase1Drawn / Candidate).
template <typename HizFetch>
FUSE_HOST_DEVICE inline u32 classify_phase1(const GpuInstance& inst, const GpuTransform& xf, const GpuTransform& prevXf,
                                            const GpuMesh& mesh, const CullConstants& c, const HizFetch& prevHiz,
                                            f32 radiusScale) {
    (void)inst;
    const Sphere s = world_sphere(xf, mesh, radiusScale);
    if ((c.flags & kCullFrustum) != 0u && !frustum_visible(c, s)) {
        return kResultFrustumCulled;
    }
    if ((c.flags & kCullOcclusion) == 0u) {
        return kResultPhase1Drawn;
    }
    if ((c.flags & kCullHistoryValid) == 0u) {
        return kResultCandidate;
    }
    const Sphere prev = world_sphere(prevXf, mesh, radiusScale);
    return hiz_visible(c.prevViewProj, prev, c, prevHiz) ? kResultPhase1Drawn : kResultCandidate;
}

/// Phase-2 decision for a candidate (Phase2Drawn / Occluded).
template <typename HizFetch>
FUSE_HOST_DEVICE inline u32 classify_phase2(const GpuTransform& xf, const GpuMesh& mesh, const CullConstants& c,
                                            const HizFetch& hiz, f32 radiusScale) {
    const Sphere s = world_sphere(xf, mesh, radiusScale);
    return hiz_visible(c.viewProj, s, c, hiz) ? kResultPhase2Drawn : kResultOccluded;
}

/// Instance eligibility shared by both kernels: valid + visible flags, not transparent (WP-2.3 draws
/// those in the forward pass) and an existing mesh.
FUSE_HOST_DEVICE inline bool eligible(const GpuInstance& inst, u32 meshCount) {
    constexpr u32 kNeed = gpu_scene::kInstanceValid | gpu_scene::kInstanceVisible;
    return (inst.flags & (kNeed | gpu_scene::kInstanceTransparent)) == kNeed && inst.mesh < meshCount;
}

/// The draw an eligible instance emits: the mesh's draw range in the scene index buffer (WP-1.4,
/// gpu_scene_types.hpp "Index layout"), or the legacy implicit range {3 x triangleCount, 0, 0} for a
/// mesh without one (indexCount 0).
FUSE_HOST_DEVICE inline DrawIndexedIndirectCommand make_draw(const GpuMesh& mesh, u32 slot) {
    DrawIndexedIndirectCommand d{};
    const bool ranged = mesh.indexCount != 0u;
    d.indexCount = gpu_scene::meshDrawIndexCount(mesh);
    d.instanceCount = 1u;
    d.firstIndex = ranged ? mesh.firstIndex : 0u;
    d.vertexOffset = ranged ? mesh.vertexOffset : 0;
    d.firstInstance = slot;
    return d;
}

struct HizFetchCpu {
    const HizLevels* levels = nullptr;
    FUSE_HOST_DEVICE f32 operator()(u32 mip, u32 x, u32 y) const { return levels->fetch(mip, x, y); }
};

struct Params {
    kernel::Span<const GpuInstance> instances;
    kernel::Span<const GpuTransform> transforms;
    kernel::Span<const GpuTransform> prevTransforms;
    kernel::Span<const GpuMesh> meshes;
    const CullConstants* constants = nullptr;
    HizLevels prevHiz{}; ///< last frame's pyramid (phase 1)
    HizLevels hiz{};     ///< this frame's pyramid after the phase-1 draws (phase 2)
    f32 radiusScale = 1.f;
    u32 phase = 1u;
    kernel::Span<u32> results; ///< one CullResult per instance slot; phase 2 updates candidates in place
};

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const u32 i = idx.linear;
        const GpuInstance& inst = p.instances[i];
        if (p.phase == 1u) {
            if (!eligible(inst, p.meshes.size)) {
                p.results[i] = kResultNone;
                return;
            }
            const HizFetchCpu prev{&p.prevHiz};
            p.results[i] = classify_phase1(inst, p.transforms[i], p.prevTransforms[i], p.meshes[inst.mesh], *p.constants,
                                           prev, p.radiusScale);
            return;
        }
        if (p.results[i] != kResultCandidate) {
            return;
        }
        const HizFetchCpu cur{&p.hiz};
        p.results[i] = classify_phase2(p.transforms[i], p.meshes[inst.mesh], *p.constants, cur, p.radiusScale);
    }
};

inline kernel::KernelLaunch make_launch(u32 instanceCount) {
    return kernel::KernelLaunch{kName, kernel::extent1(instanceCount), {kWorkgroup, 1u, 1u}};
}

} // namespace fuse::renderer::culling::cull_kernel
