#pragma once

// WP-3.2 virtual shadow maps: the CPU reference of the page rasteriser and of the shadow filters
// (docs/compute-kernels.md, single source). shaders/shadow_vsm/vsm_shadow.{glsl,slang} evaluate the
// same f32 expressions in the same order with no contraction (`precise` / -fp-mode precise; this
// header is compiled with -ffp-contract=off) and use only +, -, *, /, floor and compares, so on
// Lavapipe (correctly rounded division) the GPU pages and visibilities equal these bit for bit;
// the gates still compare depths with a tolerance and treat a depth comparison within kAmbiguity of
// its threshold as ambiguous, because Vulkan allows 2.5 ulp division.
//
// Directional page raster ("vsm.raster", one workgroup per render-list page):
//   vertex: object -> world (the 3x4 transform, rows) -> light space (VsmFrameConstants::lightRotation);
//   page texel coordinates tx = (lx * invPageWorld - pageX) * 128, ty likewise (absolute page of the
//   render-list slot in the level's window); depth d = ((depthCenter - lz) * invPageWorld) / 256 + 0.5
//   (0 towards the light; the level's depth key centres the range, see kDirDepthPages).
//   Triangle: both windings; edge functions at texel centres (x + 0.5, y + 0.5), inside when the three
//   barycentrics w_k / area are >= 0 (shared edges are covered twice: harmless under a min); depth
//   interpolated linearly, clamped to [0, 1] per texel, then atomic-min of its float bits into the
//   R32_UINT pool (non-negative floats order like their bits). A triangle whose three vertex depths
//   are > 1 is skipped (it cannot lower a cleared texel).
// Local page raster ("vsm.local_raster", one workgroup per spot page / cube face):
//   view space of the face (spot: the light's forward / right / up; point: cube_basis(face)), clipped
//   against z = nearPlane (a triangle becomes 0, 1 or 2), perspective projection tx = (x / z *
//   invTanHalf) * 64 + 64; coverage by the same edge functions, depth = the view depth where the texel
//   centre's ray meets the triangle's view-space plane (persp_depth), stored as z * invRange.
//   A triangle with every vertex beyond the range is skipped.
// Shadow lookup (light.shade, the forward pass, vsm.probe):
//   directional: the receiver's level = the marking's (select_level of its light-space position); while
//   the centre page is unmapped it tries one level finer, then the coarser levels; the receiver moves along its
//   normal by normalOffset texels of that level, its depth is lowered by depthBias texels; a tap is lit
//   when receiverDepth <= stored. Taps in unmapped pages are skipped.
//   local: face (point: the major axis of the offset receiver), same offset / bias with the texel size
//   at the receiver's view depth; taps clamp to the page.
//   Filters: hard (one tap), PCF (box of (2r + 1)^2 taps, visibility = lit / taps), PCSS (5 x 5 blocker
//   search over pcssMaxRadius, average blocker depth, penumbra = light size x (receiver - blocker) /
//   blocker (directional: x tan(angular radius)), PCF with that radius in texels, capped).

#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/shadow/vsm/vsm_types.hpp>
#include <fuse/renderer/shadow/vsm_raster/vsm_raster_types.hpp>

#include <cmath>
#include <cstring>

