#pragma once

// WP-5.1 per-meshlet cull, CPU reference as a single-source kernel (docs/compute-kernels.md), one
// (task group, lane) = one meshlet per item. shaders/meshlet/meshlet_common.{glsl,slang} are
// line-by-line twins of world_meshlet / classify below (same operations in the same order, `precise`,
// Slang with -fp-mode precise); the GPU task shader is checked against this kernel by
// fuse_rp_meshlet_path.
//
// The decision is made by the two existing reference kernels, on a WORLD-space meshlet record:
//   frustum + cone  geometry::cull_kernel::cull_meshlet (WP-1.2) on world_meshlet()'s record, with
//                   the culler's frustum planes (CullConstants::planes) and the camera position;
//   Hi-Z            culling::cull_kernel::hiz_visible (WP-1.3) on the meshlet's world sphere.
// world_meshlet: sphere = culling::cull_kernel::world_sphere of the meshlet's (center, radius) under
// the instance transform (Gershgorin bound: conservative for any affine transform); cone apex through
// the transform, axis through its linear part A WITHOUT normalisation, cutoff' = cutoff * |A axis|,
// so cull_meshlet's apex test dot(apex - eye, axis') >= cutoff' |apex - eye| is the object-space
// test dot(normalize(apex - eye), axis) >= cutoff for any similarity transform (rotation, uniform
// scale, reflection: A axis is parallel to the transformed normal direction A^-T axis). For a
// non-similar linear part (non-uniform scale, shear) the cone does not transform that way and the
// cone test is skipped (conservative); a cutoff >= 1 ("never cone-cull", meshlet_types.hpp) skips it too.
//
// Cone culling removes back-facing meshlets. The visibility pipelines are two-sided (WP-1.4 open
// issue: no cull flag yet), so the cone test changes the image only where a back face is visible:
// open meshes seen from behind, or a camera inside / clipping a closed mesh. MeshletFrameDesc::cone
// therefore defaults to off; turn it on for closed geometry (the gate checks both).
//
// Phases (MeshletMode):
//   early    (culler phase-1 instances) frustum, cone; then, with occlusion and history, the sphere
//            under LAST frame's transform and view against LAST frame's Hi-Z: visible -> Phase1Drawn,
//            else Deferred. Occlusion off -> Phase1Drawn; no history -> Deferred.
//   late     Deferred only: THIS frame's sphere against the Hi-Z of the phase-1 depth -> Phase2Drawn /
//            Occluded (a meshlet is drawn at most once per frame).
//   phase 2  (culler phase-2 instances) frustum, cone, then THIS frame's Hi-Z (occlusion on) ->
//            Phase2Drawn / Occluded.
// Nothing visible is lost: frustum and Hi-Z are conservative (WP-1.3 argument), cone culling only
// removes back faces. The culler's instance decision precedes all of this.
//
// GPU parity rule (as WP-1.2 / WP-1.3): only correctly rounded adds, multiplies and compares plus
// sqrt and the perspective divides. The reference evaluates every meshlet at a strict (radius and
// cutoff x (1 - e)) and a lenient (x (1 + e)) setting, e = kParityEpsilon; every test is monotone in
// both, so a meshlet decided the same way at both ends is decided identically by any evaluation in
// between, and only the others are excluded from the bit-for-bit comparison (and counted).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/culling/instance_cull_kernel.hpp>
#include <fuse/renderer/geometry/meshlet_cull_kernel.hpp>
#include <fuse/renderer/geometry/meshlet_types.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/meshlet/meshlet_types.hpp>

#include <cmath>

