#pragma once

// Single-source clustered light culling + deferred shading (docs/compute-kernels.md). Every function
// here is FUSE_HOST_DEVICE and is the ONLY implementation of the cluster slice mapping, the cluster
// AABB build, the sphere/AABB cull and the point-light shade: cluster_math / ClusterSliceLayout /
// ClusterGridLayout (src/lighting/clustered_light_culler.cpp), clustered_shading
// (src/lighting/clustered_shading.cpp) and the CUDA wrapper (kernels/clustered_lighting.cu) all run it.
//
// Launches (item kernels; names are the render-graph pass names where one exists):
//
//   kBuildName   "clustered_cluster_build"  one item per cluster: tight view-space AABB of the cell.
//   kBoundsName  "clustered_light_bounds"   one item per light: view-space sphere + reachable slices.
//   kBinName     "clustered_light_bin"      one item per depth slice: the lights whose slice window covers
//                                           it, in ascending index (fixed capacity = light count).
//   kCullName    "clustered_light_cull"     one item per cluster: walks its slice's light list and writes a
//                                           fixed-capacity list (no atomics). Membership = slice window &&
//                                           exact sphere/AABB test, the same rule the light-major reference
//                                           used, so lists are identical (ascending light index).
//   kCompactName "clustered_light_compact"  one item per cluster: copies its fixed-capacity list to the
//                                           flat light list at the offset of a host exclusive scan
//                                           (deterministic count -> scan -> write).
//   kShadeName   "deferred_shading"         workgroup kernel, one pixel per thread (8x8 tiles): reconstruct,
//                                           look up the cluster, sum point lights in list order; per-tile
//                                           counters reduce in scratch (one global add per tile).

#include <fuse/compute_kernel/atomics.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::clustered_kernel {

inline constexpr const char* kBuildName = "clustered_cluster_build";
inline constexpr const char* kBoundsName = "clustered_light_bounds";
inline constexpr const char* kBinName = "clustered_light_bin";
inline constexpr const char* kCullName = "clustered_light_cull";
inline constexpr const char* kCompactName = "clustered_light_compact";
inline constexpr const char* kShadeName = "deferred_shading";

inline constexpr kernel::Dim3 kLinearWorkgroup{64u, 1u, 1u};
inline constexpr kernel::Dim3 kShadeWorkgroup{8u, 8u, 1u};

/// POD cluster grid (ClusterDesc without member functions).
struct GridDims {
    u32 tiles_x = 0;
    u32 tiles_y = 0;
    u32 slices_z = 0;
    u32 max_lights = 0; ///< Per-cluster cap (0 = unlimited).

    FUSE_HOST_DEVICE u32 cluster_count() const { return tiles_x * tiles_y * slices_z; }
};

/// POD camera with the view basis and projection tangents resolved once on the host.
struct CameraView {
    math::Vec3 position{};
    math::Vec3 right{1.f, 0.f, 0.f};
    math::Vec3 up{0.f, 1.f, 0.f};
    math::Vec3 back{0.f, 0.f, 1.f};
    f32 near_plane = 0.1f;
    f32 far_plane = 1000.f;
    f32 tan_x = 1.f;
    f32 tan_y = 1.f;
    bool reversed_z = true;
};

// ---------------------------------------------------------------------------------------------
// Camera / slice math
// ---------------------------------------------------------------------------------------------

FUSE_HOST_DEVICE inline f32 clamp01(f32 value) {
    return std::clamp(value, 0.f, 1.f);
}

/// Orthonormal view basis (right, up, back) from a forward direction and an up hint.
FUSE_HOST_DEVICE inline void view_basis(const math::Vec3& forward_in,
                                        const math::Vec3& up_hint,
                                        math::Vec3& out_right,
                                        math::Vec3& out_up,
                                        math::Vec3& out_back) {
    math::Vec3 forward = forward_in.normalized();
    if (forward.dot(forward) == 0.f) {
        forward = {0.f, 0.f, -1.f};
    }
    math::Vec3 right = math::cross(forward, up_hint).normalized();
    if (right.dot(right) == 0.f) {
        // Up hint parallel to forward: pick any perpendicular axis.
        const math::Vec3 fallback = std::fabs(forward.y) < 0.99f ? math::Vec3{0.f, 1.f, 0.f} : math::Vec3{1.f, 0.f, 0.f};
        right = math::cross(forward, fallback).normalized();
    }
    out_right = right;
    out_up = math::cross(right, forward);
    out_back = forward * -1.f;
}

