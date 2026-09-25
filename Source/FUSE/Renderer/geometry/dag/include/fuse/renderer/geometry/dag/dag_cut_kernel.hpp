#pragma once

// WP-5.2 CPU reference LOD cut over a cluster DAG, single-source (docs/compute-kernels.md), one
// cluster per item: the kernel WP-5.3 (streaming / residency) and WP-5.1 (mesh-shader path) GPU
// selection is checked against.
//
// Acceptance of a DagLodBounds b for a view (all in the mesh's object space):
//
//   acceptable(b) = b.error < kDagErrorTerminal
//                && b.error * view.error_scale <= view.threshold * max(|b.center - camera| - b.radius, view.znear)
//
// i.e. the error projected onto the screen from the nearest point of the LOD sphere is at most
// `threshold` pixels (error_scale = 0.5 * viewport_height * P[1][1], see make_dag_view). The test is
// division-free; |.| is one sqrt. A cluster is in the cut when
//
//   acceptable(link.self) && !acceptable(link.parent)
//
// Because every cluster of a group carries bit-identical copies of the group's bounds, all members
// of a group (and all children of a group) decide together; because acceptance is monotone up the
// DAG (parent error >= child error, parent sphere contains child sphere, both with a cook-time
// slack), the selected clusters tile the surface exactly once for every view: no cracks, no holes,
// no double cover (tests: fuse_rp_cluster_dag_cut).
//
// Why the cook adds slack (DagBuildOptions::sphere_slack = 2^-10, error_slack = 2^-16): exact
// nesting is not enough in f32, where |c - cam| - r carries an absolute rounding error of about
// 4 ulp of (distance + radius). The sphere slack keeps (d - r) of a group at least ~2^-10 r below
// its producer's, which covers that error for cameras within ~2000 radii; beyond that (d >> r)
// the relative error of d - r is a few ulp and the 2^-16 error slack covers it. Checked with
// exact-tie views (threshold set to a group's own acceptance boundary) in the cut gate.
//
// GPU parity (WP-5.1 / 5.3): evaluate in this order with `precise`; only sqrt is not correctly
// rounded on Vulkan, so a parity test should skip clusters whose deciding margin
// |error * error_scale - threshold * dist| is below ~1e-5 of the operands.
// Instances: transform the camera into object space; a uniform scale needs no correction (the test
// is scale-invariant). Non-uniform scale is not modelled (the caller must be conservative).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/geometry/dag/cluster_dag_types.hpp>

#include <cmath>

namespace fuse::renderer::geometry::dag::cut_kernel {

inline constexpr const char* kName = "geometry_dag_cut";
inline constexpr u32 kWorkgroup = 64u;

/// Result per cluster.
inline constexpr u32 kNotInCut = 0u;
inline constexpr u32 kInCut = 1u;

struct DagView {
    f32 camera[3] = {0.f, 0.f, 0.f}; ///< object space
    f32 error_scale = 1.f;           ///< 0.5 * viewport_height * P[1][1] (pixels per unit error at distance 1)
    f32 threshold = 1.f;             ///< allowed projected error, pixels (>= 0)
    f32 znear = 0.01f;               ///< > 0; distance floor for spheres around / behind the camera
};

FUSE_HOST_DEVICE inline bool lod_acceptable(const DagLodBounds& b, const DagView& v) {
    if (!(b.error < kDagErrorTerminal)) {
        return false;
    }
    const f32 dx = b.center[0] - v.camera[0];
    const f32 dy = b.center[1] - v.camera[1];
    const f32 dz = b.center[2] - v.camera[2];
    const f32 d = std::sqrt(dx * dx + dy * dy + dz * dz) - b.radius;
    const f32 dist = d > v.znear ? d : v.znear;
    return b.error * v.error_scale <= v.threshold * dist;
}

FUSE_HOST_DEVICE inline u32 cut_cluster(const DagClusterLink& link, const DagView& v) {
    return lod_acceptable(link.self, v) && !lod_acceptable(link.parent, v) ? kInCut : kNotInCut;
}

struct Params {
    kernel::Span<const DagClusterLink> links; ///< one per cluster id
    DagView view{};
    kernel::Span<u32> out; ///< kInCut / kNotInCut per cluster id
};

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        p.out[idx.linear] = cut_cluster(p.links[idx.linear], p.view);
    }
};

inline kernel::KernelLaunch make_launch(u32 cluster_count) {
    return kernel::KernelLaunch{kName, kernel::extent1(cluster_count), {kWorkgroup, 1u, 1u}};
}

/// View for a symmetric perspective projection: `proj_y` = P[1][1] = cot(fov_y / 2).
inline DagView make_dag_view(const f32 camera_object_space[3], f32 proj_y, f32 viewport_height, f32 threshold_pixels,
                             f32 znear) {
    DagView v{};
    for (u32 a = 0; a < 3u; ++a) {
        v.camera[a] = camera_object_space[a];
    }
    v.error_scale = 0.5f * viewport_height * proj_y;
    v.threshold = threshold_pixels;
    v.znear = znear;
    return v;
}

} // namespace fuse::renderer::geometry::dag::cut_kernel
