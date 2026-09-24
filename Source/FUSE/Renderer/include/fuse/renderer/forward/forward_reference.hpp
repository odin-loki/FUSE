#pragma once

// WP-2.3 forward transparency: the CPU side that decides what is drawn and in which order, and the
// CPU composite the gates compare the blended image against. Stub-safe (no Vulkan).
//
// Transparent set: GpuScene instances with kInstanceValid | kInstanceVisible | kInstanceTransparent
// (the WP-1.3 culler skips kInstanceTransparent, so these never reach the visibility buffer), a mesh
// with a draw range in the scene index buffer and a positive bounding radius, whose world bounding
// sphere is not entirely outside the view frustum.
//
// Order: back to front by the clip-space w of the world bounding-sphere centre (the distance along
// the view axis for a perspective projection; clip z for an orthographic one), ties by slot, so the
// order is total and deterministic. A CPU sort, not a GPU one: the transparent set is small (tens to a
// few thousand instances against the opaque scene's 10^5), the CPU mirror of the GPU scene already
// holds every input, the result feeds plain vkCmdDrawIndexed calls in order (per-draw front face for
// mirrored instances, no indirect-count plumbing), and the gates can reproduce the exact order. At
// 4,096 transparent instances the collection + std::sort is ~0.1 ms on one core; a GPU radix sort
// (cmake/gpu_radix_sort.cmake) + indirect draws is the path if that ever dominates.
//
// Blend: premultiplied "over", colour = src.rgb + (1 - src.a) dst.rgb, alpha = src.a + (1 - src.a) dst.a,
// with src = (radiance * opacity, opacity) (blend_over below == the pipeline's blend state).

#include <fuse/math/vec.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::forward {

/// Per-slot opacity (ForwardTransparency::setOpacity); an entry applies while the slot's generation
/// matches, otherwise the instance is opaque-looking (opacity 1).
struct OpacityEntry {
    u32 generation = 0;
    f32 opacity = 1.f;
    bool set = false;
};

/// One transparent draw, CPU side.
struct SortedDraw {
    f32 key = 0.f;       ///< sort key (clip w, or clip z for an orthographic projection); larger = farther
    u32 slot = 0;        ///< instance slot
    f32 opacity = 1.f;   ///< clamped to [0, 1]
    u32 mesh = 0;        ///< GpuMesh index
    bool mirrored = false; ///< negative determinant: the winding flips
};

struct CollectParams {
    const gpu_scene::GpuInstance* instances = nullptr;
    const gpu_scene::GpuTransform* transforms = nullptr;
    u32 instanceCount = 0; ///< slot high-water mark
    const gpu_scene::GpuMesh* meshes = nullptr;
    u32 meshCount = 0;
    const f32* viewProj = nullptr; ///< column-major 4x4, Vulkan clip space (0 <= z <= w)
    const OpacityEntry* opacity = nullptr;
    u32 opacityCount = 0;
    bool frustumCull = true;
};

struct CollectResult {
    u32 count = 0;       ///< draws written
    u32 candidates = 0;  ///< transparent instances seen (before the frustum test / capacity)
    u32 culled = 0;      ///< outside the frustum
    u32 dropped = 0;     ///< beyond `capacity` (the nearest ones are kept)
};

inline constexpr u32 kTransparentFlags =
    gpu_scene::kInstanceValid | gpu_scene::kInstanceVisible | gpu_scene::kInstanceTransparent;

/// Row r of a column-major 4x4: (m[r], m[4 + r], m[8 + r], m[12 + r]).
inline f32 clip_row_dot(const f32* m, u32 r, const math::Vec3& p) {
    return m[r] * p.x + m[4u + r] * p.y + m[8u + r] * p.z + m[12u + r];
}

inline bool orthographic(const f32* viewProj) {
    return viewProj[3] == 0.f && viewProj[7] == 0.f && viewProj[11] == 0.f;
}

/// World bounding sphere of an instance (mesh bounds through the transform, radius x the largest column scale).
inline void world_sphere(const gpu_scene::GpuTransform& t, const gpu_scene::GpuMesh& mesh, math::Vec3& center, f32& radius) {
    const f32* c = mesh.boundsCenter;
    center = {t.rows[0][0] * c[0] + t.rows[0][1] * c[1] + t.rows[0][2] * c[2] + t.rows[0][3],
              t.rows[1][0] * c[0] + t.rows[1][1] * c[1] + t.rows[1][2] * c[2] + t.rows[1][3],
              t.rows[2][0] * c[0] + t.rows[2][1] * c[1] + t.rows[2][2] * c[2] + t.rows[2][3]};
    f32 s2 = 0.f;
    for (u32 j = 0; j < 3u; ++j) {
        const f32 l2 = t.rows[0][j] * t.rows[0][j] + t.rows[1][j] * t.rows[1][j] + t.rows[2][j] * t.rows[2][j];
        s2 = std::max(s2, l2);
    }
    radius = mesh.boundsRadius * std::sqrt(s2);
}

