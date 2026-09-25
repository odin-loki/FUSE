#pragma once

// WP-8.1 froxel fog: the CPU reference of every GPU pass, one function per GPU thread
// (shaders/volumetric/fog_*.{comp,slang} are line-for-line twins: same f32 expressions in the same order,
// `precise` / -fp-mode precise so nothing is contracted). The oracles it builds on:
//   * the global medium is the B5 exponential height fog, sample_volumetric_fog_density
//     (volumetric/volumetric_fog.hpp): density x exp(-falloff x max(0, y - base)), same expression;
//   * the slice distribution is the B5 FroxelSliceLayout / clustered slice_near_z one,
//     near x (far / near)^(s / Z) (host-computed table, so both sides read identical depths);
//   * lights are the WP-2.1 records and rules: the directional list, then the cluster's list (ascending
//     slot), point falloff / spot cone of clustered_gpu_kernel.hpp, the cluster of a sample from
//     clustered_kernel::map_screen_depth_to_cluster; shadows through a hook (the GPU calls WP-3.2's
//     fuse_vsm_shadow with a zero normal).
//
// Model (per froxel):
//   inject     sample p at the froxel's jittered point; extinction sigma_t = height fog + local volumes,
//              scattering sigma_s = albedo x sigma_t (per source); in-scattered radiance
//              L = ambient + sum_lights colour x intensity x attenuation x visibility x HG(g, l . v);
//              current = (sigma_s x L, sigma_t)
//   temporal   history = lerp(reprojected previous history (trilinear, clamp to edge), current, alpha);
//              the current sample alone where the froxel centre was outside the previous volume
//   integrate  per column, front to back from the camera (slice 0 starts at depth 0): step length
//              ds = slice thickness x |ray| (the column centre's view ray per unit depth), energy-conserving
//              step (Hillaire 2015): S_int = S (1 - e^{-sigma ds}) / sigma, accum += T S_int, T *= e^{-sigma ds};
//              stored at the slice's FAR boundary
//   apply      the pixel's view depth -> piecewise-linear lookup between slice boundaries (depth 0 = (0, 1)),
//              bilinear across the four nearest columns; out = lit x T + in-scattering
#include <fuse/math/vec.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/lighting/clustered_kernel.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_types.hpp>
#include <fuse/renderer/volumetric/gpu/froxel_fog_types.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::volumetric_gpu::fog_kernel {

using math::Vec3;
using math::Vec4;

inline constexpr f32 kInv4Pi = 0.0795774715459477f;
inline constexpr f32 kMaxAnisotropy = 0.99f;
/// Below this optical depth a step uses the series of (1 - e^-x) / x (7 terms, truncation < 3e-12): 1 - e^-x
/// cancels, and the device exp's few-ulp error would be amplified by 1 / x.
inline constexpr f32 kSeriesOpticalDepth = 0.1f;

/// Light sources of the inject pass (the WP-2.1 frame: camera / grid / light count read from the
/// LightingFrameConstants fields the GPU reads; lists as the GPU buffers hold them).
struct LightView {
    const lighting_gpu::LightingFrameConstants* frame = nullptr; ///< null = ambient only
    const gpu_scene::GpuLight* lights = nullptr;                 ///< scene light table (slots)
    const u32* directional = nullptr;                            ///< directional slots
    u32 directionalCount = 0;
    const u32* grid = nullptr;      ///< (offset, count) per cluster (u32 pairs, == ClusterGridEntry)
    const u32* lightList = nullptr; ///< flat list of slots
    /// WP-3.2 shadows: visibility of light `slot` at world point `p` (GPU: fuse_vsm_shadow(address, slot,
    /// p, 0)). Called only with kFogFlagShadows; null = 1.
    f32 (*shadow)(const void* user, u32 slot, const Vec3& p) = nullptr;
    const void* shadowUser = nullptr;
};

// --- small helpers (the shader twins: fog_common.{glsl,slang}) ---------------------------------------
inline Vec3 v3(const f32 (&a)[3]) { return {a[0], a[1], a[2]}; }
inline f32 dot3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline f32 lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
inline Vec4 lerp4(const Vec4& a, const Vec4& b, f32 t) {
    return {lerpf(a.x, b.x, t), lerpf(a.y, b.y, t), lerpf(a.z, b.z, t), lerpf(a.w, b.w, t)};
}
inline f32 clampf(f32 v, f32 lo, f32 hi) { return std::min(std::max(v, lo), hi); }