namespace fuse::renderer::vsm::raster_math {

inline constexpr f32 kMaxLightCoord = 1e30f;
inline constexpr f32 kMaxPageCoord = 1e9f;
inline constexpr f32 kMaxArea = 3.0e38f;
/// A depth comparison this close to its threshold may flip between conforming implementations.
inline constexpr f32 kAmbiguity = 1e-6f;
inline constexpr s32 kWindowTexels = static_cast<s32>(kPagesPerAxis * kPageTexels);

FUSE_HOST_DEVICE inline u32 bits_of(f32 f) {
    u32 u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
FUSE_HOST_DEVICE inline f32 float_of(u32 u) {
    f32 f = 0.f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}
FUSE_HOST_DEVICE inline f32 abs_f(f32 v) { return v < 0.f ? -v : v; }
FUSE_HOST_DEVICE inline f32 min_f(f32 a, f32 b) { return a < b ? a : b; }
FUSE_HOST_DEVICE inline f32 max_f(f32 a, f32 b) { return a > b ? a : b; }
FUSE_HOST_DEVICE inline s32 min_s(s32 a, s32 b) { return a < b ? a : b; }
FUSE_HOST_DEVICE inline s32 max_s(s32 a, s32 b) { return a > b ? a : b; }
FUSE_HOST_DEVICE inline u32 min_u(u32 a, u32 b) { return a < b ? a : b; }
FUSE_HOST_DEVICE inline u32 slot_of(s32 a) { return static_cast<u32>(a) & (kPagesPerAxis - 1u); }

// --- vertex transforms ------------------------------------------------------------------------------

/// world = rows . (p, 1), left to right (fuse_vis_clip's order).
FUSE_HOST_DEVICE inline void world_point(const gpu_scene::GpuTransform& t, const f32 p[3], f32 w[3]) {
    for (u32 r = 0; r < 3u; ++r) {
        w[r] = ((t.rows[r][0] * p[0] + t.rows[r][1] * p[1]) + t.rows[r][2] * p[2]) + t.rows[r][3];
    }
}

/// light = lightRotation . world (vsm_kernel.hpp bounds_rect's order).
FUSE_HOST_DEVICE inline void light_point(const VsmFrameConstants& c, const f32 w[3], f32 l[3]) {
    const f32* R = c.lightRotation;
    l[0] = (R[0] * w[0] + R[1] * w[1]) + R[2] * w[2];
    l[1] = (R[4] * w[0] + R[5] * w[1]) + R[6] * w[2];
    l[2] = (R[8] * w[0] + R[9] * w[1]) + R[10] * w[2];
}

/// Directional depth of light-space z on a level.
FUSE_HOST_DEVICE inline f32 dir_depth(const VsmLevelConstants& L, f32 lz) {
    return ((L.depthCenter - lz) * L.invPageWorld) * (1.f / kDirDepthPages) + 0.5f;
}

/// Page texel coordinates + depth of a light-space point for absolute page (ax, ay) of a level.
FUSE_HOST_DEVICE inline void dir_page_vertex(const VsmLevelConstants& L, s32 ax, s32 ay, const f32 l[3], f32& x, f32& y, f32& d) {
    x = (l[0] * L.invPageWorld - static_cast<f32>(ax)) * static_cast<f32>(kPageTexels);
    y = (l[1] * L.invPageWorld - static_cast<f32>(ay)) * static_cast<f32>(kPageTexels);
    d = dir_depth(L, l[2]);
}

/// Absolute page of a render-list virtual page (level-major, slot y, slot x).
FUSE_HOST_DEVICE inline void dir_page_of(const VsmFrameConstants& c, u32 virtualPage, u32& level, s32& ax, s32& ay) {
    level = virtualPage / kPagesPerLevel;
    const u32 inLevel = virtualPage % kPagesPerLevel;
    const u32 sx = inLevel % kPagesPerAxis;
    const u32 sy = inLevel / kPagesPerAxis;
    const VsmLevelConstants& L = c.level[level < kMaxLevels ? level : 0u];
    const s32 half = static_cast<s32>(kPagesPerAxis / 2u);
    const s32 bx = L.originX - half;
    const s32 by = L.originY - half;
    ax = bx + static_cast<s32>((static_cast<u32>(static_cast<s32>(sx) - bx)) & (kPagesPerAxis - 1u));
    ay = by + static_cast<s32>((static_cast<u32>(static_cast<s32>(sy) - by)) & (kPagesPerAxis - 1u));
}

/// Cube face basis (forward, right, up), exact +-1 / 0 components.
FUSE_HOST_DEVICE inline void cube_basis(u32 face, f32 f[3], f32 r[3], f32 u[3]) {
    for (u32 k = 0; k < 3u; ++k) {
        f[k] = 0.f;
        r[k] = 0.f;
        u[k] = 0.f;
    }
    switch (face) {
    case 0u: f[0] = 1.f; r[2] = -1.f; u[1] = 1.f; break;
    case 1u: f[0] = -1.f; r[2] = 1.f; u[1] = 1.f; break;
    case 2u: f[1] = 1.f; r[0] = 1.f; u[2] = -1.f; break;
    case 3u: f[1] = -1.f; r[0] = 1.f; u[2] = 1.f; break;
    case 4u: f[2] = 1.f; r[0] = 1.f; u[1] = 1.f; break;
    default: f[2] = -1.f; r[0] = -1.f; u[1] = 1.f; break;
    }
}

/// Cube face of a light-relative direction: the major axis (ties: x, then y, then z), + face when >= 0.
FUSE_HOST_DEVICE inline u32 cube_face(const f32 rel[3]) {
    const f32 ax = abs_f(rel[0]);
    const f32 ay = abs_f(rel[1]);
    const f32 az = abs_f(rel[2]);
    if (ax >= ay && ax >= az) {
        return rel[0] >= 0.f ? 0u : 1u;
    }
    if (ay >= az) {
        return rel[1] >= 0.f ? 2u : 3u;
    }
    return rel[2] >= 0.f ? 4u : 5u;
}

FUSE_HOST_DEVICE inline f32 dot3(const f32 a[3], const f32 b[3]) { return (a[0] * b[0] + a[1] * b[1]) + a[2] * b[2]; }

/// Face basis of a local light (spot: its own; point: the cube face).
FUSE_HOST_DEVICE inline void local_basis(const VsmLocalLight& e, u32 face, f32 f[3], f32 r[3], f32 u[3]) {
    if (e.type == kLocalPoint) {
        cube_basis(face, f, r, u);
        return;
    }
    for (u32 k = 0; k < 3u; ++k) {
        f[k] = e.forward[k];
        r[k] = e.right[k];
        u[k] = e.up[k];
    }
}

/// View-space position (x right, y up, z along the face) of a world point.
FUSE_HOST_DEVICE inline void local_view(const VsmLocalLight& e, const f32 f[3], const f32 r[3], const f32 u[3], const f32 w[3],
                                        f32 v[3]) {
    const f32 rel[3] = {w[0] - e.position[0], w[1] - e.position[1], w[2] - e.position[2]};
    v[0] = dot3(rel, r);
    v[1] = dot3(rel, u);
    v[2] = dot3(rel, f);
}

/// Screen texel coordinates of a view-space point in front of the near plane.
FUSE_HOST_DEVICE inline void local_project(const VsmLocalLight& e, const f32 v[3], f32& x, f32& y, f32& iz) {
    x = ((v[0] / v[2]) * e.invTanHalf) * 64.f + 64.f;
    y = ((v[1] / v[2]) * e.invTanHalf) * 64.f + 64.f;
    iz = 1.f / v[2];
}

// --- triangle rasteriser ----------------------------------------------------------------------------

/// A screen-space triangle of one page: texel coordinates and, for directional pages, the vertex
/// depths z (interpolated linearly); for local pages z = the view-space plane normal and h its offset
/// (n . v = h): the depth at a texel is the ray / plane intersection, which stays well conditioned when
/// near-plane clipping throws projected vertices thousands of texels off the page.
struct Tri {
    f32 x[3] = {};
    f32 y[3] = {};
    f32 z[3] = {};
    f32 h = 0.f;
};

struct TriSetup {
    f32 ex[3] = {}; ///< edge k (opposite vertex k): x[k+2] - x[k+1]
    f32 ey[3] = {};
    f32 invArea = 0.f;
    f32 dz1 = 0.f;
    f32 dz2 = 0.f;
    s32 x0 = 0, y0 = 0, x1 = -1, y1 = -1; ///< texel bounding box in the page (inclusive)
};

/// false when the triangle has no area, is not finite, or misses the page's texel centres' box.
FUSE_HOST_DEVICE inline bool setup_tri(const Tri& t, TriSetup& s) {
    s.ex[0] = t.x[2] - t.x[1];
    s.ey[0] = t.y[2] - t.y[1];
    s.ex[1] = t.x[0] - t.x[2];
    s.ey[1] = t.y[0] - t.y[2];
    s.ex[2] = t.x[1] - t.x[0];
    s.ey[2] = t.y[1] - t.y[0];
    const f32 area = s.ex[2] * (t.y[2] - t.y[0]) - s.ey[2] * (t.x[2] - t.x[0]);
    if (!(abs_f(area) > 0.f) || !(abs_f(area) < kMaxArea)) {
        return false;
    }
    s.invArea = 1.f / area;
    s.dz1 = t.z[1] - t.z[0];
    s.dz2 = t.z[2] - t.z[0];
    const f32 minX = min_f(min_f(t.x[0], t.x[1]), t.x[2]);
    const f32 maxX = max_f(max_f(t.x[0], t.x[1]), t.x[2]);
    const f32 minY = min_f(min_f(t.y[0], t.y[1]), t.y[2]);
    const f32 maxY = max_f(max_f(t.y[0], t.y[1]), t.y[2]);
    const f32 top = static_cast<f32>(kPageTexels) - 1.f;
    const f32 fx0 = max_f(std::ceil(minX - 0.5f), 0.f);
    const f32 fx1 = min_f(std::floor(maxX - 0.5f), top);
    const f32 fy0 = max_f(std::ceil(minY - 0.5f), 0.f);
    const f32 fy1 = min_f(std::floor(maxY - 0.5f), top);
    if (!(fx0 <= fx1) || !(fy0 <= fy1)) {
        return false;
    }
    s.x0 = static_cast<s32>(fx0);
    s.x1 = static_cast<s32>(fx1);
    s.y0 = static_cast<s32>(fy0);
    s.y1 = static_cast<s32>(fy1);
    return true;
}

/// Coverage of texel (px, py) and the interpolated z at its centre.
FUSE_HOST_DEVICE inline bool tri_texel(const Tri& t, const TriSetup& s, s32 px, s32 py, f32& z) {
    const f32 cx = static_cast<f32>(px) + 0.5f;
    const f32 cy = static_cast<f32>(py) + 0.5f;
    const f32 w0 = s.ex[0] * (cy - t.y[1]) - s.ey[0] * (cx - t.x[1]);
    const f32 w1 = s.ex[1] * (cy - t.y[2]) - s.ey[1] * (cx - t.x[2]);
    const f32 w2 = s.ex[2] * (cy - t.y[0]) - s.ey[2] * (cx - t.x[0]);
    const f32 b0 = w0 * s.invArea;
    const f32 b1 = w1 * s.invArea;
    const f32 b2 = w2 * s.invArea;
    if (!(b0 >= 0.f) || !(b1 >= 0.f) || !(b2 >= 0.f)) {
        return false;
    }
    z = (t.z[0] + b1 * s.dz1) + b2 * s.dz2;
    return true;
}

/// Clamp to [0, 1] (NaN -> 0 cannot happen for finite inputs; kept deterministic anyway).
FUSE_HOST_DEVICE inline f32 clamp_depth(f32 d) {
    const f32 lo = d > 0.f ? d : 0.f;
    return lo < 1.f ? lo : 1.f;
}


/// View-space plane of a triangle: n = (v1 - v0) x (v2 - v0), h = n . v0.
FUSE_HOST_DEVICE inline void view_plane(const f32 v[3][3], f32 n[3], f32& h) {
    const f32 e1[3] = {v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2]};
    const f32 e2[3] = {v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2]};
    n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    h = dot3(n, v[0]);
}