FUSE_HOST_DEVICE inline math::Vec3 world_to_view(const CameraView& c, const math::Vec3& world) {
    const math::Vec3 rel = world - c.position;
    return {rel.dot(c.right), rel.dot(c.up), rel.dot(c.back)};
}

FUSE_HOST_DEVICE inline math::Vec3 view_to_world(const CameraView& c, const math::Vec3& view) {
    return c.position + c.right * view.x + c.up * view.y + c.back * view.z;
}

/// Positive linear view depth from a device depth value; 0 for the cleared / sky value.
FUSE_HOST_DEVICE inline f32 view_depth_from_device_depth(f32 device_depth, f32 near_plane, f32 far_plane, bool reversed_z) {
    if (reversed_z) {
        // Infinite-far reversed-Z: ndc = near / d; 0 is the cleared (sky) value.
        return device_depth > 0.f ? near_plane / device_depth : 0.f;
    }
    // Standard [0,1]: ndc = far (d - near) / (d (far - near)); 1 is the cleared value.
    if (device_depth >= 1.f || far_plane <= near_plane) {
        return 0.f;
    }
    const f32 n = near_plane;
    const f32 f = far_plane;
    return (n * f) / (f - device_depth * (f - n));
}

/// View-space position for normalized screen coords (0,0 = top-left) at a linear view depth.
FUSE_HOST_DEVICE inline math::Vec3 view_position_from_screen(f32 screen_x, f32 screen_y, f32 view_depth, f32 tan_x, f32 tan_y) {
    const f32 ndc_x = screen_x * 2.f - 1.f;
    const f32 ndc_y = 1.f - screen_y * 2.f;
    return {ndc_x * tan_x * view_depth, ndc_y * tan_y * view_depth, -view_depth};
}

FUSE_HOST_DEVICE inline f32 slice_near_z(u32 slice, u32 slices_z, f32 near_plane, f32 far_plane) {
    if (slice >= slices_z) {
        return far_plane;
    }
    const f32 depth_ratio = far_plane / near_plane;
    const f32 t0 = static_cast<f32>(slice) / static_cast<f32>(slices_z);
    return near_plane * std::pow(depth_ratio, t0);
}

FUSE_HOST_DEVICE inline f32 slice_far_z(u32 slice, u32 slices_z, f32 near_plane, f32 far_plane) {
    if (slice >= slices_z) {
        return far_plane;
    }
    const f32 depth_ratio = far_plane / near_plane;
    const f32 t1 = static_cast<f32>(slice + 1u) / static_cast<f32>(slices_z);
    return near_plane * std::pow(depth_ratio, t1);
}

FUSE_HOST_DEVICE inline u32 clamp_index(u32 value, u32 extent) {
    return extent == 0u ? 0u : std::min(value, extent - 1u);
}

/// Exponential depth slice of a linear view depth (clamped to the grid).
FUSE_HOST_DEVICE inline u32 slice_from_depth(f32 view_depth, u32 slices_z, f32 near_plane, f32 far_plane) {
    if (slices_z == 0u || near_plane <= 0.f || far_plane <= near_plane) {
        return 0u;
    }
    const f32 clamped = std::clamp(view_depth, near_plane, far_plane);
    const f32 depth_ratio = far_plane / near_plane;
    const f32 log_depth = std::log(clamped / near_plane) / std::log(depth_ratio);
    const u32 slice = static_cast<u32>(log_depth * static_cast<f32>(slices_z));
    return clamp_index(slice, slices_z);
}

/// Flat cluster index (slice fastest): (tileY * tilesX + tileX) * slicesZ + sliceZ.
FUSE_HOST_DEVICE inline u32 cluster_index(u32 tile_x, u32 tile_y, u32 slice_z, u32 tiles_x, u32 slices_z) {
    return (tile_y * tiles_x + tile_x) * slices_z + slice_z;
}