inline Vec3 safe_normalize(const Vec3& v, const Vec3& fallback) {
    const f32 len2 = dot3(v, v);
    if (!(len2 > 0.f) || !std::isfinite(len2)) {
        return fallback;
    }
    const f32 inv = 1.f / std::sqrt(len2);
    return {v.x * inv, v.y * inv, v.z * inv};
}

inline u32 froxel_index(const FogFrameConstants& c, u32 x, u32 y, u32 z) { return (y * c.gridX + x) * c.gridZ + z; }

/// View-space point (clustered_kernel::view_position_from_screen) -> world (view_to_world), fog camera.
inline Vec3 fog_world(const FogFrameConstants& c, f32 sx, f32 sy, f32 depth) {
    const f32 nx = sx * 2.f - 1.f;
    const f32 ny = 1.f - sy * 2.f;
    const f32 vx = nx * c.tanX * depth;
    const f32 vy = ny * c.tanY * depth;
    const f32 vz = -depth;
    return {c.position[0] + c.right[0] * vx + c.up[0] * vy + c.back[0] * vz,
            c.position[1] + c.right[1] * vx + c.up[1] * vy + c.back[1] * vz,
            c.position[2] + c.right[2] * vx + c.up[2] * vy + c.back[2] * vz};
}

/// Slice s with sliceDepth[s] <= d < sliceDepth[s + 1] (binary search; 0 below, gridZ - 1 above).
inline u32 find_slice(const FogFrameConstants& c, f32 d) {
    u32 lo = 0u;
    u32 hi = c.gridZ;
    while (hi - lo > 1u) {
        const u32 mid = (lo + hi) / 2u;
        if (d >= c.sliceDepth[mid]) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// --- medium ------------------------------------------------------------------------------------------
inline f32 volume_weight(const FogVolume& v, const Vec3& p) {
    const f32 dx = p.x - v.center[0];
    const f32 dy = p.y - v.center[1];
    const f32 dz = p.z - v.center[2];
    f32 d = 0.f;
    if (v.shape == kFogVolumeBox) {
        d = std::max(std::max(std::fabs(dx) / v.halfExtent[0], std::fabs(dy) / v.halfExtent[1]),
                     std::fabs(dz) / v.halfExtent[2]);
    } else {
        d = std::sqrt(dx * dx + dy * dy + dz * dz) / v.halfExtent[0];
    }
    if (!(d < 1.f)) {
        return 0.f;
    }
    return std::min((1.f - d) / std::max(v.edge, 1e-4f), 1.f);
}

/// Extinction at p; `scattering` = the albedo-weighted scattering coefficient.
inline f32 medium(const FogFrameConstants& c, const Vec3& p, Vec3& scattering) {
    const f32 heightDelta = p.y - c.baseHeight;
    // sample_volumetric_fog_density (B5 oracle): density * exp(-falloff * max(0, y - base)).
    const f32 fog = c.density * std::exp(-c.heightFalloff * std::max(0.f, heightDelta));
    f32 extinction = fog;
    Vec3 s{c.albedo[0] * fog, c.albedo[1] * fog, c.albedo[2] * fog};
    for (u32 i = 0; i < std::min(c.volumeCount, kFogMaxVolumes); ++i) {
        const FogVolume& v = c.volumes[i];
        const f32 w = volume_weight(v, p);
        if (w > 0.f) {
            const f32 e = v.density * w;
            extinction = extinction + e;
            s = Vec3{s.x + v.albedo[0] * e, s.y + v.albedo[1] * e, s.z + v.albedo[2] * e};
        }
    }
    scattering = s;
    return extinction;
}

// --- lighting ----------------------------------------------------------------------------------------
/// Henyey-Greenstein phase of the angle between the direction to the light and the view ray.
inline f32 phase_hg(f32 g, f32 cosTheta) {
    const f32 g2 = g * g;
    const f32 denom = std::max(1.f + g2 - 2.f * g * cosTheta, 1e-6f);
    return (1.f - g2) * kInv4Pi / (denom * std::sqrt(denom));
}

inline f32 point_falloff(f32 d, f32 radius) {
    if (!(radius > 0.f) || !(d < radius)) {
        return 0.f;
    }
    const f32 ratio = d / radius;
    const f32 ratio2 = ratio * ratio;
    const f32 w = clampf(1.f - ratio2 * ratio2, 0.f, 1.f);
    return (w * w) / std::max(d * d, 1e-4f);
}

inline f32 spot_cone(f32 cosAngle, f32 cosInner, f32 cosOuter) {
    const f32 t = clampf((cosAngle - cosOuter) / std::max(cosInner - cosOuter, 1e-4f), 0.f, 1.f);
    return t * t;
}

/// One light's in-scattered radiance at p towards the camera (v = unit camera -> p), phase included.
inline Vec3 light_in(const FogFrameConstants& c, const LightView& lv, u32 slot, const Vec3& p, const Vec3& v) {
    const gpu_scene::GpuLight& light = lv.lights[slot];
    Vec3 l{};
    f32 attenuation = 1.f;
    if (light.type == static_cast<u32>(gpu_scene::GpuLightType::Directional)) {
        l = safe_normalize(Vec3{-light.direction[0], -light.direction[1], -light.direction[2]}, Vec3{0.f, 0.f, 1.f});
    } else if (light.type == static_cast<u32>(gpu_scene::GpuLightType::Point) ||
               light.type == static_cast<u32>(gpu_scene::GpuLightType::Spot)) {
        const Vec3 toLight{light.position[0] - p.x, light.position[1] - p.y, light.position[2] - p.z};
        const f32 distance = std::sqrt(dot3(toLight, toLight));
        attenuation = point_falloff(distance, light.range);
        if (attenuation == 0.f) {
            return {};
        }
        const f32 inv = 1.f / std::max(distance, 1e-6f);
        l = Vec3{toLight.x * inv, toLight.y * inv, toLight.z * inv};
        if (light.type == static_cast<u32>(gpu_scene::GpuLightType::Spot)) {
            const Vec3 axis = safe_normalize(v3(light.direction), Vec3{0.f, 0.f, -1.f});
            attenuation = attenuation * spot_cone(-dot3(l, axis), light.cosInner, light.cosOuter);
            if (attenuation == 0.f) {
                return {};
            }
        }
    } else {
        return {}; // free slots, WP-2.2 area lights (not scattered by the fog yet)
    }
    f32 visibility = 1.f;
    if ((c.flags & kFogFlagShadows) != 0u && lv.shadow != nullptr) {
        visibility = lv.shadow(lv.shadowUser, slot, p);
    }
    const f32 ph = phase_hg(c.anisotropy, dot3(l, v));
    const f32 s = light.intensity * attenuation * visibility * ph;
    return {light.color[0] * s, light.color[1] * s, light.color[2] * s};
}

/// The WP-2.1 cluster of a world point (lighting camera): false outside the clustered range.
inline bool light_cluster(const lighting_gpu::LightingFrameConstants& f, const Vec3& p, u32& cluster) {
    const Vec3 rel{p.x - f.cameraPosition[0], p.y - f.cameraPosition[1], p.z - f.cameraPosition[2]};
    const f32 vx = dot3(rel, v3(f.right));
    const f32 vy = dot3(rel, v3(f.up));
    const f32 depth = -dot3(rel, v3(f.back));
    if (!(depth > 0.f)) {
        return false;
    }
    const f32 ndcX = vx / (f.tanX * depth);
    const f32 ndcY = vy / (f.tanY * depth);
    const f32 sx = (ndcX + 1.f) * 0.5f;
    const f32 sy = (1.f - ndcY) * 0.5f;
    const clustered_kernel::GridDims g{f.tilesX, f.tilesY, f.slicesZ, f.capacity};
    return clustered_kernel::map_screen_depth_to_cluster(sx, sy, depth, g, f.nearPlane, f.farPlane, cluster);
}

/// ambient + the directional list + the cluster's lights (ascending slot), each x phase x visibility.
inline Vec3 in_scattering(const FogFrameConstants& c, const LightView& lv, const Vec3& p, const Vec3& v) {
    Vec3 L{c.ambient[0], c.ambient[1], c.ambient[2]};
    if ((c.flags & kFogFlagLights) == 0u || lv.frame == nullptr) {
        return L;
    }
    const lighting_gpu::LightingFrameConstants& f = *lv.frame;
    for (u32 i = 0; i < lv.directionalCount; ++i) {
        const u32 slot = lv.directional[i];
        if (slot < f.lightCount) {
            const Vec3 add = light_in(c, lv, slot, p, v);
            L = Vec3{L.x + add.x, L.y + add.y, L.z + add.z};
        }
    }
    u32 cluster = 0;
    if (lv.grid != nullptr && light_cluster(f, p, cluster)) {
        const u32 offset = lv.grid[cluster * 2u];
        const u32 count = lv.grid[cluster * 2u + 1u];
        for (u32 i = 0; i < count; ++i) {
            const u32 slot = lv.lightList[offset + i];
            if (slot < f.lightCount) {
                const Vec3 add = light_in(c, lv, slot, p, v);
                L = Vec3{L.x + add.x, L.y + add.y, L.z + add.z};
            }
        }
    }
    return L;
}

/// Scattering x radiance and extinction of the medium at world point p.
inline Vec4 inject_point(const FogFrameConstants& c, const LightView& lv, const Vec3& p) {
    Vec3 scattering{};
    const f32 extinction = medium(c, p, scattering);
    if (!(extinction > 0.f)) { // densities are >= 0 (the host clamps them): no medium, no scattering
        return {0.f, 0.f, 0.f, 0.f};
    }
    const Vec3 rel{p.x - c.position[0], p.y - c.position[1], p.z - c.position[2]};
    const Vec3 v = safe_normalize(rel, Vec3{-c.back[0], -c.back[1], -c.back[2]});
    const Vec3 L = in_scattering(c, lv, p, v);
    return {scattering.x * L.x, scattering.y * L.y, scattering.z * L.z, extinction};
}

/// Sample point of froxel (x, y, z) at offset (jx, jy, jz) in [0, 1)^3.
inline Vec3 froxel_point(const FogFrameConstants& c, u32 x, u32 y, u32 z, f32 jx, f32 jy, f32 jz) {
    const f32 sx = (static_cast<f32>(x) + jx) * c.invGridX;
    const f32 sy = (static_cast<f32>(y) + jy) * c.invGridY;
    const f32 z0 = c.sliceDepth[z];
    const f32 z1 = c.sliceDepth[z + 1u];
    const f32 depth = z0 + (z1 - z0) * jz;
    return fog_world(c, sx, sy, depth);
}

/// fog.inject: one froxel at this frame's jitter.
inline Vec4 inject_froxel(const FogFrameConstants& c, const LightView& lv, u32 x, u32 y, u32 z) {
    return inject_point(c, lv, froxel_point(c, x, y, z, c.jitter[0], c.jitter[1], c.jitter[2]));
}

// --- temporal ------------------------------------------------------------------------------------------
/// Linear interpolation along z of column (x, y) then bilinear across columns (clamp to edge).
inline Vec4 sample_grid(const FogFrameConstants& c, const Vec4* buf, f32 gx, f32 gy, f32 gz) {
    const f32 cx = clampf(gx, 0.f, static_cast<f32>(c.gridX - 1u));
    const f32 cy = clampf(gy, 0.f, static_cast<f32>(c.gridY - 1u));
    const f32 cz = clampf(gz, 0.f, static_cast<f32>(c.gridZ - 1u));
    const u32 x0 = static_cast<u32>(cx);
    const u32 y0 = static_cast<u32>(cy);
    const u32 z0 = static_cast<u32>(cz);
    const u32 x1 = std::min(x0 + 1u, c.gridX - 1u);
    const u32 y1 = std::min(y0 + 1u, c.gridY - 1u);
    const u32 z1 = std::min(z0 + 1u, c.gridZ - 1u);
    const f32 wx = cx - static_cast<f32>(x0);
    const f32 wy = cy - static_cast<f32>(y0);
    const f32 wz = cz - static_cast<f32>(z0);
    const Vec4 c00 = lerp4(buf[froxel_index(c, x0, y0, z0)], buf[froxel_index(c, x0, y0, z1)], wz);
    const Vec4 c10 = lerp4(buf[froxel_index(c, x1, y0, z0)], buf[froxel_index(c, x1, y0, z1)], wz);
    const Vec4 c01 = lerp4(buf[froxel_index(c, x0, y1, z0)], buf[froxel_index(c, x0, y1, z1)], wz);
    const Vec4 c11 = lerp4(buf[froxel_index(c, x1, y1, z0)], buf[froxel_index(c, x1, y1, z1)], wz);
    return lerp4(lerp4(c00, c10, wx), lerp4(c01, c11, wx), wy);
}

/// Continuous grid coordinates (froxel centres at integers) of world point p in the previous camera's
/// volume; false outside it.
inline bool reproject(const FogFrameConstants& c, const Vec3& p, f32& gx, f32& gy, f32& gz) {
    const Vec3 rel{p.x - c.prevPosition[0], p.y - c.prevPosition[1], p.z - c.prevPosition[2]};
    const f32 vx = dot3(rel, v3(c.prevRight));
    const f32 vy = dot3(rel, v3(c.prevUp));
    const f32 depth = -dot3(rel, v3(c.prevBack));
    if (!(depth >= c.nearPlane) || !(depth <= c.farPlane)) {
        return false;
    }
    const f32 ndcX = vx / (c.prevTanX * depth);
    const f32 ndcY = vy / (c.prevTanY * depth);
    const f32 sx = (ndcX + 1.f) * 0.5f;
    const f32 sy = (1.f - ndcY) * 0.5f;
    if (!(sx >= 0.f) || !(sx <= 1.f) || !(sy >= 0.f) || !(sy <= 1.f)) {
        return false;
    }
    const u32 s = find_slice(c, depth);
    const f32 z0 = c.sliceDepth[s];
    const f32 z1 = c.sliceDepth[s + 1u];
    gx = sx * static_cast<f32>(c.gridX) - 0.5f;
    gy = sy * static_cast<f32>(c.gridY) - 0.5f;
    gz = static_cast<f32>(s) + (depth - z0) / (z1 - z0) - 0.5f;
    return true;
}

/// fog.temporal: the blended history of froxel (x, y, z).
inline Vec4 temporal_froxel(const FogFrameConstants& c, const Vec4* current, const Vec4* history, u32 x, u32 y, u32 z) {
    const u32 i = froxel_index(c, x, y, z);
    const Vec4 cur = current[i];
    if ((c.flags & kFogFlagHistory) == 0u) {
        return cur;
    }
    Vec4 h = history[i];
    if ((c.flags & kFogFlagReproject) != 0u) {
        const Vec3 p = froxel_point(c, x, y, z, 0.5f, 0.5f, 0.5f);
        f32 gx = 0.f;
        f32 gy = 0.f;
        f32 gz = 0.f;
        if (!reproject(c, p, gx, gy, gz)) {
            return cur;
        }
        h = sample_grid(c, history, gx, gy, gz);
    }
    return lerp4(h, cur, c.temporalAlpha);
}

// --- integration -----------------------------------------------------------------------------------
/// |view ray| per unit view depth at the centre of column (x, y).
inline f32 column_ray_scale(const FogFrameConstants& c, u32 x, u32 y) {
    const f32 sx = (static_cast<f32>(x) + 0.5f) * c.invGridX;
    const f32 sy = (static_cast<f32>(y) + 0.5f) * c.invGridY;
    const f32 rx = (sx * 2.f - 1.f) * c.tanX;
    const f32 ry = (1.f - sy * 2.f) * c.tanY;
    return std::sqrt(rx * rx + ry * ry + 1.f);
}

/// (1 - e^{-sigma ds}) / sigma, exact for sigma -> 0.
inline f32 step_weight(f32 sigma, f32 ds, f32 stepT) {
    const f32 x = sigma * ds;
    if (x < kSeriesOpticalDepth) {
        return ds * (1.f - x * (0.5f - x * (1.f / 6.f - x * (1.f / 24.f - x * (1.f / 120.f - x * (1.f / 720.f - x * (1.f / 5040.f)))))));
    }
    return (1.f - stepT) / sigma;
}

/// fog.integrate: column (x, y) front to back; `out` = (in-scattering, transmittance) at each slice's far
/// boundary.
inline void integrate_column(const FogFrameConstants& c, const Vec4* froxels, Vec4* out, u32 x, u32 y) {
    const f32 scale = column_ray_scale(c, x, y);
    f32 ar = 0.f;
    f32 ag = 0.f;
    f32 ab = 0.f;
    f32 t = 1.f;
    for (u32 z = 0; z < c.gridZ; ++z) {
        const u32 i = froxel_index(c, x, y, z);
        const Vec4 s = froxels[i];
        const f32 start = z == 0u ? 0.f : c.sliceDepth[z];
        const f32 ds = (c.sliceDepth[z + 1u] - start) * scale;
        const f32 sigma = std::max(s.w, 0.f);
        const f32 stepT = std::exp(-sigma * ds);
        const f32 k = step_weight(sigma, ds, stepT);
        ar = ar + t * (s.x * k);
        ag = ag + t * (s.y * k);
        ab = ab + t * (s.z * k);
        t = t * stepT;
        out[i] = Vec4{ar, ag, ab, t};
    }
}

// --- apply -----------------------------------------------------------------------------------------
/// Value of column (x, y) at view depth d: linear between slice-far boundaries, (0, 0, 0, 1) at depth 0.
inline Vec4 column_at(const FogFrameConstants& c, const Vec4* integrated, u32 x, u32 y, u32 k, f32 f) {
    const Vec4 hi = integrated[froxel_index(c, x, y, k)];
    const Vec4 lo = k == 0u ? Vec4{0.f, 0.f, 0.f, 1.f} : integrated[froxel_index(c, x, y, k - 1u)];
    return lerp4(lo, hi, f);
}

/// The apply helper: (in-scattering, transmittance) between the camera and view depth `depth` along the
/// ray through screen point (sx, sy) (bilinear over columns, piecewise linear in depth).
inline Vec4 sample_integrated(const FogFrameConstants& c, const Vec4* integrated, f32 sx, f32 sy, f32 depth) {
    const f32 d = clampf(depth, 0.f, c.farPlane);
    const u32 k = find_slice(c, d);
    const f32 lo = k == 0u ? 0.f : c.sliceDepth[k];
    const f32 f = clampf((d - lo) / (c.sliceDepth[k + 1u] - lo), 0.f, 1.f);
    const f32 cx = clampf(sx * static_cast<f32>(c.gridX) - 0.5f, 0.f, static_cast<f32>(c.gridX - 1u));
    const f32 cy = clampf(sy * static_cast<f32>(c.gridY) - 0.5f, 0.f, static_cast<f32>(c.gridY - 1u));
    const u32 x0 = static_cast<u32>(cx);
    const u32 y0 = static_cast<u32>(cy);
    const u32 x1 = std::min(x0 + 1u, c.gridX - 1u);
    const u32 y1 = std::min(y0 + 1u, c.gridY - 1u);
    const f32 wx = cx - static_cast<f32>(x0);
    const f32 wy = cy - static_cast<f32>(y0);
    const Vec4 c00 = column_at(c, integrated, x0, y0, k, f);
    const Vec4 c10 = column_at(c, integrated, x1, y0, k, f);
    const Vec4 c01 = column_at(c, integrated, x0, y1, k, f);
    const Vec4 c11 = column_at(c, integrated, x1, y1, k, f);
    return lerp4(lerp4(c00, c10, wx), lerp4(c01, c11, wx), wy);
}

/// RT4 device depth -> view depth (clustered_kernel::view_depth_from_device_depth); 0 = sky.
inline f32 view_depth(const FogFrameConstants& c, f32 d) {
    if ((c.flags & kFogFlagReversedZ) != 0u) {
        return d > 0.f ? c.depthNear / d : 0.f;
    }
    if (d >= 1.f || c.depthFar <= c.depthNear) {
        return 0.f;
    }
    return (c.depthNear * c.depthFar) / (c.depthFar - d * (c.depthFar - c.depthNear));
}

/// fog.apply: pixel (px, py) of the lit image with device depth `deviceDepth`.
inline Vec4 apply_pixel(const FogFrameConstants& c, const Vec4* integrated, u32 px, u32 py, f32 deviceDepth, const Vec4& lit) {
    f32 depth = view_depth(c, deviceDepth);
    if (!(depth > 0.f) || !std::isfinite(depth)) {
        depth = c.farPlane; // sky: the whole fog range
    }
    const f32 sx = (static_cast<f32>(px) + 0.5f) * c.invWidth;
    const f32 sy = (static_cast<f32>(py) + 0.5f) * c.invHeight;
    const Vec4 fog = sample_integrated(c, integrated, sx, sy, depth);
    return {lit.x * fog.w + fog.x, lit.y * fog.w + fog.y, lit.z * fog.w + fog.z, lit.w};
}

} // namespace fuse::renderer::volumetric_gpu::fog_kernel