/// Stored local depth at texel (px, py): view depth of the triangle's plane along the texel centre's ray
/// (x / z = ((px + 0.5 - 64) / 64) / invTanHalf), times invRange, clamped; 1 when the plane is not in
/// front (grazing).
FUSE_HOST_DEVICE inline f32 persp_depth(const Tri& t, const VsmLocalLight& e, s32 px, s32 py) {
    const f32 qx = ((static_cast<f32>(px) + 0.5f - 64.f) * (1.f / 64.f)) / e.invTanHalf;
    const f32 qy = ((static_cast<f32>(py) + 0.5f - 64.f) * (1.f / 64.f)) / e.invTanHalf;
    const f32 den = (t.z[0] * qx + t.z[1] * qy) + t.z[2];
    const f32 z = t.h / den;
    return z > 0.f ? clamp_depth(z * e.invRange) : 1.f;
}

/// Clips a view-space triangle against z >= nearPlane (Sutherland-Hodgman, one plane) and projects it:
/// 0, 1 or 2 screen triangles carrying the triangle's view plane. Returns their count.
FUSE_HOST_DEVICE inline u32 clip_project(const VsmLocalLight& e, const f32 v[3][3], Tri out[2]) {
    f32 poly[4][3];
    u32 n = 0;
    const f32 zn = e.nearPlane;
    for (u32 i = 0; i < 3u; ++i) {
        const f32* a = v[i];
        const f32* b = v[(i + 1u) % 3u];
        const bool ain = a[2] >= zn;
        const bool bin = b[2] >= zn;
        if (ain) {
            poly[n][0] = a[0];
            poly[n][1] = a[1];
            poly[n][2] = a[2];
            ++n;
        }
        if (ain != bin) {
            const f32 t = (zn - a[2]) / (b[2] - a[2]);
            poly[n][0] = a[0] + (b[0] - a[0]) * t;
            poly[n][1] = a[1] + (b[1] - a[1]) * t;
            poly[n][2] = zn;
            ++n;
        }
    }
    if (n < 3u) {
        return 0u;
    }
    f32 sx[4], sy[4], sz[4];
    for (u32 i = 0; i < n; ++i) {
        local_project(e, poly[i], sx[i], sy[i], sz[i]);
    }
    f32 plane[3];
    f32 h = 0.f;
    view_plane(v, plane, h);
    const u32 count = n - 2u;
    for (u32 k = 0; k < count; ++k) {
        const u32 idx[3] = {0u, k + 1u, k + 2u};
        for (u32 j = 0; j < 3u; ++j) {
            out[k].x[j] = sx[idx[j]];
            out[k].y[j] = sy[idx[j]];
            out[k].z[j] = plane[j];
        }
        out[k].h = h;
    }
    return count;
}