/// Screen coords + linear view depth -> clamped cluster index; false outside [near, far] or empty grid.
FUSE_HOST_DEVICE inline bool map_screen_depth_to_cluster(f32 screen_x,
                                                         f32 screen_y,
                                                         f32 view_depth,
                                                         const GridDims& g,
                                                         f32 near_plane,
                                                         f32 far_plane,
                                                         u32& out_cluster) {
    if (g.tiles_x == 0u || g.tiles_y == 0u || g.slices_z == 0u) {
        return false;
    }
    if (view_depth < near_plane || view_depth > far_plane) {
        return false;
    }
    const u32 tile_x = static_cast<u32>(clamp01(screen_x) * static_cast<f32>(g.tiles_x));
    const u32 tile_y = static_cast<u32>(clamp01(screen_y) * static_cast<f32>(g.tiles_y));
    const u32 slice_z = slice_from_depth(view_depth, g.slices_z, near_plane, far_plane);
    out_cluster = cluster_index(clamp_index(tile_x, g.tiles_x), clamp_index(tile_y, g.tiles_y),
                                clamp_index(slice_z, g.slices_z), g.tiles_x, g.slices_z);
    return true;
}

/// Tight view-space AABB of one frustum cell (tile x/y, exponential depth slice z).
FUSE_HOST_DEVICE inline ClusterAABB build_cluster_aabb(u32 tile_x,
                                                       u32 tile_y,
                                                       u32 slice_z,
                                                       const GridDims& g,
                                                       f32 near_plane,
                                                       f32 far_plane,
                                                       f32 tan_x,
                                                       f32 tan_y) {
    ClusterAABB aabb{};
    if (g.tiles_x == 0u || g.tiles_y == 0u || g.slices_z == 0u) {
        return aabb;
    }
    const f32 depths[2] = {slice_near_z(slice_z, g.slices_z, near_plane, far_plane),
                           slice_far_z(slice_z, g.slices_z, near_plane, far_plane)};
    // Tile 0 is the top row (screenY = 0 -> ndc y = +1), matching map_screen_depth_to_cluster.
    const f32 ndc_x[2] = {(static_cast<f32>(tile_x) / static_cast<f32>(g.tiles_x)) * 2.f - 1.f,
                          (static_cast<f32>(tile_x + 1u) / static_cast<f32>(g.tiles_x)) * 2.f - 1.f};
    const f32 ndc_y[2] = {1.f - (static_cast<f32>(tile_y + 1u) / static_cast<f32>(g.tiles_y)) * 2.f,
                          1.f - (static_cast<f32>(tile_y) / static_cast<f32>(g.tiles_y)) * 2.f};

    // The cell is the convex hull of its 8 corners, so their bounds are the tight AABB.
    bool first = true;
    for (u32 d = 0; d < 2u; ++d) {
        for (u32 ix = 0; ix < 2u; ++ix) {
            for (u32 iy = 0; iy < 2u; ++iy) {
                const f32 depth = depths[d];
                const math::Vec3 corner{ndc_x[ix] * tan_x * depth, ndc_y[iy] * tan_y * depth, -depth};
                if (first) {
                    aabb.minP = corner;
                    aabb.maxP = corner;
                    first = false;
                    continue;
                }
                aabb.minP = {std::min(aabb.minP.x, corner.x), std::min(aabb.minP.y, corner.y),
                             std::min(aabb.minP.z, corner.z)};
                aabb.maxP = {std::max(aabb.maxP.x, corner.x), std::max(aabb.maxP.y, corner.y),
                             std::max(aabb.maxP.z, corner.z)};
            }
        }
    }
    return aabb;
}