namespace fuse::renderer::meshlet::cull_kernel {

using gpu_scene::GpuInstance;
using gpu_scene::GpuMesh;
using gpu_scene::GpuTransform;

inline constexpr const char* kName = "meshlet_cull";
inline constexpr u32 kWorkgroup = kMeshletTaskGroup;
inline constexpr f32 kParityEpsilon = 1.0e-4f;

FUSE_HOST_DEVICE inline f32 abs_f(f32 v) { return v < 0.f ? -v : v; }
FUSE_HOST_DEVICE inline f32 max_f(f32 a, f32 b) { return a > b ? a : b; }

/// True when the linear part of `t` is a similarity (A^T A = s^2 I within kMeshletSimilarityTolerance
/// of its largest diagonal entry): the cone test is valid under it.
FUSE_HOST_DEVICE inline bool is_similarity(const GpuTransform& t) {
    const f32 g00 = t.rows[0][0] * t.rows[0][0] + t.rows[1][0] * t.rows[1][0] + t.rows[2][0] * t.rows[2][0];
    const f32 g11 = t.rows[0][1] * t.rows[0][1] + t.rows[1][1] * t.rows[1][1] + t.rows[2][1] * t.rows[2][1];
    const f32 g22 = t.rows[0][2] * t.rows[0][2] + t.rows[1][2] * t.rows[1][2] + t.rows[2][2] * t.rows[2][2];
    const f32 d01 = t.rows[0][0] * t.rows[0][1] + t.rows[1][0] * t.rows[1][1] + t.rows[2][0] * t.rows[2][1];
    const f32 d02 = t.rows[0][0] * t.rows[0][2] + t.rows[1][0] * t.rows[1][2] + t.rows[2][0] * t.rows[2][2];
    const f32 d12 = t.rows[0][1] * t.rows[0][2] + t.rows[1][1] * t.rows[1][2] + t.rows[2][1] * t.rows[2][2];
    const f32 gmax = max_f(g00, max_f(g11, g22));
    const f32 tol = gmax * kMeshletSimilarityTolerance;
    return gmax > 0.f && abs_f(g00 - g11) <= tol && abs_f(g00 - g22) <= tol && abs_f(d01) <= tol && abs_f(d02) <= tol &&
           abs_f(d12) <= tol;
}

/// A meshlet in world space: the record cull_meshlet tests, plus the tests that apply to it.
struct WorldMeshlet {
    geometry::MeshletRecord record{};
    u32 tests = 0; ///< geometry::cull_kernel::kTestFrustum | kTestCone
};

/// World-space record of meshlet `m` under instance transform `t` (see the header comment).
/// `radiusScale` / `cutoffScale` are the parity perturbations (1 on the GPU).
FUSE_HOST_DEVICE inline WorldMeshlet world_meshlet(const GpuTransform& t, const geometry::MeshletRecord& m, u32 flags,
                                                   f32 radiusScale, f32 cutoffScale) {
    WorldMeshlet w{};
    GpuMesh bounds{};
    bounds.boundsCenter[0] = m.center[0];
    bounds.boundsCenter[1] = m.center[1];
    bounds.boundsCenter[2] = m.center[2];
    bounds.boundsRadius = m.radius;
    const culling::cull_kernel::Sphere s = culling::cull_kernel::world_sphere(t, bounds, radiusScale);
    for (u32 r = 0; r < 3u; ++r) {
        w.record.center[r] = s.c[r];
    }
    w.record.radius = s.r;
    for (u32 r = 0; r < 3u; ++r) {
        w.record.cone_apex[r] =
            t.rows[r][0] * m.cone_apex[0] + t.rows[r][1] * m.cone_apex[1] + t.rows[r][2] * m.cone_apex[2] + t.rows[r][3];
        w.record.cone_axis[r] = t.rows[r][0] * m.cone_axis[0] + t.rows[r][1] * m.cone_axis[1] + t.rows[r][2] * m.cone_axis[2];
    }
    const f32 len2 = w.record.cone_axis[0] * w.record.cone_axis[0] + w.record.cone_axis[1] * w.record.cone_axis[1] +
                     w.record.cone_axis[2] * w.record.cone_axis[2];
    w.record.cone_cutoff = m.cone_cutoff * std::sqrt(len2) * cutoffScale;
    w.tests = (flags & kMeshletCullFrustum) != 0u ? geometry::cull_kernel::kTestFrustum : 0u;
    if ((flags & kMeshletCullCone) != 0u && m.cone_cutoff < 1.f && is_similarity(t)) {
        w.tests |= geometry::cull_kernel::kTestCone;
    }
    return w;
}

/// Frustum + cone through the WP-1.2 kernel: kMeshletResultNone (passes), FrustumCulled or ConeCulled.
FUSE_HOST_DEVICE inline u32 frustum_cone(const WorldMeshlet& w, const MeshletConstants& c) {
    geometry::cull_kernel::CullView view{};
    for (u32 i = 0; i < 6u; ++i) {
        for (u32 k = 0; k < 4u; ++k) {
            view.planes[i][k] = c.cull.planes[i][k];
        }
    }
    view.camera[0] = c.camera[0];
    view.camera[1] = c.camera[1];
    view.camera[2] = c.camera[2];
    view.flags = w.tests;
    const u32 r = geometry::cull_kernel::cull_meshlet(w.record, view);
    if ((r & geometry::cull_kernel::kCulledFrustum) != 0u) {
        return kMeshletResultFrustumCulled;
    }
    if ((r & geometry::cull_kernel::kCulledCone) != 0u) {
        return kMeshletResultConeCulled;
    }
    return kMeshletResultNone;
}

FUSE_HOST_DEVICE inline bool oversize(const geometry::MeshletRecord& m) {
    return m.vertex_count > kMeshletMaxVertices || m.triangle_count > kMeshletMaxTriangles;
}

FUSE_HOST_DEVICE inline bool occlusion_on(const MeshletConstants& c) {
    return (c.flags & kMeshletCullOcclusion) != 0u && (c.cull.flags & culling::kCullOcclusion) != 0u;
}

/// Early (mode 0) or phase-2 (mode 2) decision. `prevHiz`: last frame's pyramid, `hiz`: this frame's
/// after the phase-1 draws.
template <typename HizFetch>
FUSE_HOST_DEVICE inline u32 classify(u32 mode, const GpuTransform& xf, const GpuTransform& prevXf,
                                     const geometry::MeshletRecord& m, const MeshletConstants& c, const HizFetch& prevHiz,
                                     const HizFetch& hiz, f32 radiusScale, f32 cutoffScale) {
    if (oversize(m)) {
        return kMeshletResultOversize;
    }
    const WorldMeshlet w = world_meshlet(xf, m, c.flags, radiusScale, cutoffScale);
    const u32 fc = frustum_cone(w, c);
    if (fc != kMeshletResultNone) {
        return fc;
    }
    culling::cull_kernel::Sphere s{};
    if (mode == kMeshletModeEarly) {
        if (!occlusion_on(c)) {
            return kMeshletResultPhase1Drawn;
        }
        if ((c.cull.flags & culling::kCullHistoryValid) == 0u) {
            return kMeshletResultDeferred;
        }
        const WorldMeshlet prev = world_meshlet(prevXf, m, 0u, radiusScale, cutoffScale);
        s.c[0] = prev.record.center[0];
        s.c[1] = prev.record.center[1];
        s.c[2] = prev.record.center[2];
        s.r = prev.record.radius;
        return culling::cull_kernel::hiz_visible(c.cull.prevViewProj, s, c.cull, prevHiz) ? kMeshletResultPhase1Drawn
                                                                                          : kMeshletResultDeferred;
    }
    if (!occlusion_on(c)) {
        return kMeshletResultPhase2Drawn;
    }
    s.c[0] = w.record.center[0];
    s.c[1] = w.record.center[1];
    s.c[2] = w.record.center[2];
    s.r = w.record.radius;
    return culling::cull_kernel::hiz_visible(c.cull.viewProj, s, c.cull, hiz) ? kMeshletResultPhase2Drawn
                                                                               : kMeshletResultOccluded;
}

/// Late (mode 1) decision for a Deferred meshlet: this frame's sphere against this frame's Hi-Z.
template <typename HizFetch>
FUSE_HOST_DEVICE inline u32 classify_late(const GpuTransform& xf, const geometry::MeshletRecord& m, const MeshletConstants& c,
                                          const HizFetch& hiz, f32 radiusScale) {
    const WorldMeshlet w = world_meshlet(xf, m, 0u, radiusScale, 1.f);
    culling::cull_kernel::Sphere s{};
    s.c[0] = w.record.center[0];
    s.c[1] = w.record.center[1];
    s.c[2] = w.record.center[2];
    s.r = w.record.radius;
    return culling::cull_kernel::hiz_visible(c.cull.viewProj, s, c.cull, hiz) ? kMeshletResultPhase2Drawn
                                                                               : kMeshletResultOccluded;
}

/// Final result of a meshlet of a region-0 (culler phase 1) or region-1 (phase 2) instance, as the
/// GPU results buffer holds it after the whole frame (early, then late for Deferred meshlets).
template <typename HizFetch>
FUSE_HOST_DEVICE inline u32 classify_frame(u32 region, const GpuTransform& xf, const GpuTransform& prevXf,
                                           const geometry::MeshletRecord& m, const MeshletConstants& c,
                                           const HizFetch& prevHiz, const HizFetch& hiz, f32 radiusScale, f32 cutoffScale) {
    if (region != 0u) {
        return classify(kMeshletModePhase2, xf, prevXf, m, c, prevHiz, hiz, radiusScale, cutoffScale);
    }
    const u32 early = classify(kMeshletModeEarly, xf, prevXf, m, c, prevHiz, hiz, radiusScale, cutoffScale);
    return early == kMeshletResultDeferred ? classify_late(xf, m, c, hiz, radiusScale) : early;
}

/// Kernel over the GPU's task-group records of one region: item = group * kMeshletTaskGroup + lane.
struct Params {
    kernel::Span<const GpuInstance> instances;
    kernel::Span<const GpuTransform> transforms;
    kernel::Span<const GpuTransform> prevTransforms;
    /// Per mesh (GpuInstance::mesh), the mesh's meshlet records.
    kernel::Span<const kernel::Span<const geometry::MeshletRecord>> meshlets;
    kernel::Span<const MeshletGroup> groups;
    const MeshletConstants* constants = nullptr;
    culling::cull_kernel::HizLevels prevHiz{};
    culling::cull_kernel::HizLevels hiz{};
    u32 region = 0;
    f32 radiusScale = 1.f;
    f32 cutoffScale = 1.f;
    kernel::Span<u32> results; ///< one MeshletResult per item
};

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const u32 g = idx.linear / kMeshletTaskGroup;
        const u32 lane = idx.linear % kMeshletTaskGroup;
        const MeshletGroup rec = p.groups[g];
        const GpuInstance& inst = p.instances[rec.instance];
        const kernel::Span<const geometry::MeshletRecord> mesh = p.meshlets[inst.mesh];
        const u32 m = rec.firstMeshlet + lane;
        if (m >= mesh.size) {
            p.results[idx.linear] = kMeshletResultNone;
            return;
        }
        const culling::cull_kernel::HizFetchCpu prev{&p.prevHiz};
        const culling::cull_kernel::HizFetchCpu cur{&p.hiz};
        p.results[idx.linear] = classify_frame(p.region, p.transforms[rec.instance], p.prevTransforms[rec.instance], mesh[m],
                                               *p.constants, prev, cur, p.radiusScale, p.cutoffScale);
    }
};

inline kernel::KernelLaunch make_launch(u32 groupCount) {
    return kernel::KernelLaunch{kName, kernel::extent1(groupCount * kMeshletTaskGroup), {kWorkgroup, 1u, 1u}};
}

} // namespace fuse::renderer::meshlet::cull_kernel