// --- shadow lookup ----------------------------------------------------------------------------------

/// CPU view of the sampled resources (read-back or CPU-rendered).
struct ShadowStore {
    const u32* pageTable = nullptr; ///< VsmFrameConstants::virtualPages PTEs
    const u32* pool = nullptr;      ///< physical pool, row-major, poolWidth texels per row
    u32 poolWidth = 0;
    const u32* local = nullptr;     ///< local atlas, row-major, localWidth texels per row
    u32 localWidth = 0;
};

struct SampleResult {
    f32 visibility = 1.f;   ///< < 0: the forced level's page is unmapped (probe only)
    s32 level = -1;         ///< directional level sampled
    f32 receiverDepth = 0.f;
    f32 margin = 3.0e38f;   ///< min |receiverDepth - stored| over the comparisons made
};

/// One comparison tap: accumulates lit / taps / margin.
struct TapAcc {
    f32 lit = 0.f;
    f32 taps = 0.f;
    f32 margin = 3.0e38f;
    f32 blockerSum = 0.f;
    f32 blockers = 0.f;
};

FUSE_HOST_DEVICE inline void tap_compare(TapAcc& a, f32 rd, u32 bits) {
    const f32 stored = float_of(bits);
    a.taps += 1.f;
    a.lit += rd <= stored ? 1.f : 0.f;
    a.margin = min_f(a.margin, abs_f(rd - stored));
}