/// Closed sphere-vs-AABB overlap (touching counts); false for negative or non-finite radius.
FUSE_HOST_DEVICE inline bool sphere_intersects_aabb(const math::Vec3& center, f32 radius, const ClusterAABB& aabb) {
    if (!(radius >= 0.f) || !std::isfinite(radius)) {
        return false;
    }
    const f32 closest_x = std::clamp(center.x, aabb.minP.x, aabb.maxP.x);
    const f32 closest_y = std::clamp(center.y, aabb.minP.y, aabb.maxP.y);
    const f32 closest_z = std::clamp(center.z, aabb.minP.z, aabb.maxP.z);
    const f32 dx = center.x - closest_x;
    const f32 dy = center.y - closest_y;
    const f32 dz = center.z - closest_z;
    return (dx * dx + dy * dy + dz * dz) <= (radius * radius);
}

// ---------------------------------------------------------------------------------------------
// Point-light shading
// ---------------------------------------------------------------------------------------------

/// Windowed inverse-square falloff: saturate(1 - (d/r)^4)^2 / max(d^2, 1e-4); 0 for d >= r or r <= 0.
FUSE_HOST_DEVICE inline f32 point_light_falloff(f32 distance, f32 radius) {
    if (!(radius > 0.f) || !(distance < radius)) {
        return 0.f;
    }
    const f32 ratio = distance / radius;
    const f32 ratio2 = ratio * ratio;
    const f32 window = std::clamp(1.f - ratio2 * ratio2, 0.f, 1.f);
    return (window * window) / std::max(distance * distance, 1e-4f);
}

/// Lambert diffuse radiance from one point light (colour * intensity * falloff * N.L * albedo).
FUSE_HOST_DEVICE inline math::Vec3 point_light_contribution(const PointLightInput& light,
                                                            const math::Vec3& world_pos,
                                                            const math::Vec3& normal,
                                                            const math::Vec3& albedo) {
    const math::Vec3 to_light = light.position - world_pos;
    const f32 distance = to_light.length();
    const f32 falloff = point_light_falloff(distance, light.radius);
    if (falloff == 0.f) {
        return {};
    }
    const f32 n_dot_l = std::max(normal.dot(to_light * (1.f / std::max(distance, 1e-6f))), 0.f);
    const f32 scale = light.intensity * falloff * n_dot_l;
    return {light.color.x * albedo.x * scale, light.color.y * albedo.y * scale, light.color.z * albedo.z * scale};
}

/// Brute force: every point light in ascending index order; `evaluations` += light count.
FUSE_HOST_DEVICE inline math::Vec3 shade_all_lights(kernel::Span<const PointLightInput> lights,
                                                    const math::Vec3& world_pos,
                                                    const math::Vec3& normal,
                                                    const math::Vec3& albedo,
                                                    u32& evaluations) {
    math::Vec3 radiance{};
    for (u32 light = 0; light < lights.size; ++light) {
        radiance = radiance + point_light_contribution(lights[light], world_pos, normal, albedo);
    }
    evaluations += lights.size;
    return radiance;
}

/// The point lights of one cluster's rebuilt list, in list (ascending index) order.
FUSE_HOST_DEVICE inline math::Vec3 shade_cluster_lights(kernel::Span<const ClusterGridEntry> grid,
                                                        kernel::Span<const u32> light_list,
                                                        u32 cluster,
                                                        kernel::Span<const PointLightInput> lights,
                                                        const math::Vec3& world_pos,
                                                        const math::Vec3& normal,
                                                        const math::Vec3& albedo,
                                                        u32& evaluations) {
    math::Vec3 radiance{};
    if (cluster >= grid.size) {
        return radiance;
    }
    const ClusterGridEntry entry = grid[cluster];
    const u32 end = std::min(entry.offset + entry.count, light_list.size);
    for (u32 i = entry.offset; i < end; ++i) {
        const u32 light = light_list[i];
        if (light >= lights.size) {
            continue; // Spot light (encoded after point lights) — not handled by this pass.
        }
        radiance = radiance + point_light_contribution(lights[light], world_pos, normal, albedo);
        ++evaluations;
    }
    return radiance;
}

// ---------------------------------------------------------------------------------------------
// Cluster build (kBuildName)
// ---------------------------------------------------------------------------------------------

struct BuildParams {
    GridDims grid{};
    CameraView camera{};
    kernel::Span<ClusterAABB> out_aabbs; ///< cluster_count, flat cluster order
};

