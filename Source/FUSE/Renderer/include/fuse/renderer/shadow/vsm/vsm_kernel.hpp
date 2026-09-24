#pragma once

// WP-3.1 virtual shadow maps: the CPU reference of the GPU marking and invalidation math as
// single-source kernels (docs/compute-kernels.md). vsm_common.{glsl,slang} (this directory) evaluate
// the same f32 expressions in the same order with no contraction (`precise` / -fp-mode precise;
// this header is compiled with -ffp-contract=off), and use only +, -, *, /, floor, abs and compares
// (all correctly rounded on Lavapipe), so the GPU request bits equal these bit for bit.
//
// Page marking (vsm.mark, one pixel per item):
//   depth texel (forward depth; 1 = background, skipped) -> NDC -> light space through
//   VsmFrameConstants::depthToLight (one 4x4, then the perspective divide);
//   level: with d the light-space Chebyshev distance to the camera and level_of(q) = q < 1 ? 0 :
//   exponent(q) + 1 (read from the float's exponent bits: exact), the coarser of
//     containment = level_of(d * levelSelectScale), levelSelectScale = 1 / ((64 - 1) * pageWorld0):
//                   the finest window that still holds the point with a page of margin, and
//     density     = level_of(d * densityScale) + lodBias, densityScale = pixelSpread / (texelsPerPixel *
//                   texel0): the first level whose texel covers the pixel footprint (screen density);
//   levels past the last are not marked;
//   page = floor(light.xy * invPageWorld[level]) (absolute page), marked in slot page mod 128 when it
//   lies in the level's window; with markRadiusPages > 0 the neighbours within that radius too (at most
//   2 x 2 pages, for filtering footprints that cross a page edge).
//
// Invalidation (vsm.invalidate, one instance slot x one level per item on the GPU):
//   an instance's world bounds record = the mesh bounding sphere through the 3x4 transform as a
//   conservative box (centre = T * c, half extent_k = r * sum_j |T_kj|); a record that differs (bit for
//   bit) from last frame's clears the cached bit of every page of every level under the light-space
//   footprint of the old and of the new box (clipped to the level's window).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/shadow/vsm/vsm_types.hpp>

#include <cmath>
#include <cstring>