FUSE_HOST_DEVICE inline void tap_blocker(TapAcc& a, f32 rd, u32 bits) {
    const f32 stored = float_of(bits);
    a.margin = min_f(a.margin, abs_f(rd - stored));
    if (stored < rd) {
        a.blockerSum += stored;
        a.blockers += 1.f;
    }
}

/// Blocker-grid offset k (0..kPcssBlockerGrid-1) for a search radius (symmetric integer division).
FUSE_HOST_DEVICE inline s32 blocker_offset(u32 k, u32 radius) {
    const s32 i = static_cast<s32>(k) - 2;
    const s32 m = static_cast<s32>((static_cast<u32>(i < 0 ? -i : i) * radius) / 2u);
    return i < 0 ? -m : m;
}

/// Filter radius from a penumbra width in texels (rounded, capped).
FUSE_HOST_DEVICE inline u32 penumbra_radius(f32 texels, u32 cap) {
    const f32 w = texels > 0.f ? texels + 0.5f : 0.f;
    const f32 capped = w < static_cast<f32>(cap) ? w : static_cast<f32>(cap);
    return static_cast<u32>(std::floor(capped));
}

/// Directional tap: depth bits of window texel (ix, iy) of a level when its page is mapped.
FUSE_HOST_DEVICE inline bool dir_fetch(const VsmFrameConstants& c, const ShadowStore& st, u32 level, s32 ix, s32 iy, u32& bits) {
    if (ix < 0 || iy < 0 || ix >= kWindowTexels || iy >= kWindowTexels) {
        return false;
    }
    const VsmLevelConstants& L = c.level[level];
    const s32 half = static_cast<s32>(kPagesPerAxis / 2u);
    const s32 ax = (L.originX - half) + ix / static_cast<s32>(kPageTexels);
    const s32 ay = (L.originY - half) + iy / static_cast<s32>(kPageTexels);
    const u32 v = level * kPagesPerLevel + slot_of(ay) * kPagesPerAxis + slot_of(ax);
    const u32 pte = st.pageTable[v];
    if ((pte & kPteMapped) == 0u) {
        return false;
    }
    const u32 phys = pte & kPtePhysMask;
    const u32 x = (phys % c.poolPagesX) * kPageTexels + static_cast<u32>(ix) % kPageTexels;
    const u32 y = (phys / c.poolPagesX) * kPageTexels + static_cast<u32>(iy) % kPageTexels;
    bits = st.pool[static_cast<usize>(y) * st.poolWidth + x];
    return true;
}