struct BuildKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const BuildParams& p) const {
        const u32 cluster = idx.linear;
        const u32 slice_z = cluster % p.grid.slices_z;
        const u32 tile = cluster / p.grid.slices_z;
        p.out_aabbs[cluster] = build_cluster_aabb(tile % p.grid.tiles_x, tile / p.grid.tiles_x, slice_z, p.grid,
                                                  p.camera.near_plane, p.camera.far_plane, p.camera.tan_x,
                                                  p.camera.tan_y);
    }
};

// ---------------------------------------------------------------------------------------------
// Light bounds (kBoundsName)
// ---------------------------------------------------------------------------------------------

using LightBounds = ClusterLightBounds;

struct BoundsParams {
    GridDims grid{};
    CameraView camera{};
    kernel::Span<const PointLightInput> point_lights;
    kernel::Span<const SpotLightInput> spot_lights; ///< Indexed after the point lights.
    kernel::Span<LightBounds> out_bounds;           ///< point + spot count
};

struct BoundsKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const BoundsParams& p) const {
        const u32 light = idx.linear;
        math::Vec3 world_pos{};
        f32 radius = 0.f;
        if (light < p.point_lights.size) {
            world_pos = p.point_lights[light].position;
            radius = p.point_lights[light].radius;
        } else {
            // Spot lights are bounded by their range sphere (conservative superset of the cone).
            world_pos = p.spot_lights[light - p.point_lights.size].position;
            radius = p.spot_lights[light - p.point_lights.size].radius;
        }
        LightBounds bounds{};
        // A light with no range (falloff is identically zero) occupies no cluster.
        if (!(radius > 0.f) || !std::isfinite(radius) || !std::isfinite(world_pos.x) || !std::isfinite(world_pos.y) ||
            !std::isfinite(world_pos.z)) {
            p.out_bounds[light] = bounds;
            return;
        }
        const CameraView& c = p.camera;
        bounds.center = world_to_view(c, world_pos);
        bounds.radius = radius;
        // Only depth slices the sphere can reach are candidates; the window is widened by one on each
        // side so float rounding in the log/pow slice mapping can never skip a cluster — the exact
        // sphere/AABB test decides membership.
        const f32 depth = -bounds.center.z;
        const f32 min_depth = std::clamp(depth - radius, c.near_plane, c.far_plane);
        const f32 max_depth = std::clamp(depth + radius, c.near_plane, c.far_plane);
        const u32 lo = slice_from_depth(min_depth, p.grid.slices_z, c.near_plane, c.far_plane);
        const u32 hi = slice_from_depth(max_depth, p.grid.slices_z, c.near_plane, c.far_plane);
        bounds.sliceLo = lo > 0u ? lo - 1u : 0u;
        bounds.sliceHi = std::min(hi + 1u, p.grid.slices_z - 1u);
        p.out_bounds[light] = bounds;
    }
};

// ---------------------------------------------------------------------------------------------
// Light cull (kCullName)
// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------
// Slice binning (kBinName)
// ---------------------------------------------------------------------------------------------

struct BinParams {
    kernel::Span<const LightBounds> bounds;
    u32 capacity = 0;               ///< Slots per slice list (= light count).
    kernel::Span<u32> out_lights;   ///< slice * capacity + i, ascending light index
    kernel::Span<u32> out_counts;   ///< Lights per slice
};

struct BinKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const BinParams& p) const {
        const u32 slice = idx.linear;
        u32* list = p.out_lights.data + static_cast<usize>(slice) * p.capacity;
        u32 count = 0u;
        for (u32 light = 0; light < p.bounds.size; ++light) {
            if (slice >= p.bounds[light].sliceLo && slice <= p.bounds[light].sliceHi) {
                list[count++] = light;
            }
        }
        p.out_counts[slice] = count;
    }
};

// ---------------------------------------------------------------------------------------------
// Light cull (kCullName)
// ---------------------------------------------------------------------------------------------