namespace fuse::renderer::vsm::kernel_math {

inline constexpr f32 kMaxLightCoord = 1e30f; ///< larger light-space coordinates are rejected
inline constexpr f32 kMaxPageCoord = 1e9f;   ///< page coordinates are clamped / rejected beyond this

FUSE_HOST_DEVICE inline u32 float_bits(f32 f) {
    u32 u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

FUSE_HOST_DEVICE inline f32 abs_f(f32 v) { return v < 0.f ? -v : v; }
FUSE_HOST_DEVICE inline f32 max_f(f32 a, f32 b) { return a > b ? a : b; }
FUSE_HOST_DEVICE inline s32 max_s(s32 a, s32 b) { return a > b ? a : b; }
FUSE_HOST_DEVICE inline s32 min_s(s32 a, s32 b) { return a < b ? a : b; }

/// Slot of an absolute page coordinate (kPagesPerAxis is a power of two).
FUSE_HOST_DEVICE inline u32 slot_of(s32 a) { return static_cast<u32>(a) & (kPagesPerAxis - 1u); }

/// floor(log2(q)) + 1 for q >= 1 (from the exponent bits: exact), 0 below.
FUSE_HOST_DEVICE inline s32 level_of(f32 q) {
    return q >= 1.f ? static_cast<s32>((float_bits(q) >> 23u) & 0xFFu) - 127 + 1 : 0;
}

/// Clipmap level for a light-space point, or -1 when it is past the last level: the coarser of the
/// containment level (the finest window that holds the point with a page of margin) and the density
/// level + lodBias (the first level whose texel covers the pixel footprint / texelsPerPixel).
FUSE_HOST_DEVICE inline s32 select_level(const VsmFrameConstants& c, f32 lx, f32 ly, f32 lz) {
    const f32 dx = abs_f(lx - c.cameraLight[0]);
    const f32 dy = abs_f(ly - c.cameraLight[1]);
    const f32 dz = abs_f(lz - c.cameraLight[2]);
    const f32 d = max_f(max_f(dx, dy), dz);
    const s32 containment = level_of(d * c.levelSelectScale);
    const s32 density = level_of(d * c.densityScale) + c.lodBias;
    const s32 level = max_s(containment, density);
    return level < static_cast<s32>(c.levels) && level < static_cast<s32>(kMaxLevels) ? level : -1;
}

/// Virtual pages (level-major index) a depth texel requests; returns their count (0..4).
FUSE_HOST_DEVICE inline u32 mark_pixel(const VsmFrameConstants& c, u32 x, u32 y, f32 depth, u32 out[4]) {
    if (!(depth < 1.f) || !(depth >= 0.f)) {
        return 0u; // background (or NaN)
    }
    const f32 nx = (static_cast<f32>(x) + 0.5f) * c.ndcScaleX - 1.f;
    const f32 ny = (static_cast<f32>(y) + 0.5f) * c.ndcScaleY - 1.f;
    const f32* m = c.depthToLight;
    const f32 hx = ((m[0] * nx + m[4] * ny) + m[8] * depth) + m[12];
    const f32 hy = ((m[1] * nx + m[5] * ny) + m[9] * depth) + m[13];
    const f32 hz = ((m[2] * nx + m[6] * ny) + m[10] * depth) + m[14];
    const f32 hw = ((m[3] * nx + m[7] * ny) + m[11] * depth) + m[15];
    if (!(hw > 0.f)) {
        return 0u;
    }
    const f32 lx = hx / hw;
    const f32 ly = hy / hw;
    const f32 lz = hz / hw;
    if (!(abs_f(lx) <= kMaxLightCoord) || !(abs_f(ly) <= kMaxLightCoord) || !(abs_f(lz) <= kMaxLightCoord)) {
        return 0u;
    }
    const s32 level = select_level(c, lx, ly, lz);
    if (level < 0) {
        return 0u;
    }
    const VsmLevelConstants& L = c.level[level];
    const f32 u = lx * L.invPageWorld;
    const f32 v = ly * L.invPageWorld;
    if (!(abs_f(u) < kMaxPageCoord) || !(abs_f(v) < kMaxPageCoord)) {
        return 0u;
    }
    const s32 half = static_cast<s32>(kPagesPerAxis / 2u);
    const s32 wx0 = L.originX - half;
    const s32 wx1 = L.originX + half - 1;
    const s32 wy0 = L.originY - half;
    const s32 wy1 = L.originY + half - 1;
    const s32 cx = static_cast<s32>(std::floor(u));
    const s32 cy = static_cast<s32>(std::floor(v));
    if (cx < wx0 || cx > wx1 || cy < wy0 || cy > wy1) {
        return 0u;
    }
    const f32 r = c.markRadiusPages;
    const s32 x0 = max_s(static_cast<s32>(std::floor(u - r)), wx0);
    const s32 x1 = min_s(static_cast<s32>(std::floor(u + r)), wx1);
    const s32 y0 = max_s(static_cast<s32>(std::floor(v - r)), wy0);
    const s32 y1 = min_s(static_cast<s32>(std::floor(v + r)), wy1);
    const u32 base = static_cast<u32>(level) * kPagesPerLevel;
    u32 n = 0u;
    for (s32 py = y0; py <= y1 && n < 4u; ++py) {
        for (s32 px = x0; px <= x1 && n < 4u; ++px) {
            out[n++] = base + slot_of(py) * kPagesPerAxis + slot_of(px);
        }
    }
    return n;
}

/// Bounds record of one instance slot (all zero when it is not tracked). `mesh` is null when the
/// instance's mesh index is out of range.
FUSE_HOST_DEVICE inline VsmBoundsRecord make_bounds_record(const gpu_scene::GpuInstance& inst, const gpu_scene::GpuMesh* mesh,
                                                           const gpu_scene::GpuTransform& t) {
    VsmBoundsRecord r{};
    const u32 need = gpu_scene::kInstanceValid | gpu_scene::kInstanceCastShadow;
    if ((inst.flags & need) != need || mesh == nullptr) {
        return r;
    }
    const f32 c0 = mesh->boundsCenter[0];
    const f32 c1 = mesh->boundsCenter[1];
    const f32 c2 = mesh->boundsCenter[2];
    const f32 rad = mesh->boundsRadius;
    for (u32 k = 0; k < 3u; ++k) {
        const f32* row = t.rows[k];
        r.center[k] = ((row[0] * c0 + row[1] * c1) + row[2] * c2) + row[3];
        r.extent[k] = rad * ((abs_f(row[0]) + abs_f(row[1])) + abs_f(row[2]));
    }
    r.tracked = 1u;
    return r;
}

FUSE_HOST_DEVICE inline bool same_record(const VsmBoundsRecord& a, const VsmBoundsRecord& b) {
    return float_bits(a.center[0]) == float_bits(b.center[0]) && float_bits(a.center[1]) == float_bits(b.center[1]) &&
           float_bits(a.center[2]) == float_bits(b.center[2]) && a.tracked == b.tracked &&
           float_bits(a.extent[0]) == float_bits(b.extent[0]) && float_bits(a.extent[1]) == float_bits(b.extent[1]) &&
           float_bits(a.extent[2]) == float_bits(b.extent[2]) && a.reserved == b.reserved;
}

FUSE_HOST_DEVICE inline f32 clamp_page(f32 v) {
    return v < -kMaxPageCoord ? -kMaxPageCoord : (v > kMaxPageCoord ? kMaxPageCoord : v);
}

/// Absolute page rectangle {x0, y0, x1, y1} (inclusive) of a record's light-space footprint on one
/// level, clipped to the level's window; false when it is untracked, degenerate or outside.
FUSE_HOST_DEVICE inline bool bounds_rect(const VsmFrameConstants& c, const VsmBoundsRecord& b, u32 level, s32 rect[4]) {
    if (b.tracked == 0u || level >= c.levels || level >= kMaxLevels) {
        return false;
    }
    const f32* R = c.lightRotation;
    const f32 lcx = (R[0] * b.center[0] + R[1] * b.center[1]) + R[2] * b.center[2];
    const f32 lcy = (R[4] * b.center[0] + R[5] * b.center[1]) + R[6] * b.center[2];
    const f32 lex = (abs_f(R[0]) * b.extent[0] + abs_f(R[1]) * b.extent[1]) + abs_f(R[2]) * b.extent[2];
    const f32 ley = (abs_f(R[4]) * b.extent[0] + abs_f(R[5]) * b.extent[1]) + abs_f(R[6]) * b.extent[2];
    const VsmLevelConstants& L = c.level[level];
    const f32 fx0 = (lcx - lex) * L.invPageWorld;
    const f32 fx1 = (lcx + lex) * L.invPageWorld;
    const f32 fy0 = (lcy - ley) * L.invPageWorld;
    const f32 fy1 = (lcy + ley) * L.invPageWorld;
    if (!(fx0 <= fx1) || !(fy0 <= fy1)) {
        return false; // NaN / negative extent
    }
    const s32 half = static_cast<s32>(kPagesPerAxis / 2u);
    const s32 x0 = max_s(static_cast<s32>(std::floor(clamp_page(fx0))), L.originX - half);
    const s32 x1 = min_s(static_cast<s32>(std::floor(clamp_page(fx1))), L.originX + half - 1);
    const s32 y0 = max_s(static_cast<s32>(std::floor(clamp_page(fy0))), L.originY - half);
    const s32 y1 = min_s(static_cast<s32>(std::floor(clamp_page(fy1))), L.originY + half - 1);
    if (x0 > x1 || y0 > y1) {
        return false;
    }
    rect[0] = x0;
    rect[1] = y0;
    rect[2] = x1;
    rect[3] = y1;
    return true;
}

} // namespace fuse::renderer::vsm::kernel_math

namespace fuse::renderer::vsm::mark_kernel {

inline constexpr const char* kName = "vsm.mark";
inline constexpr u32 kWorkgroup = 64u;

struct Params {
    const VsmFrameConstants* constants = nullptr;
    kernel::Span<const f32> depth; ///< width x height, row-major
    u32 width = 0;
    kernel::Span<u32> pages;       ///< 4 per pixel: requested virtual pages, kPageNone-padded
};

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const u32 x = idx.linear % p.width;
        const u32 y = idx.linear / p.width;
        u32 out[4] = {kPageNone, kPageNone, kPageNone, kPageNone};
        kernel_math::mark_pixel(*p.constants, x, y, p.depth[idx.linear], out);
        for (u32 i = 0; i < 4u; ++i) {
            p.pages[idx.linear * 4u + i] = out[i];
        }
    }
};