FUSE_HOST_DEVICE inline void dir_pcf(const VsmFrameConstants& c, const ShadowStore& st, u32 level, s32 ix, s32 iy, s32 r, f32 rd,
                                     TapAcc& a) {
    for (s32 dy = -r; dy <= r; ++dy) {
        for (s32 dx = -r; dx <= r; ++dx) {
            u32 bits = 0;
            if (dir_fetch(c, st, level, ix + dx, iy + dy, bits)) {
                tap_compare(a, rd, bits);
            }
        }
    }
}

/// Directional visibility of a world receiver (see the header comment). forceLevel >= 0 samples
/// that level only (visibility -1 when its page is unmapped).
FUSE_HOST_DEVICE inline SampleResult dir_visibility(const VsmFrameConstants& c, const VsmShadowConstants& s, const ShadowStore& st,
                                                    const f32 pos[3], const f32 n[3], s32 forceLevel) {
    SampleResult r{};
    f32 lp[3];
    light_point(c, pos, lp);
    if (!(abs_f(lp[0]) <= kMaxLightCoord) || !(abs_f(lp[1]) <= kMaxLightCoord) || !(abs_f(lp[2]) <= kMaxLightCoord)) {
        return r;
    }
    s32 first = forceLevel;
    if (first < 0) {
        const f32 dx = abs_f(lp[0] - c.cameraLight[0]);
        const f32 dy = abs_f(lp[1] - c.cameraLight[1]);
        const f32 dz = abs_f(lp[2] - c.cameraLight[2]);
        const f32 d = max_f(max_f(dx, dy), dz);
        const f32 qc = d * c.levelSelectScale;
        const f32 qd = d * c.densityScale;
        const s32 lc = qc >= 1.f ? static_cast<s32>((bits_of(qc) >> 23u) & 0xFFu) - 127 + 1 : 0;
        const s32 ld = (qd >= 1.f ? static_cast<s32>((bits_of(qd) >> 23u) & 0xFFu) - 127 + 1 : 0) + c.lodBias;
        first = max_s(lc, ld);
    }
    const s32 levels = static_cast<s32>(c.levels < kMaxLevels ? c.levels : kMaxLevels);
    if (first >= levels) {
        if (forceLevel >= 0) {
            r.visibility = -1.f;
        }
        return r;
    }
    // Levels tried: the selected one, one finer, then coarser ones (the marking's level can differ by
    // one from the receiver's where the two reconstruct the position differently).
    const s32 tries = forceLevel >= 0 ? 1 : levels - first + 1;
    for (s32 i = 0; i < tries; ++i) {
        const s32 lev = i == 0 ? first : (i == 1 ? first - 1 : first + i - 1);
        if (lev < 0) {
            continue;
        }
        const VsmLevelConstants& L = c.level[lev];
        const f32 texel = L.pageWorld * (1.f / static_cast<f32>(kPageTexels));
        const f32 off = s.normalOffset * texel;
        const f32 bp[3] = {pos[0] + n[0] * off, pos[1] + n[1] * off, pos[2] + n[2] * off};
        f32 bl[3];
        light_point(c, bp, bl);
        const f32 u = bl[0] * L.invPageWorld;
        const f32 v = bl[1] * L.invPageWorld;
        if (!(abs_f(u) < kMaxPageCoord) || !(abs_f(v) < kMaxPageCoord)) {
            return r;
        }
        const s32 half = static_cast<s32>(kPagesPerAxis / 2u);
        const f32 tu = (u - static_cast<f32>(L.originX - half)) * static_cast<f32>(kPageTexels);
        const f32 tv = (v - static_cast<f32>(L.originY - half)) * static_cast<f32>(kPageTexels);
        const f32 limit = static_cast<f32>(kWindowTexels);
        u32 bits = 0;
        const bool inside = tu >= 0.f && tu < limit && tv >= 0.f && tv < limit;
        const s32 ix = inside ? static_cast<s32>(std::floor(tu)) : -1;
        const s32 iy = inside ? static_cast<s32>(std::floor(tv)) : -1;
        if (!inside || !dir_fetch(c, st, static_cast<u32>(lev), ix, iy, bits)) {
            if (forceLevel >= 0) {
                r.visibility = -1.f;
                r.level = lev;
                return r;
            }
            continue;
        }
        const f32 rd = dir_depth(L, bl[2]) - s.depthBias * kDirDepthPerTexel;
        r.level = lev;
        r.receiverDepth = rd;
        TapAcc a{};
        s32 radius = 0;
        if (s.filterMode == kFilterPcf) {
            radius = static_cast<s32>(min_u(s.pcfRadius, kMaxFilterRadius));
        } else if (s.filterMode == kFilterPcss) {
            const u32 search = min_u(s.pcssMaxRadius, kMaxFilterRadius);
            for (u32 j = 0; j < kPcssBlockerGrid; ++j) {
                for (u32 i = 0; i < kPcssBlockerGrid; ++i) {
                    u32 b = 0;
                    if (dir_fetch(c, st, static_cast<u32>(lev), ix + blocker_offset(i, search), iy + blocker_offset(j, search), b)) {
                        tap_blocker(a, rd, b);
                    }
                }
            }
            if (!(a.blockers > 0.f)) {
                r.visibility = 1.f;
                r.margin = a.margin;
                return r;
            }
            const f32 avg = a.blockerSum / a.blockers;
            const f32 texels = ((rd - avg) * (1.f / kDirDepthPerTexel)) * s.sunTanAngle;
            radius = static_cast<s32>(penumbra_radius(texels, search));
        }
        dir_pcf(c, st, static_cast<u32>(lev), ix, iy, radius, rd, a);
        r.visibility = a.lit / a.taps;
        r.margin = a.margin;
        return r;
    }
    return r;
}

