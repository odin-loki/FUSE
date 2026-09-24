#pragma once

// WP-2.1 GPU clustered deferred shading: the single-source CPU reference (docs/compute-kernels.md) of
// the "light.shade" kernel (shaders/lighting/lc_shade.{comp,slang}, math twins lc_common.{glsl,slang}).
//
// Per pixel of the WP-1.5 G-buffer (GBufferAttachment layout, write_gbuffer packing):
//   RT4 device depth -> linear view depth -> world position and cluster (the B5 oracle's own
//   reconstruction and cluster mapping: clustered_kernel::view_depth_from_device_depth,
//   map_screen_depth_to_cluster, view_position_from_screen, view_to_world); sky / out-of-range pixels
//   are (0, 0, 0, 0);
//   RT0 signed-octahedral normal + AO, RT1 albedo, RT2 roughness / metallic, RT5 emissive radiance;
//   radiance = emissive + ambient * albedo * AO
//            + sum over the directional list (ascending slot)
//            + sum over the pixel's cluster list (ascending slot; point and spot lights)
//   of the Cook-Torrance BRDF of shaders/common/brdf.glsl (GGX D, height-correlated Smith V,
//   Schlick F with the fifth power as multiplies) x colour x intensity x attenuation x N.L;
//   attenuation: point = the oracle's windowed inverse-square falloff (clustered_kernel::
//   point_light_falloff, exactly 0 outside the range sphere, so a light outside a pixel's cluster list
//   contributes nothing); spot = that x smooth cone t^2, t = saturate((cos - cosOuter) /
//   max(cosInner - cosOuter, 1e-4)); directional = 1 (L = -direction). Alpha = 1 on shaded pixels.
//
// Every shading model uses this lobe for now (per-model lobes are a later Phase-2 package). The
// kernels evaluate the same expressions in the same order in f32.
//
// WP-2.2: with a BRDF LUT (ShadeParams::brdf_lut, LightingFrameConstants::brdfLut != 0; ltc_kernel.hpp)
// the lobe is the multi-scatter compensated BRDF (brdf::multi_scatter_cos: Fdez-Aguera compensation,
// diffuse weighted by 1 - E_spec, exact metallic blend), and two more light types shade: rectangle and
// disk area lights (LTC, x the area window of the distance to the centre, 0 at d >= range), and a
// directional light with an angular radius (cosOuter < ltc::kMinSunCos) shades as a disk at infinity.
// Without the LUT the WP-2.1 lobe above is used unchanged and area lights contribute nothing.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/lighting/clustered_kernel.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_types.hpp>
#include <fuse/renderer/lighting/ltc/ltc_kernel.hpp>
#include <fuse/renderer/material/brdf.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::lighting_gpu {

inline constexpr const char* kShadeName = "light.shade";
inline constexpr kernel::Dim3 kShadeWorkgroup{kShadeTile, kShadeTile, 1u};

inline constexpr f32 kPi = 3.14159265358979f;
inline constexpr f32 kMinRoughness = 0.045f;
inline constexpr f32 kSpotMinWidth = 1e-4f;

/// One surface sample decoded from the G-buffer.
struct SurfaceSample {
    math::Vec3 position{};
    math::Vec3 normal{0.f, 0.f, 1.f};
    math::Vec3 albedo{1.f, 1.f, 1.f};
    f32 roughness = 0.5f;
    f32 metallic = 0.f;
    f32 ao = 1.f;
    math::Vec3 emissive{};
};

FUSE_HOST_DEVICE inline f32 saturate(f32 v) { return std::min(std::max(v, 0.f), 1.f); }