struct CullParams {
    GridDims grid{};
    u32 capacity = 0; ///< Slots per cluster list (min(max_lights, light count), or light count when unlimited).
    kernel::Span<const ClusterAABB> aabbs;
    kernel::Span<const LightBounds> bounds;
    u32 slice_capacity = 0;                ///< BinParams::capacity
    kernel::Span<const u32> slice_lights;  ///< BinKernel output
    kernel::Span<const u32> slice_counts;  ///< BinKernel output
    kernel::Span<u32> out_lights;  ///< cluster * capacity + i, ascending light index
    kernel::Span<u32> out_counts;  ///< Lights stored per cluster (<= capacity)
    kernel::Span<u32> out_dropped; ///< Intersecting lights dropped at capacity per cluster
};

struct CullKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const CullParams& p) const {
        const u32 cluster = idx.linear;
        const u32 slice_z = cluster % p.grid.slices_z;
        const ClusterAABB aabb = p.aabbs[cluster];
        const u32* candidates = p.slice_lights.data + static_cast<usize>(slice_z) * p.slice_capacity;
        const u32 candidate_count = p.slice_counts[slice_z];
        u32* list = p.out_lights.data + static_cast<usize>(cluster) * p.capacity;
        u32 count = 0u;
        u32 dropped = 0u;
        for (u32 i = 0; i < candidate_count; ++i) {
            const u32 light = candidates[i];
            const LightBounds& b = p.bounds[light];
            if (!sphere_intersects_aabb(b.center, b.radius, aabb)) {
                continue;
            }
            if (count < p.capacity) {
                list[count++] = light;
            } else {
                ++dropped;
            }
        }
        p.out_counts[cluster] = count;
        p.out_dropped[cluster] = dropped;
    }
};

// ---------------------------------------------------------------------------------------------
// Compaction (kCompactName)
// ---------------------------------------------------------------------------------------------

struct CompactParams {
    u32 capacity = 0;
    kernel::Span<const u32> lights;    ///< cull output (fixed capacity)
    kernel::Span<const u32> counts;    ///< cull output
    kernel::Span<const u32> offsets;   ///< exclusive scan of counts
    kernel::Span<u32> out_light_list;  ///< flat list (sum of counts)
};