inline kernel::KernelLaunch make_launch(u32 width, u32 height) {
    return kernel::KernelLaunch{kName, kernel::extent1(width * height), {kWorkgroup, 1u, 1u}};
}

} // namespace fuse::renderer::vsm::mark_kernel

namespace fuse::renderer::vsm::bounds_kernel {

inline constexpr const char* kName = "vsm.bounds";
inline constexpr u32 kWorkgroup = 64u;

struct Params {
    kernel::Span<const gpu_scene::GpuInstance> instances;
    kernel::Span<const gpu_scene::GpuTransform> transforms;
    kernel::Span<const gpu_scene::GpuMesh> meshes;
    kernel::Span<VsmBoundsRecord> records; ///< one per instance slot
};

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const gpu_scene::GpuInstance& inst = p.instances[idx.linear];
        const gpu_scene::GpuMesh* mesh = inst.mesh < p.meshes.size ? &p.meshes[inst.mesh] : nullptr;
        p.records[idx.linear] = kernel_math::make_bounds_record(inst, mesh, p.transforms[idx.linear]);
    }
};

inline kernel::KernelLaunch make_launch(u32 instances) {
    return kernel::KernelLaunch{kName, kernel::extent1(instances), {kWorkgroup, 1u, 1u}};
}

} // namespace fuse::renderer::vsm::bounds_kernel