FUSE_HOST_DEVICE inline u32 local_fetch(const VsmShadowConstants& s, const ShadowStore& st, u32 page, s32 ix, s32 iy) {
    const s32 top = static_cast<s32>(kLocalPageTexels) - 1;
    const u32 x = static_cast<u32>(ix < 0 ? 0 : (ix > top ? top : ix));
    const u32 y = static_cast<u32>(iy < 0 ? 0 : (iy > top ? top : iy));
    const u32 px = (page % s.localPagesX) * kLocalPageTexels + x;
    const u32 py = (page / s.localPagesX) * kLocalPageTexels + y;
    return st.local[static_cast<usize>(py) * st.localWidth + px];
}

/// Local-light visibility of a world receiver.
FUSE_HOST_DEVICE inline SampleResult local_visibility(const VsmShadowConstants& s, const VsmLocalLight& e, const ShadowStore& st,
                                                      const f32 pos[3], const f32 n[3]) {
    SampleResult r{};
    f32 f[3], rt[3], up[3], v[3];
    const f32 rel0[3] = {pos[0] - e.position[0], pos[1] - e.position[1], pos[2] - e.position[2]};
    u32 face = e.type == kLocalPoint ? cube_face(rel0) : 0u;
    local_basis(e, face, f, rt, up);
    local_view(e, f, rt, up, pos, v);
    if (!(v[2] > e.nearPlane)) {
        return r;
    }
    const f32 texel = (v[2] * (1.f / 64.f)) / e.invTanHalf;
    const f32 off = s.normalOffset * texel;
    const f32 bp[3] = {pos[0] + n[0] * off, pos[1] + n[1] * off, pos[2] + n[2] * off};
    if (e.type == kLocalPoint) {
        const f32 rel[3] = {bp[0] - e.position[0], bp[1] - e.position[1], bp[2] - e.position[2]};
        face = cube_face(rel);
        local_basis(e, face, f, rt, up);
    }
    local_view(e, f, rt, up, bp, v);
    if (!(v[2] > e.nearPlane)) {
        return r;
    }
    f32 tx, ty, iz;
    local_project(e, v, tx, ty, iz);
    const f32 limit = static_cast<f32>(kLocalPageTexels);
    if (!(tx >= 0.f && tx < limit && ty >= 0.f && ty < limit)) {
        if (e.type != kLocalPoint) {
            return r; // outside the spot frustum (and so outside the cone)
        }
        tx = tx < 0.f ? 0.f : (tx < limit ? tx : limit - 1.f);
        ty = ty < 0.f ? 0.f : (ty < limit ? ty : limit - 1.f);
    }
    const s32 ix = static_cast<s32>(std::floor(tx));
    const s32 iy = static_cast<s32>(std::floor(ty));
    const u32 page = e.page[face < kCubeFaces ? face : 0u];
    const f32 rd = v[2] * e.invRange - (s.depthBias * texel) * e.invRange;
    r.receiverDepth = rd;
    TapAcc a{};
    s32 radius = 0;
    if (s.filterMode == kFilterPcf) {
        radius = static_cast<s32>(min_u(s.pcfRadius, kMaxFilterRadius));
    } else if (s.filterMode == kFilterPcss) {
        const u32 search = min_u(s.pcssMaxRadius, kMaxFilterRadius);
        for (u32 j = 0; j < kPcssBlockerGrid; ++j) {
            for (u32 i = 0; i < kPcssBlockerGrid; ++i) {
                tap_blocker(a, rd, local_fetch(s, st, page, ix + blocker_offset(i, search), iy + blocker_offset(j, search)));
            }
        }
        if (!(a.blockers > 0.f)) {
            r.margin = a.margin;
            return r;
        }
        const f32 zb = (a.blockerSum / a.blockers) * e.range;
        const f32 zr = v[2];
        const f32 world = ((zr - zb) / max_f(zb, e.nearPlane)) * e.lightSize;
        const f32 texels = (world * e.invTanHalf) * 64.f / zr;
        radius = static_cast<s32>(penumbra_radius(texels, search));
    }
    for (s32 dy = -radius; dy <= radius; ++dy) {
        for (s32 dx = -radius; dx <= radius; ++dx) {
            tap_compare(a, rd, local_fetch(s, st, page, ix + dx, iy + dy));
        }
    }
    r.visibility = a.lit / a.taps;
    r.margin = a.margin;
    return r;
}

/// The lookup light.shade / the forward pass make per light: 1 for a light without a shadow.
FUSE_HOST_DEVICE inline SampleResult shadow_visibility(const VsmShadowConstants& s, const VsmFrameConstants* c, const ShadowStore& st,
                                                       u32 slot, const f32 pos[3], const f32 n[3]) {
    if (slot == s.directionalSlot && c != nullptr && s.vsm != 0u) {
        return dir_visibility(*c, s, st, pos, n, -1);
    }
    const u32 count = min_u(s.localCount, kMaxLocalLights);
    for (u32 k = 0; k < count; ++k) {
        if (s.local[k].slot == slot) {
            return local_visibility(s, s.local[k], st, pos, n);
        }
    }
    return SampleResult{};
}

} // namespace fuse::renderer::vsm::raster_math