struct CompactKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const CompactParams& p) const {
        const u32 cluster = idx.linear;
        const u32* src = p.lights.data + static_cast<usize>(cluster) * p.capacity;
        u32* dst = p.out_light_list.data + p.offsets[cluster];
        for (u32 i = 0; i < p.counts[cluster]; ++i) {
            dst[i] = src[i];
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Deferred shade (kShadeName)
// ---------------------------------------------------------------------------------------------

struct ShadeParams {
    u32 width = 0;
    u32 height = 0;
    f32 inv_width = 0.f;
    f32 inv_height = 0.f;
    GridDims grid{};
    CameraView camera{};
    const f32* device_depth = nullptr;
    const math::Vec3* normals = nullptr;
    const math::Vec3* albedo = nullptr;
    kernel::Span<const PointLightInput> lights;
    kernel::Span<const ClusterGridEntry> cluster_grid; ///< Rebuilt light grid (offset, count)
    kernel::Span<const u32> light_list;                 ///< Flat light list
    bool use_clusters = true;
    math::Vec3* out_radiance = nullptr;
    u32* shaded_pixels = nullptr;  ///< Global counters (order-independent integer adds).
    u32* skipped_pixels = nullptr;
    u32* evaluations = nullptr;    ///< [0] low, [1] high word of the u64 light-evaluation count.
};

/// Order-independent 64-bit counter from two u32 atomics: the add that wraps the low word carries.
FUSE_HOST_DEVICE inline void add_u64_counter(u32* words, u32 value) {
    const u32 old = kernel::global_atomic_add(&words[0], value);
    if (old + value < old) {
        kernel::global_atomic_add(&words[1], 1u);
    }
}

/// One pixel per thread. Phase 0 shades and writes the pixel's counters to the thread's own scratch
/// slots (padding threads write zeros); phase 1 thread 0 sums the tile in thread order and publishes it
/// with one set of global integer adds (order-independent).
struct ShadeKernel {
    static constexpr u32 kThreads = kShadeWorkgroup.x * kShadeWorkgroup.y * kShadeWorkgroup.z;
    using Scratch = u32;
    static constexpr u32 kScratchCount = 3u * kThreads; ///< [shaded | skipped | evaluations] x thread
    static constexpr u32 kPhases = 2u;

    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx,
                                     const kernel::WorkgroupContext<u32>& wg,
                                     const ShadeParams& p) const {
        u32* shaded = wg.scratch;
        u32* skipped = wg.scratch + kThreads;
        u32* evals = wg.scratch + 2u * kThreads;
        const u32 t = idx.local_linear;
        if (wg.phase == 1u) {
            if (t == 0u) {
                u32 tile_shaded = 0u;
                u32 tile_skipped = 0u;
                u64 tile_evals = 0u;
                for (u32 i = 0; i < kThreads; ++i) {
                    tile_shaded += shaded[i];
                    tile_skipped += skipped[i];
                    tile_evals += evals[i];
                }
                kernel::global_atomic_add(p.shaded_pixels, tile_shaded);
                kernel::global_atomic_add(p.skipped_pixels, tile_skipped);
                add_u64_counter(p.evaluations, static_cast<u32>(tile_evals));
                kernel::global_atomic_add(&p.evaluations[1], static_cast<u32>(tile_evals >> 32u));
            }
            return;
        }
        shaded[t] = 0u;
        skipped[t] = 0u;
        evals[t] = 0u;
        if (!idx.active) {
            return; // padding thread of an edge tile
        }
        const u32 px = idx.global.x;
        const u32 py = idx.global.y;
        const usize pixel = static_cast<usize>(py) * p.width + px;
        const f32 screen_x = (static_cast<f32>(px) + 0.5f) * p.inv_width;
        const f32 screen_y = (static_cast<f32>(py) + 0.5f) * p.inv_height;
        const CameraView& c = p.camera;

        const f32 view_depth =
            view_depth_from_device_depth(p.device_depth[pixel], c.near_plane, c.far_plane, c.reversed_z);
        u32 cluster = 0u;
        if (!(view_depth > 0.f) || !std::isfinite(view_depth) ||
            !map_screen_depth_to_cluster(screen_x, screen_y, view_depth, p.grid, c.near_plane, c.far_plane, cluster)) {
            p.out_radiance[pixel] = {};
            skipped[t] = 1u;
            return;
        }
        const math::Vec3 world_pos =
            view_to_world(c, view_position_from_screen(screen_x, screen_y, view_depth, c.tan_x, c.tan_y));
        const math::Vec3 normal = p.normals[pixel];
        const math::Vec3 albedo = p.albedo[pixel];

        u32 evaluations = 0u;
        p.out_radiance[pixel] =
            p.use_clusters
                ? shade_cluster_lights(p.cluster_grid, p.light_list, cluster, p.lights, world_pos, normal, albedo,
                                       evaluations)
                : shade_all_lights(p.lights, world_pos, normal, albedo, evaluations);
        shaded[t] = 1u;
        evals[t] = evaluations;
    }
};

// ---------------------------------------------------------------------------------------------
// Host helpers (launch descriptors + parameter resolution)
// ---------------------------------------------------------------------------------------------

inline GridDims make_grid(const ClusterDesc& desc) {
    return GridDims{desc.tilesX, desc.tilesY, desc.slicesZ, desc.maxLightsPerCluster};
}

inline CameraView make_camera(const ClusterCameraDesc& camera) {
    CameraView c{};
    c.position = camera.position;
    view_basis(camera.forward, camera.up, c.right, c.up, c.back);
    c.near_plane = camera.nearPlane;
    c.far_plane = camera.farPlane;
    c.tan_y = std::tan(camera.fovYRadians * 0.5f);
    c.tan_x = c.tan_y * camera.aspect();
    c.reversed_z = camera.reversedZ;
    return c;
}

inline kernel::KernelLaunch make_linear_launch(const char* name, u32 count) {
    return kernel::KernelLaunch{name, kernel::extent1(count), kLinearWorkgroup};
}

inline kernel::KernelLaunch make_shade_launch(u32 width, u32 height) {
    return kernel::KernelLaunch{kShadeName, kernel::extent2(width, height), kShadeWorkgroup};
}

} // namespace fuse::renderer::clustered_kernel