inline bool mirrored(const gpu_scene::GpuTransform& t) {
    const auto& r = t.rows;
    const f32 det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                    r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
    return det < 0.f;
}

/// False when the sphere is entirely outside one of the six clip planes (conservative).
inline bool sphere_in_frustum(const f32* m, const math::Vec3& c, f32 radius) {
    f32 rows[4][4];
    for (u32 r = 0; r < 4u; ++r) {
        for (u32 j = 0; j < 4u; ++j) {
            rows[r][j] = m[j * 4u + r];
        }
    }
    // Vulkan clip volume: -w <= x <= w, -w <= y <= w, 0 <= z <= w.
    f32 planes[6][4];
    for (u32 j = 0; j < 4u; ++j) {
        planes[0][j] = rows[3][j] + rows[0][j];
        planes[1][j] = rows[3][j] - rows[0][j];
        planes[2][j] = rows[3][j] + rows[1][j];
        planes[3][j] = rows[3][j] - rows[1][j];
        planes[4][j] = rows[2][j];
        planes[5][j] = rows[3][j] - rows[2][j];
    }
    for (const auto& pl : planes) {
        const f32 n = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
        if (pl[0] * c.x + pl[1] * c.y + pl[2] * c.z + pl[3] < -radius * n) {
            return false;
        }
    }
    return true;
}

/// Strict weak order: farther first, then ascending slot.
inline bool draw_before(const SortedDraw& a, const SortedDraw& b) {
    if (a.key != b.key) {
        return a.key > b.key;
    }
    return a.slot < b.slot;
}

/// Collects the transparent draws into `out` (capacity `capacity`) and sorts them back to front.
/// Never allocates. When more than `capacity` instances qualify, the nearest `capacity` are kept.
inline CollectResult collect_transparent(const CollectParams& p, SortedDraw* out, u32 capacity) {
    CollectResult r{};
    if (p.instances == nullptr || p.transforms == nullptr || p.meshes == nullptr || p.viewProj == nullptr) {
        return r;
    }
    const bool ortho = orthographic(p.viewProj);
    u32 kept = 0;
    for (u32 slot = 0; slot < p.instanceCount; ++slot) {
        const gpu_scene::GpuInstance& inst = p.instances[slot];
        if ((inst.flags & kTransparentFlags) != kTransparentFlags || inst.mesh >= p.meshCount) {
            continue;
        }
        const gpu_scene::GpuMesh& mesh = p.meshes[inst.mesh];
        if (mesh.indexCount == 0u || !(mesh.boundsRadius > 0.f)) {
            continue;
        }
        ++r.candidates;
        const gpu_scene::GpuTransform& t = p.transforms[slot];
        math::Vec3 center{};
        f32 radius = 0.f;
        world_sphere(t, mesh, center, radius);
        if (p.frustumCull && !sphere_in_frustum(p.viewProj, center, radius)) {
            ++r.culled;
            continue;
        }
        SortedDraw d{};
        d.key = ortho ? clip_row_dot(p.viewProj, 2u, center) : clip_row_dot(p.viewProj, 3u, center);
        d.slot = slot;
        f32 opacity = 1.f;
        if (slot < p.opacityCount && p.opacity != nullptr && p.opacity[slot].set && p.opacity[slot].generation == inst.generation) {
            opacity = p.opacity[slot].opacity;
        }
        d.opacity = std::isfinite(opacity) ? std::clamp(opacity, 0.f, 1.f) : 1.f;
        d.mesh = inst.mesh;
        d.mirrored = mirrored(t);
        if (kept < capacity) {
            out[kept++] = d;
            continue;
        }
        // Full: replace the farthest kept draw when this one is nearer (keeps the nearest `capacity`).
        ++r.dropped;
        u32 far = 0;
        for (u32 i = 1; i < kept; ++i) {
            if (draw_before(out[i], out[far])) {
                far = i;
            }
        }
        if (kept > 0u && draw_before(out[far], d)) {
            out[far] = d;
        }
    }
    std::sort(out, out + kept, draw_before);
    r.count = kept;
    return r;
}

/// Premultiplied "over" (the forward pipeline's blend state): src = (radiance * alpha, alpha).
FUSE_HOST_DEVICE inline math::Vec4 blend_over(const math::Vec3& radiance, f32 alpha, const math::Vec4& dst) {
    const f32 k = 1.f - alpha;
    return {radiance.x * alpha + dst.x * k, radiance.y * alpha + dst.y * k, radiance.z * alpha + dst.z * k,
            alpha + dst.w * k};
}

} // namespace fuse::renderer::forward
