#pragma once

// WP-1.2 per-meshlet bounds finalisation, single-source (docs/compute-kernels.md), one meshlet per item.
//
// The builder first takes meshoptimizer's sphere and normal cone (computed on the *decoded*
// positions), then this kernel makes the volumes exact for what the GPU rasterises:
//   * AABB of the meshlet's decoded vertices;
//   * the sphere radius grows (never shrinks) until it contains every decoded vertex, evaluated in
//     f64 and rounded up to the next f32 when the f32 square falls short, so `dist <= radius` holds
//     for every vertex in exact arithmetic.
// The cone is left as meshoptimizer produced it (its apex/cutoff are conservative by construction).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/geometry/meshlet_types.hpp>

#include <cmath>

namespace fuse::renderer::geometry::bounds_kernel {

inline constexpr const char* kName = "geometry_meshlet_bounds";
inline constexpr u32 kWorkgroup = 64u;

struct Params {
    kernel::Span<MeshletRecord> meshlets;      ///< in: ranges + sphere/cone, out: + AABB, radius
    kernel::Span<const u32> meshlet_vertices;  ///< meshlet-local -> mesh vertex
    kernel::Span<const f32> positions;         ///< decoded xyz per mesh vertex
};

FUSE_HOST_DEVICE inline void finalize_bounds(const Params& p, u32 m) {
    MeshletRecord& r = p.meshlets[m];
    f64 maxDist2 = 0.0;
    for (u32 a = 0; a < 3u; ++a) {
        r.aabb_min[a] = 0.f;
        r.aabb_max[a] = 0.f;
    }
    for (u32 i = 0; i < r.vertex_count; ++i) {
        const u32 v = p.meshlet_vertices[r.vertex_offset + i];
        f64 d2 = 0.0;
        for (u32 a = 0; a < 3u; ++a) {
            const f32 x = p.positions[v * 3u + a];
            if (i == 0u || x < r.aabb_min[a]) {
                r.aabb_min[a] = x;
            }
            if (i == 0u || x > r.aabb_max[a]) {
                r.aabb_max[a] = x;
            }
            const f64 d = static_cast<f64>(x) - static_cast<f64>(r.center[a]);
            d2 += d * d;
        }
        maxDist2 = d2 > maxDist2 ? d2 : maxDist2;
    }
    if (static_cast<f64>(r.radius) * static_cast<f64>(r.radius) < maxDist2) {
        f32 radius = static_cast<f32>(std::sqrt(maxDist2));
        while (static_cast<f64>(radius) * static_cast<f64>(radius) < maxDist2) {
            radius = std::nextafter(radius, 3.0e38f);
        }
        r.radius = radius;
    }
}

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const { finalize_bounds(p, idx.linear); }
};

inline kernel::KernelLaunch make_launch(u32 meshlet_count) {
    return kernel::KernelLaunch{kName, kernel::extent1(meshlet_count), {kWorkgroup, 1u, 1u}};
}

} // namespace fuse::renderer::geometry::bounds_kernel