FUSE_HOST_DEVICE inline math::Vec3 vmul(const math::Vec3& a, const math::Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

/// Unit vector; `fallback` when the length is 0 or not finite.
FUSE_HOST_DEVICE inline math::Vec3 safe_normalize(const math::Vec3& v, const math::Vec3& fallback) {
    const f32 len2 = v.dot(v);
    if (!(len2 > 0.f) || !std::isfinite(len2)) {
        return fallback;
    }
    const f32 inv = 1.f / std::sqrt(len2);
    return {v.x * inv, v.y * inv, v.z * inv};
}

/// Inverse of the G-buffer's signed octahedral normal (oct_decode_signed of shaders/common/gbuffer.glsl).
FUSE_HOST_DEVICE inline math::Vec3 oct_decode_signed(f32 ox, f32 oy) {
    math::Vec3 n{ox, oy, 1.f - std::fabs(ox) - std::fabs(oy)};
    if (n.z < 0.f) {
        const f32 x = (1.f - std::fabs(oy)) * (ox >= 0.f ? 1.f : -1.f);
        const f32 y = (1.f - std::fabs(ox)) * (oy >= 0.f ? 1.f : -1.f);
        n.x = x;
        n.y = y;
    }
    return safe_normalize(n, math::Vec3{0.f, 0.f, 1.f});
}

// ---------------------------------------------------------------------------------------------
// BRDF (shaders/common/brdf.glsl, the fifth power as multiplies)
// ---------------------------------------------------------------------------------------------

/// (diffuse + specular) * N.L for unit N, V, L; 0 when N.L <= 0.
FUSE_HOST_DEVICE inline math::Vec3 brdf_cos(const SurfaceSample& s, const math::Vec3& v, const math::Vec3& l) {
    const f32 n_dot_l = s.normal.dot(l);
    if (!(n_dot_l > 0.f)) {
        return {};
    }
    const math::Vec3 h = safe_normalize(v + l, s.normal);
    const f32 n_dot_v = std::max(s.normal.dot(v), 1e-4f);
    const f32 n_dot_h = std::max(s.normal.dot(h), 0.f);
    const f32 v_dot_h = std::max(v.dot(h), 0.f);
    const f32 r = std::clamp(s.roughness, kMinRoughness, 1.f);
    const f32 a = r * r;
    const f32 a2 = a * a;
    const f32 dd = (n_dot_h * a2 - n_dot_h) * n_dot_h + 1.f;
    const f32 d = a2 / (kPi * dd * dd);
    const f32 gv = n_dot_l * std::sqrt(n_dot_v * n_dot_v * (1.f - a2) + a2);
    const f32 gl = n_dot_v * std::sqrt(n_dot_l * n_dot_l * (1.f - a2) + a2);
    const f32 vis = 0.5f / std::max(gv + gl, 1e-5f);
    const f32 metallic = saturate(s.metallic);
    const f32 f = saturate(1.f - v_dot_h);
    const f32 f2 = f * f;
    const f32 f5 = f2 * f2 * f;
    const f32 dv = d * vis;
    const f32 kd = (1.f - metallic) / kPi;
    // Per channel: F0 = mix(0.04, albedo, metallic), F = Schlick, (kd (1 - F) albedo + D V F) N.L.
    auto channel = [&](f32 albedo) {
        const f32 f0 = 0.04f + (albedo - 0.04f) * metallic;
        const f32 fr = f0 + (1.f - f0) * f5;
        const f32 spec = dv * fr;
        const f32 diff = (1.f - fr) * kd * albedo;
        return (diff + spec) * n_dot_l;
    };
    return {channel(s.albedo.x), channel(s.albedo.y), channel(s.albedo.z)};
}

// ---------------------------------------------------------------------------------------------
// Lights (gpu_scene::GpuLight rows)
// ---------------------------------------------------------------------------------------------

FUSE_HOST_DEVICE inline math::Vec3 light_vec3(const f32 (&v)[3]) { return {v[0], v[1], v[2]}; }

/// Smooth spot cone: t^2, t = saturate((cos - cosOuter) / max(cosInner - cosOuter, 1e-4)).
FUSE_HOST_DEVICE inline f32 spot_cone(f32 cos_angle, f32 cos_inner, f32 cos_outer) {
    const f32 t = saturate((cos_angle - cos_outer) / std::max(cos_inner - cos_outer, kSpotMinWidth));
    return t * t;
}

/// Outgoing radiance towards `v` from one light; 0 for free slots and unknown types.
FUSE_HOST_DEVICE inline math::Vec3 light_contribution(const gpu_scene::GpuLight& light, const SurfaceSample& s,
                                                      const math::Vec3& v) {
    math::Vec3 l{};
    f32 attenuation = 1.f;
    if (light.type == static_cast<u32>(gpu_scene::GpuLightType::Directional)) {
        l = safe_normalize(light_vec3(light.direction) * -1.f, math::Vec3{0.f, 0.f, 1.f});
    } else if (light.type == static_cast<u32>(gpu_scene::GpuLightType::Point) ||
               light.type == static_cast<u32>(gpu_scene::GpuLightType::Spot)) {
        const math::Vec3 to_light = light_vec3(light.position) - s.position;
        const f32 distance = to_light.length();
        attenuation = clustered_kernel::point_light_falloff(distance, light.range);
        if (attenuation == 0.f) {
            return {};
        }
        l = to_light * (1.f / std::max(distance, 1e-6f));
        if (light.type == static_cast<u32>(gpu_scene::GpuLightType::Spot)) {
            const math::Vec3 axis = safe_normalize(light_vec3(light.direction), math::Vec3{0.f, 0.f, -1.f});
            attenuation = attenuation * spot_cone(-(l.dot(axis)), light.cosInner, light.cosOuter);
            if (attenuation == 0.f) {
                return {};
            }
        }
    } else {
        return {};
    }
    const math::Vec3 lobe = brdf_cos(s, v, l);
    const f32 scale = light.intensity * attenuation;
    return {lobe.x * light.color[0] * scale, lobe.y * light.color[1] * scale, lobe.z * light.color[2] * scale};
}

// ---------------------------------------------------------------------------------------------
// WP-2.2: compensated BRDF and area lights (with a BRDF LUT)
// ---------------------------------------------------------------------------------------------

/// Per-pixel terms: the multi-scatter weights (DFG at N.V) and the LTC frame / matrix.
struct SurfaceTerms {
    brdf::MultiScatterTerms ms{};
    ltc::LtcFrame frame{};
};

FUSE_HOST_DEVICE inline SurfaceTerms surface_terms(const SurfaceSample& s, const math::Vec3& v, const f32* lut) {
    SurfaceTerms t{};
    const f32 n_dot_v = std::max(s.normal.dot(v), brdf::kMinNoV);
    f32 dfg_a = 0.f;
    f32 dfg_b = 0.f;
    ltc::sample_dfg(lut, n_dot_v, s.roughness, dfg_a, dfg_b);
    t.ms = brdf::multi_scatter_terms(s.albedo, s.metallic, dfg_a, dfg_b);
    t.frame = ltc::make_frame(s.normal, v);
    ltc::sample_ltc(lut, n_dot_v, s.roughness, t.frame.m);
    return t;
}

/// Outgoing radiance towards `v` from one light with the compensated BRDF; 0 for free slots and
/// unknown types.
FUSE_HOST_DEVICE inline math::Vec3 light_contribution(const gpu_scene::GpuLight& light, const SurfaceSample& s,
                                                      const math::Vec3& v, const SurfaceTerms& t, const f32* lut) {
    math::Vec3 response{};
    f32 attenuation = 1.f;
    if (light.type == static_cast<u32>(gpu_scene::GpuLightType::Directional)) {
        const math::Vec3 l = safe_normalize(light_vec3(light.direction) * -1.f, math::Vec3{0.f, 0.f, 1.f});
        if (light.cosOuter < ltc::kMinSunCos) {
            response = ltc::sun_response(lut, t.frame, t.ms, s.albedo, s.roughness, v, l, light.cosOuter);
        } else {
            response = brdf::multi_scatter_cos(t.ms, s.albedo, s.roughness, s.normal, v, l);
        }
    } else if (light.type == static_cast<u32>(gpu_scene::GpuLightType::Point) ||
               light.type == static_cast<u32>(gpu_scene::GpuLightType::Spot)) {
        const math::Vec3 to_light = light_vec3(light.position) - s.position;
        const f32 distance = to_light.length();
        attenuation = clustered_kernel::point_light_falloff(distance, light.range);
        if (attenuation == 0.f) {
            return {};
        }
        const math::Vec3 l = to_light * (1.f / std::max(distance, 1e-6f));
        if (light.type == static_cast<u32>(gpu_scene::GpuLightType::Spot)) {
            const math::Vec3 axis = safe_normalize(light_vec3(light.direction), math::Vec3{0.f, 0.f, -1.f});
            attenuation = attenuation * spot_cone(-(l.dot(axis)), light.cosInner, light.cosOuter);
            if (attenuation == 0.f) {
                return {};
            }
        }
        response = brdf::multi_scatter_cos(t.ms, s.albedo, s.roughness, s.normal, v, l);
    } else if (light.type == ltc::kLightRect || light.type == ltc::kLightDisk) {
        const math::Vec3 c = light_vec3(light.position) - s.position;
        attenuation = ltc::area_window(c.length(), light.range);
        if (attenuation == 0.f) {
            return {};
        }
        const math::Vec3 normal = safe_normalize(light_vec3(light.direction), math::Vec3{0.f, 0.f, -1.f});
        math::Vec3 ex{};
        math::Vec3 ey{};
        ltc::area_axes(normal, ltc::decode_tangent(light.flags), light.cosInner, light.cosOuter, ex, ey);
        response = ltc::area_light_response(t.frame, t.ms, light.type == ltc::kLightDisk, c, ex, ey);
    } else {
        return {};
    }
    const f32 scale = light.intensity * attenuation;
    return {response.x * light.color[0] * scale, response.y * light.color[1] * scale, response.z * light.color[2] * scale};
}

// ---------------------------------------------------------------------------------------------
// light.shade reference kernel
// ---------------------------------------------------------------------------------------------

/// Decoded G-buffer channels (row-major, row 0 at the top), what texelFetch returns on the GPU.
struct GBufferTexels {
    const f32* depth = nullptr;          ///< RT4 .r (device depth)
    const math::Vec4* normalAo = nullptr; ///< RT0: signed oct normal .xy, AO .w
    const math::Vec4* albedo = nullptr;   ///< RT1
    const math::Vec4* roughMetal = nullptr; ///< RT2: roughness .x, metallic .y
    const math::Vec4* emissive = nullptr; ///< RT5 .rgb
};

struct ShadeParams {
    u32 width = 0;
    u32 height = 0;
    f32 inv_width = 0.f;
    f32 inv_height = 0.f;
    clustered_kernel::GridDims grid{};
    clustered_kernel::CameraView camera{};
    math::Vec3 ambient{};
    GBufferTexels gbuffer{};
    kernel::Span<const gpu_scene::GpuLight> lights;      ///< scene light table (slots)
    kernel::Span<const ClusterGridEntry> cluster_grid;   ///< (offset, count) per cluster
    kernel::Span<const u32> light_list;                  ///< flat list of light slots
    kernel::Span<const u32> directional;                 ///< directional light slots, ascending
    math::Vec4* out = nullptr;                           ///< width * height radiance (alpha 1 = shaded)
    /// WP-2.2 BRDF LUT (ltc::kLutWords, ltc::BrdfLut): compensated BRDF + area lights; empty = the
    /// WP-2.1 lobe.
    kernel::Span<const f32> brdf_lut;
};

/// Shades one pixel; false (and zero radiance) for sky / out-of-range pixels.
FUSE_HOST_DEVICE inline bool shade_pixel(const ShadeParams& p, u32 px, u32 py, math::Vec4& out) {
    out = {0.f, 0.f, 0.f, 0.f};
    const usize pixel = static_cast<usize>(py) * p.width + px;
    const f32 screen_x = (static_cast<f32>(px) + 0.5f) * p.inv_width;
    const f32 screen_y = (static_cast<f32>(py) + 0.5f) * p.inv_height;
    const clustered_kernel::CameraView& c = p.camera;
    const f32 view_depth =
        clustered_kernel::view_depth_from_device_depth(p.gbuffer.depth[pixel], c.near_plane, c.far_plane, c.reversed_z);
    u32 cluster = 0u;
    if (!(view_depth > 0.f) || !std::isfinite(view_depth) ||
        !clustered_kernel::map_screen_depth_to_cluster(screen_x, screen_y, view_depth, p.grid, c.near_plane, c.far_plane,
                                                       cluster)) {
        return false;
    }
    SurfaceSample s{};
    s.position = clustered_kernel::view_to_world(
        c, clustered_kernel::view_position_from_screen(screen_x, screen_y, view_depth, c.tan_x, c.tan_y));
    const math::Vec4 rt0 = p.gbuffer.normalAo[pixel];
    const math::Vec4 rt1 = p.gbuffer.albedo[pixel];
    const math::Vec4 rt2 = p.gbuffer.roughMetal[pixel];
    const math::Vec4 rt5 = p.gbuffer.emissive[pixel];
    s.normal = oct_decode_signed(rt0.x, rt0.y);
    s.ao = rt0.w;
    s.albedo = {rt1.x, rt1.y, rt1.z};
    s.roughness = rt2.x;
    s.metallic = rt2.y;
    s.emissive = {rt5.x, rt5.y, rt5.z};
    const math::Vec3 v = safe_normalize(c.position - s.position, s.normal);

    math::Vec3 radiance = s.emissive + vmul(p.ambient, s.albedo) * s.ao;
    const bool compensated = p.brdf_lut.size >= ltc::kLutWords;
    SurfaceTerms terms{};
    if (compensated) {
        terms = surface_terms(s, v, p.brdf_lut.data);
    }
    for (u32 i = 0; i < p.directional.size; ++i) {
        const u32 slot = p.directional[i];
        if (slot < p.lights.size) {
            radiance = radiance + (compensated ? light_contribution(p.lights[slot], s, v, terms, p.brdf_lut.data)
                                               : light_contribution(p.lights[slot], s, v));
        }
    }
    if (cluster < p.cluster_grid.size) {
        const ClusterGridEntry entry = p.cluster_grid[cluster];
        const u32 end = std::min(entry.offset + entry.count, p.light_list.size);
        for (u32 i = entry.offset; i < end; ++i) {
            const u32 slot = p.light_list[i];
            if (slot < p.lights.size) {
                radiance = radiance + (compensated ? light_contribution(p.lights[slot], s, v, terms, p.brdf_lut.data)
                                                   : light_contribution(p.lights[slot], s, v));
            }
        }
    }
    out = {radiance.x, radiance.y, radiance.z, 1.f};
    return true;
}

/// One pixel per item (8 x 8 workgroups, like the GPU kernel).
struct ShadeKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const ShadeParams& p) const {
        math::Vec4 out{};
        shade_pixel(p, idx.global.x, idx.global.y, out);
        p.out[static_cast<usize>(idx.global.y) * p.width + idx.global.x] = out;
    }
};

inline kernel::KernelLaunch make_shade_launch(u32 width, u32 height) {
    return kernel::KernelLaunch{kShadeName, kernel::extent2(width, height), kShadeWorkgroup};
}

} // namespace fuse::renderer::lighting_gpu
