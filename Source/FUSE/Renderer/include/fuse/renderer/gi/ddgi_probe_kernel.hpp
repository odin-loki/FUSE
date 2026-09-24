#pragma once

// Single-source DDGI probe update (docs/compute-kernels.md). Every function here is FUSE_HOST_DEVICE
// and is the ONLY implementation of the probe-trace scene (analytic boxes + sun + sky), the
// octahedral probe encoding / bilinear tile fetch, the Chebyshev-weighted volume sample and the
// irradiance / distance-moment hysteresis blend. DdgiCpuScene / DdgiCpuVolume (src/gi/ddgi_cpu.cpp),
// DdgiIrradianceEncoding (src/gi/ddgi.cpp) and the CUDA wrapper (kernels/ddgi_probe_update.cu) all
// run this code.
//
// Two launches per update, mirroring the device split (probe_trace_kernel / probe_blend_kernel):
//
//   kTraceName ("ddgi_probe_trace")  item kernel, one ray per item (x = ray, y = scheduled slot).
//                                    Reads the pre-update volume for multi-bounce, writes per-ray
//                                    radiance + hit distance.
//   kName ("ddgi_probe_update")      workgroup kernel, one workgroup per scheduled probe (the
//                                    render-graph pass name). Phases: 0 init scratch, 1 per-texel
//                                    incoming irradiance, 2 probe-mean change detection (thread 0,
//                                    texel order), 3 irradiance + distance hysteresis blend,
//                                    4 octahedral border copy + update count.
//
// A blend reads and writes only its own probe's tiles, so workgroups never race unless one probe is
// scheduled twice in one launch; the host then runs the blend on CpuReference (serial, in list order,
// exactly the pre-port behaviour).
//
// Trace output distances are signed (RTXGI convention): a backface hit stores -(t x backface scale), so
// the probe-state pass can count backfaces; the blend uses |distance| (identical atlases).
//
// Optional third launch (kStateName, "ddgi_probe_state"), when DdgiCpuConfig::probe_relocation or
// probe_classification is on: item kernel, one scheduled probe per item, after the blend. Probe
// relocation + classification as in RTXGI DDGI (Majercik et al. 2021, JCGT 10(2); RTXGI SDK
// ProbeRelocationCS / ProbeClassificationCS) on the update's ray results; writes the per-probe data
// (offset xyz, state w) that the NEXT update's trace (ray origins), blend (inactive probes skipped) and
// every sample_irradiance (relocated positions, inactive probes skipped) read.

#include <fuse/compute_kernel/atomics.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/gi/ddgi.hpp>
#include <fuse/renderer/gi/ddgi_cpu.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::renderer::ddgi_kernel {

/// Blend kernel / profiler / GPU-timestamp name (matches DeferredFramePipeline's DdgiProbeUpdate pass).
inline constexpr const char* kName = "ddgi_probe_update";
/// Probe-ray trace sub-pass of the same render-graph pass.
inline constexpr const char* kTraceName = "ddgi_probe_trace";
/// Trace: 64 rays of one probe per workgroup. Blend: 64 threads per probe (texel loops are strided).
inline constexpr kernel::Dim3 kTraceWorkgroup{64u, 1u, 1u};
inline constexpr u32 kBlendThreads = 64u;

inline constexpr f32 kPi = 3.14159265358979323846f;
inline constexpr f32 kInvPi = 1.f / kPi;
inline constexpr f32 kRayEpsilon = 1e-4f;

// ---------------------------------------------------------------------------------------------
// Small vector helpers
// ---------------------------------------------------------------------------------------------

FUSE_HOST_DEVICE inline math::Vec3 mul(const math::Vec3& a, const math::Vec3& b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

FUSE_HOST_DEVICE inline f32 max_component(const math::Vec3& v) {
    return std::max(v.x, std::max(v.y, v.z));
}

FUSE_HOST_DEVICE inline math::Vec3 abs_vec(const math::Vec3& v) {
    return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)};
}

FUSE_HOST_DEVICE inline f32 axis(const math::Vec3& v, u32 i) {
    return i == 0u ? v.x : (i == 1u ? v.y : v.z);
}

// ---------------------------------------------------------------------------------------------
// Octahedral encoding + probe grid (DdgiIrradianceEncoding / ProbeGridLayout delegate here)
// ---------------------------------------------------------------------------------------------

FUSE_HOST_DEVICE inline bool is_empty_direction(const math::Vec3& direction) {
    return direction.dot(direction) < 1e-8f;
}

/// Normalizes `direction`, or `fallback` when empty; +Y when both are degenerate.
FUSE_HOST_DEVICE inline math::Vec3 resolve_direction(const math::Vec3& direction,
                                                     const math::Vec3& fallback = {0.f, 1.f, 0.f}) {
    if (!is_empty_direction(direction)) {
        return direction.normalized();
    }
    if (!is_empty_direction(fallback)) {
        return fallback.normalized();
    }
    return {0.f, 1.f, 0.f};
}

/// Octahedral UV in [0, 1]^2 of a (resolved) direction.
FUSE_HOST_DEVICE inline math::Vec2 encode_direction(const math::Vec3& direction) {
    math::Vec3 n = resolve_direction(direction);

    const f32 sum = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    if (sum > 1e-8f) {
        n = n * (1.f / sum);
    }

    math::Vec2 o{};
    if (n.z >= 0.f) {
        o.x = n.x;
        o.y = n.y;
    } else {
        o.x = (1.f - std::fabs(n.y)) * (n.x >= 0.f ? 1.f : -1.f);
        o.y = (1.f - std::fabs(n.x)) * (n.y >= 0.f ? 1.f : -1.f);
    }

    return {o.x * 0.5f + 0.5f, o.y * 0.5f + 0.5f};
}

FUSE_HOST_DEVICE inline bool is_empty_grid(const DDGIDesc& desc) {
    return desc.grid_dims.x == 0u || desc.grid_dims.y == 0u || desc.grid_dims.z == 0u;
}

FUSE_HOST_DEVICE inline math::Vec3 probe_world_position(const DDGIDesc& desc, u32 probe_index) {
    if (is_empty_grid(desc)) {
        return desc.grid_origin;
    }
    const u32 slice = desc.grid_dims.x * desc.grid_dims.y;
    const u32 z = probe_index / slice;
    const u32 rem = probe_index % slice;
    const u32 y = rem / desc.grid_dims.x;
    const u32 x = rem % desc.grid_dims.x;
    return desc.grid_origin + math::Vec3{desc.probe_spacing.x * static_cast<f32>(x),
                                         desc.probe_spacing.y * static_cast<f32>(y),
                                         desc.probe_spacing.z * static_cast<f32>(z)};
}

FUSE_HOST_DEVICE inline math::Vec3 world_to_probe_grid(const DDGIDesc& desc, const math::Vec3& world_position) {
    const math::Vec3 delta = world_position - desc.grid_origin;
    if (desc.probe_spacing.x <= 0.f || desc.probe_spacing.y <= 0.f || desc.probe_spacing.z <= 0.f) {
        return {};
    }
    return {delta.x / desc.probe_spacing.x, delta.y / desc.probe_spacing.y, delta.z / desc.probe_spacing.z};
}

/// Bordered-tile bilinear fetch at octahedral direction `direction`.
template <typename T>
FUSE_HOST_DEVICE inline T sample_tile(const T* tile, u32 res, const math::Vec3& direction) {
    const u32 stride = res + 2u;
    const math::Vec2 uv = encode_direction(direction);
    // Interior texel k covers [k+1, k+2) of the bordered tile; centres sit at +0.5.
    const f32 px = 1.f + uv.x * static_cast<f32>(res) - 0.5f;
    const f32 py = 1.f + uv.y * static_cast<f32>(res) - 0.5f;
    const f32 fx = std::clamp(px, 0.f, static_cast<f32>(res));
    const f32 fy = std::clamp(py, 0.f, static_cast<f32>(res));
    const u32 x0 = std::min(static_cast<u32>(fx), res);
    const u32 y0 = std::min(static_cast<u32>(fy), res);
    const u32 x1 = x0 + 1u;
    const u32 y1 = y0 + 1u;
    const f32 tx = fx - static_cast<f32>(x0);
    const f32 ty = fy - static_cast<f32>(y0);
    const T a = tile[y0 * stride + x0] * (1.f - tx) + tile[y0 * stride + x1] * tx;
    const T b = tile[y1 * stride + x0] * (1.f - tx) + tile[y1 * stride + x1] * tx;
    return a * (1.f - ty) + b * ty;
}

// ---------------------------------------------------------------------------------------------
// Analytic probe-trace scene (DdgiCpuScene delegates here)
// ---------------------------------------------------------------------------------------------

/// Closest box hit along `direction` (unit) within (t_min, t_max).
FUSE_HOST_DEVICE inline bool intersect_boxes(const DdgiCpuBox* boxes,
                                             u32 box_count,
                                             const math::Vec3& origin,
                                             const math::Vec3& direction,
                                             f32 t_min,
                                             f32 t_max,
                                             DdgiCpuHit& out_hit) {
    out_hit = DdgiCpuHit{};
    f32 best = t_max;
    for (u32 b = 0; b < box_count; ++b) {
        const DdgiCpuBox& box = boxes[b];
        f32 t_near = -std::numeric_limits<f32>::infinity();
        f32 t_far = std::numeric_limits<f32>::infinity();
        u32 near_axis = 0u;
        u32 far_axis = 0u;
        bool miss = false;
        for (u32 a = 0; a < 3u; ++a) {
            const f32 o = axis(origin, a);
            const f32 d = axis(direction, a);
            const f32 lo = axis(box.min, a);
            const f32 hi = axis(box.max, a);
            if (std::fabs(d) < 1e-12f) {
                if (o < lo || o > hi) {
                    miss = true;
                    break;
                }
                continue;
            }
            const f32 inv = 1.f / d;
            f32 t0 = (lo - o) * inv;
            f32 t1 = (hi - o) * inv;
            if (t0 > t1) {
                const f32 swap = t0;
                t0 = t1;
                t1 = swap;
            }
            if (t0 > t_near) {
                t_near = t0;
                near_axis = a;
            }
            if (t1 < t_far) {
                t_far = t1;
                far_axis = a;
            }
        }
        if (miss || t_near > t_far) {
            continue;
        }
        f32 t = 0.f;
        bool backface = false;
        u32 hit_axis = 0u;
        if (t_near > t_min) {
            t = t_near;
            hit_axis = near_axis;
        } else if (t_far > t_min) {
            t = t_far;
            hit_axis = far_axis;
            backface = true;
        } else {
            continue;
        }
        if (t >= best) {
            continue;
        }
        best = t;
        const f32 d = axis(direction, hit_axis);
        // Entry faces face against the ray; exit faces (backface) face along it.
        const f32 sign = backface ? (d >= 0.f ? 1.f : -1.f) : (d >= 0.f ? -1.f : 1.f);
        math::Vec3 n{};
        if (hit_axis == 0u) {
            n.x = sign;
        } else if (hit_axis == 1u) {
            n.y = sign;
        } else {
            n.z = sign;
        }
        out_hit.hit = true;
        out_hit.backface = backface;
        out_hit.t = t;
        out_hit.position = origin + direction * t;
        out_hit.normal = n;
        out_hit.box_index = b;
    }
    return out_hit.hit;
}

/// Sun + sky + boxes of one trace (a POD view of DdgiCpuScene).
struct SceneView {
    const DdgiCpuBox* boxes = nullptr;
    u32 box_count = 0;
    math::Vec3 sun_direction{0.f, 1.f, 0.f};
    math::Vec3 sun_irradiance{};
    math::Vec3 sky_radiance{};
};

/// Emitted + sun-lit (shadowed) Lambertian radiance leaving a front-face hit.
FUSE_HOST_DEVICE inline math::Vec3 direct_radiance(const SceneView& scene, const DdgiCpuHit& hit) {
    if (!hit.hit || hit.backface || hit.box_index >= scene.box_count) {
        return {};
    }
    const DdgiCpuSurface& surface = scene.boxes[hit.box_index].surface;
    math::Vec3 radiance = surface.emissive;
    const f32 cos_sun = hit.normal.dot(scene.sun_direction);
    if (cos_sun > 0.f && max_component(scene.sun_irradiance) > 0.f) {
        const math::Vec3 shadow_origin = hit.position + hit.normal * kRayEpsilon;
        DdgiCpuHit shadow{};
        if (!intersect_boxes(scene.boxes, scene.box_count, shadow_origin, scene.sun_direction, 0.f,
                             std::numeric_limits<f32>::infinity(), shadow)) {
            radiance = radiance + mul(surface.albedo, scene.sun_irradiance) * (cos_sun * kInvPi);
        }
    }
    return radiance;
}

// ---------------------------------------------------------------------------------------------
// Probe volume sampling (DdgiCpuVolume::probeIrradiance / probeDistance / sampleIrradiance)
// ---------------------------------------------------------------------------------------------

/// Probe states (VolumeView::probe_data[i].w).
inline constexpr f32 kProbeActive = 0.f;
inline constexpr f32 kProbeInactive = 1.f;

/// Read-only POD view of a probe volume's bordered atlases.
struct VolumeView {
    DDGIDesc desc{};
    u32 probe_count = 0;
    const math::Vec3* irradiance = nullptr; ///< probe_count tiles of (irradiance_res + 2)^2 texels (E/pi).
    const math::Vec2* distance = nullptr;   ///< probe_count tiles of (depth_res + 2)^2 moments.
    f32 normal_bias = 0.1f;
    f32 weight_crush_threshold = 0.2f;
    /// Optional per-probe (relocation offset xyz, state w); null = grid positions, every probe active.
    const math::Vec4* probe_data = nullptr;
    /// Offset of the sample point toward the viewer (world units; RTXGI probeViewBias). Used by the
    /// sample_irradiance overload that takes a view direction; 0 = normal bias only.
    f32 view_bias = 0.f;
};

/// Grid position + relocation offset (== probe_world_position without probe data).
FUSE_HOST_DEVICE inline math::Vec3 probe_position(const VolumeView& v, u32 probe_index) {
    const math::Vec3 p = probe_world_position(v.desc, probe_index);
    if (v.probe_data == nullptr || probe_index >= v.probe_count) {
        return p;
    }
    const math::Vec4& d = v.probe_data[probe_index];
    return {p.x + d.x, p.y + d.y, p.z + d.z};
}

/// False when classification marked the probe inactive.
FUSE_HOST_DEVICE inline bool probe_active(const VolumeView& v, u32 probe_index) {
    return v.probe_data == nullptr || probe_index >= v.probe_count || v.probe_data[probe_index].w == kProbeActive;
}

FUSE_HOST_DEVICE inline usize irradiance_offset(const VolumeView& v, u32 probe_index) {
    const usize tile = static_cast<usize>(v.desc.irradiance_res) + 2u;
    return static_cast<usize>(probe_index) * tile * tile;
}

FUSE_HOST_DEVICE inline usize distance_offset(const VolumeView& v, u32 probe_index) {
    const usize tile = static_cast<usize>(v.desc.depth_res) + 2u;
    return static_cast<usize>(probe_index) * tile * tile;
}

/// Bilinear octahedral read of one probe: irradiance E for surface normal `direction`.
FUSE_HOST_DEVICE inline math::Vec3 probe_irradiance(const VolumeView& v, u32 probe_index, const math::Vec3& direction) {
    if (probe_index >= v.probe_count) {
        return {};
    }
    const math::Vec3 dir = resolve_direction(direction);
    return sample_tile(v.irradiance + irradiance_offset(v, probe_index), v.desc.irradiance_res, dir) * kPi;
}

/// Bilinear octahedral read of one probe's distance moments along `direction`.
FUSE_HOST_DEVICE inline math::Vec2 probe_distance(const VolumeView& v, u32 probe_index, const math::Vec3& direction) {
    if (probe_index >= v.probe_count) {
        return {};
    }
    const math::Vec3 dir = resolve_direction(direction);
    return sample_tile(v.distance + distance_offset(v, probe_index), v.desc.depth_res, dir);
}

/// Irradiance E at `position` whose visibility / interpolation point is `biased` (see below).
FUSE_HOST_DEVICE inline math::Vec3 sample_irradiance_biased(const VolumeView& v,
                                                            const math::Vec3& position,
                                                            const math::Vec3& n,
                                                            const math::Vec3& biased) {
    const DDGIDesc& desc = v.desc;
    const math::Vec3 grid = world_to_probe_grid(desc, biased);

    const u32 dims[3] = {desc.grid_dims.x, desc.grid_dims.y, desc.grid_dims.z};
    u32 base[3]{};
    f32 alpha[3]{};
    for (u32 a = 0; a < 3u; ++a) {
        const f32 g = axis(grid, a);
        const f32 max_base = static_cast<f32>(dims[a] - 1u);
        const f32 fb = std::clamp(std::floor(g), 0.f, max_base);
        base[a] = static_cast<u32>(fb);
        alpha[a] = std::clamp(g - fb, 0.f, 1.f);
    }

    math::Vec3 sum{};
    f32 weight_sum = 0.f;
    for (u32 corner = 0; corner < 8u; ++corner) {
        f32 trilinear = 1.f;
        u32 c[3]{};
        for (u32 a = 0; a < 3u; ++a) {
            const u32 bit = (corner >> a) & 1u;
            c[a] = std::min(base[a] + bit, dims[a] - 1u);
            trilinear *= bit ? alpha[a] : (1.f - alpha[a]);
        }
        if (trilinear <= 0.f) {
            continue;
        }
        const u32 probe = c[2] * dims[0] * dims[1] + c[1] * dims[0] + c[0];
        // Classification: inactive probes (inside geometry / nothing to light) take no part; their
        // trilinear weight is renormalised onto the active neighbours.
        if (!probe_active(v, probe)) {
            continue;
        }
        // Relocation: visibility and the backface term use the moved probe; the trilinear weights keep
        // the grid positions (RTXGI).
        const math::Vec3 probe_pos = probe_position(v, probe);

        // Smooth backface term: probes behind the surface fade out without a hard cut.
        f32 weight = 1.f;
        const math::Vec3 to_probe = probe_pos - position;
        const f32 to_probe_len = to_probe.length();
        if (to_probe_len > 1e-6f) {
            const f32 wrap = (to_probe.dot(n) / to_probe_len + 1.f) * 0.5f;
            weight *= wrap * wrap + 0.2f;
        }

        // Chebyshev visibility from the probe's distance moments toward the biased point.
        const math::Vec3 probe_to_point = biased - probe_pos;
        const f32 dist = probe_to_point.length();
        if (dist > 1e-6f) {
            const math::Vec2 moments = probe_distance(v, probe, probe_to_point * (1.f / dist));
            const f32 mean = moments.x;
            if (dist > mean) {
                const f32 variance = std::fabs(moments.y - mean * mean);
                const f32 delta = dist - mean;
                f32 chebyshev = variance / std::max(variance + delta * delta, 1e-12f);
                chebyshev = std::max(chebyshev * chebyshev * chebyshev, 0.f);
                weight *= std::max(chebyshev, 0.05f);
            }
        }
        weight = std::max(weight, 1e-6f);
        const f32 crush = v.weight_crush_threshold;
        if (crush > 0.f && weight < crush) {
            weight *= (weight * weight) / (crush * crush);
        }
        weight *= trilinear;

        sum = sum + probe_irradiance(v, probe, n) * weight;
        weight_sum += weight;
    }
    if (weight_sum <= 0.f) {
        return {};
    }
    return sum * (1.f / weight_sum);
}

/// World-space irradiance E at a surface point: trilinear over 8 probes with backface (wrap) and
/// Chebyshev visibility weights; the point is offset by normal_bias along the normal.
FUSE_HOST_DEVICE inline math::Vec3 sample_irradiance(const VolumeView& v,
                                                     const math::Vec3& position,
                                                     const math::Vec3& normal) {
    const math::Vec3 n = resolve_direction(normal);
    return sample_irradiance_biased(v, position, n, position + n * v.normal_bias);
}

/// The same with the RTXGI surface bias: normal_bias along the normal plus view_bias toward the viewer
/// (`view` = unit direction from the surface to the camera; for probe rays, minus the ray direction).
/// The view term moves the point to the viewer's side of a thin wall, where the probes the viewer shares
/// a room with pass the Chebyshev test and the probes behind the wall do not (Majercik et al. 2021, §5).
FUSE_HOST_DEVICE inline math::Vec3 sample_irradiance(const VolumeView& v,
                                                     const math::Vec3& position,
                                                     const math::Vec3& normal,
                                                     const math::Vec3& view) {
    const math::Vec3 n = resolve_direction(normal);
    if (v.view_bias == 0.f) {
        return sample_irradiance_biased(v, position, n, position + n * v.normal_bias);
    }
    const math::Vec3 w = resolve_direction(view, n);
    return sample_irradiance_biased(v, position, n, position + n * v.normal_bias + w * v.view_bias);
}

// ---------------------------------------------------------------------------------------------
// Trace kernel (kTraceName)
// ---------------------------------------------------------------------------------------------

struct TraceParams {
    kernel::Span<const u32> probe_indices; ///< Scheduled probes (one grid row each).
    kernel::Span<const math::Vec3> ray_dirs; ///< rays_per_probe unit directions (rotated, shared).
    SceneView scene{};
    VolumeView volume{}; ///< Pre-update volume (multi-bounce reads).
    f32 backface_distance_scale = 0.2f;
    bool multi_bounce = true;
    kernel::Span<math::Vec3> out_radiance; ///< slot * rays + ray
    kernel::Span<f32> out_distance;        ///< slot * rays + ray
};

/// Radiance arriving at `origin` from `direction` (unit) and the hit distance used for visibility
/// (positive; `out_backface`, when given, reports a backface hit).
FUSE_HOST_DEVICE inline math::Vec3 trace_radiance(const TraceParams& p,
                                                  const math::Vec3& origin,
                                                  const math::Vec3& direction,
                                                  f32& out_distance,
                                                  bool* out_backface = nullptr) {
    const f32 max_distance = p.volume.desc.max_ray_distance;
    if (out_backface != nullptr) {
        *out_backface = false;
    }
    DdgiCpuHit hit{};
    if (!intersect_boxes(p.scene.boxes, p.scene.box_count, origin, direction, 0.f, max_distance, hit)) {
        out_distance = max_distance;
        return p.scene.sky_radiance;
    }
    if (hit.backface) {
        // Probe sits inside geometry: no light, and a short distance so visibility rejects it.
        out_distance = hit.t * p.backface_distance_scale;
        if (out_backface != nullptr) {
            *out_backface = true;
        }
        return {};
    }
    out_distance = hit.t;
    math::Vec3 radiance = direct_radiance(p.scene, hit);
    if (p.multi_bounce) {
        const math::Vec3& albedo = p.scene.boxes[hit.box_index].surface.albedo;
        // The probe is the viewer of its ray's hit (RTXGI: surface bias with the ray direction).
        radiance = radiance + mul(albedo, sample_irradiance(p.volume, hit.position, hit.normal, direction * -1.f)) * kInvPi;
    }
    return radiance;
}

struct TraceKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const TraceParams& p) const {
        const u32 ray = idx.global.x;
        const u32 slot = idx.global.y;
        const u32 probe = p.probe_indices[slot];
        if (probe >= p.volume.probe_count) {
            return;
        }
        const u32 index = slot * p.ray_dirs.size + ray;
        const math::Vec3 origin = probe_position(p.volume, probe);
        f32 distance = 0.f;
        bool backface = false;
        p.out_radiance[index] = trace_radiance(p, origin, p.ray_dirs[ray], distance, &backface);
        p.out_distance[index] = backface ? -distance : distance; // signed: backface hits negative
    }
};

inline kernel::KernelLaunch make_trace_launch(u32 rays, u32 slots) {
    return kernel::KernelLaunch{kTraceName, kernel::extent2(rays, slots), kTraceWorkgroup};
}

// ---------------------------------------------------------------------------------------------
// Blend kernel (kName)
// ---------------------------------------------------------------------------------------------

struct BlendParams {
    kernel::Span<const u32> probe_indices;
    u32 probe_count = 0; ///< Probes in the volume (out-of-range scheduled indices are skipped).
    kernel::Span<const math::Vec3> ray_dirs;
    kernel::Span<const math::Vec3> radiance; ///< slot * rays + ray (trace output)
    kernel::Span<const f32> distance;        ///< slot * rays + ray (trace output)
    kernel::Span<const math::Vec3> irradiance_texel_dirs; ///< irradiance_res^2 interior texel directions
    kernel::Span<const math::Vec3> distance_texel_dirs;   ///< depth_res^2 interior texel directions
    math::Vec3* irradiance = nullptr; ///< Volume atlas (bordered tiles), updated in place.
    math::Vec2* distance_moments = nullptr;
    u32* update_counts = nullptr;             ///< Per-probe blend count.
    kernel::Span<math::Vec4> incoming;        ///< slot * irradiance_res^2 staging (xyz incoming, w valid).
    u32* fast_response_texels = nullptr;      ///< Global counter (texels with reduced hysteresis).
    u32 irradiance_res = 0;
    u32 depth_res = 0;
    f32 hysteresis = 0.97f;            ///< clamp(desc.hysteresis, 0, 1)
    f32 probe_change_hysteresis = 0.f; ///< clamp(config.probe_change_hysteresis, 0, 1)
    f32 probe_change_threshold = 0.1f;
    f32 change_threshold = 0.25f;
    f32 change_hysteresis_drop = 0.75f;
    f32 change_floor = 1e-3f;
    f32 distance_power = 50.f; ///< max(config.distance_power, 1e-3)
    f32 distance_min_cos = 0.f; ///< pow(1e-6, 1 / distance_power): lighter rays cannot move the mean
    f32 max_distance = 20.f; ///< distance clamp of the moments (DdgiCpuConfig::distance_clamp, else max_ray_distance)
    /// Optional per-probe data (classification): probes whose state is not kProbeActive are not blended.
    const math::Vec4* probe_data = nullptr;
};

/// One workgroup per scheduled probe (see the file comment for the phases).
struct BlendKernel {
    using Scratch = u32;
    static constexpr u32 kScratchCount = 4u; ///< [0] probe changed, [1] fast texels, [2] first update
    static constexpr u32 kPhases = 5u;

    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx,
                                     const kernel::WorkgroupContext<u32>& wg,
                                     const BlendParams& p) const {
        const u32 slot = idx.group.x;
        const u32 probe = p.probe_indices[slot];
        if (probe >= p.probe_count) {
            return; // uniform across the workgroup
        }
        if (p.probe_data != nullptr && p.probe_data[probe].w != kProbeActive) {
            return; // inactive probe: history kept, not blended (uniform)
        }
        const u32 tid = idx.local_linear;
        const u32 threads = idx.workgroup.x;
        u32* s = wg.scratch;
        const u32 ir = p.irradiance_res;
        const u32 texels = ir * ir;
        const u32 irr_stride = ir + 2u;
        const usize irr_tile_size = static_cast<usize>(irr_stride) * irr_stride;
        math::Vec3* irr_tile = p.irradiance + static_cast<usize>(probe) * irr_tile_size;
        math::Vec4* incoming = p.incoming.data + static_cast<usize>(slot) * texels;
        const math::Vec3* radiance = p.radiance.data + static_cast<usize>(slot) * p.ray_dirs.size;
        const f32* distances = p.distance.data + static_cast<usize>(slot) * p.ray_dirs.size;
        const u32 ray_count = p.ray_dirs.size;

        switch (wg.phase) {
        case 0:
            if (tid == 0u) {
                s[0] = 0u;
                s[1] = 0u;
                s[2] = p.update_counts[probe] == 0u ? 1u : 0u;
            }
            break;
        case 1:
            // Cosine-weighted mean incoming radiance per interior irradiance texel.
            for (u32 t = tid; t < texels; t += threads) {
                const math::Vec3& texel_dir = p.irradiance_texel_dirs[t];
                const f32 tx = texel_dir.x;
                const f32 ty = texel_dir.y;
                const f32 tz = texel_dir.z;
                f32 sum_r = 0.f;
                f32 sum_g = 0.f;
                f32 sum_b = 0.f;
                f32 weight_sum = 0.f;
                for (u32 r = 0; r < ray_count; ++r) {
                    const math::Vec3& d = p.ray_dirs[r];
                    const f32 w = tx * d.x + ty * d.y + tz * d.z;
                    if (w > 0.f) {
                        const math::Vec3& l = radiance[r];
                        sum_r += l.x * w;
                        sum_g += l.y * w;
                        sum_b += l.z * w;
                        weight_sum += w;
                    }
                }
                if (weight_sum <= 0.f) {
                    incoming[t] = math::Vec4{0.f, 0.f, 0.f, 0.f};
                    continue;
                }
                const f32 inv_weight = 1.f / weight_sum;
                incoming[t] = math::Vec4{sum_r * inv_weight, sum_g * inv_weight, sum_b * inv_weight, 1.f};
            }
            break;
        case 2:
            // Lighting discontinuity detection. Probe level: the mean over all texels averages out
            // most per-texel ray noise, so a modest threshold catches global changes (sun, sky) with
            // few false triggers, and the stale history is dropped. Texel level (phase 3): a larger
            // threshold catches local changes and lowers the hysteresis by change_hysteresis_drop.
            // Serial on thread 0 in texel order: the float sums match on every backend.
            if (tid == 0u && s[2] == 0u) {
                math::Vec3 incoming_mean{};
                math::Vec3 history_mean{};
                u32 valid_count = 0u;
                for (u32 t = 0; t < texels; ++t) {
                    if (incoming[t].w == 0.f) {
                        continue;
                    }
                    const u32 y = t / ir;
                    const u32 x = t % ir;
                    incoming_mean = incoming_mean + math::Vec3{incoming[t].x, incoming[t].y, incoming[t].z};
                    history_mean = history_mean + irr_tile[(y + 1u) * irr_stride + (x + 1u)];
                    ++valid_count;
                }
                if (valid_count > 0u) {
                    const f32 inv = 1.f / static_cast<f32>(valid_count);
                    const math::Vec3 in_mean = incoming_mean * inv;
                    const math::Vec3 hist_mean = history_mean * inv;
                    const f32 scale =
                        std::max(std::max(max_component(in_mean), max_component(hist_mean)), p.change_floor);
                    s[0] = max_component(abs_vec(in_mean - hist_mean)) > p.probe_change_threshold * scale ? 1u : 0u;
                }
            }
            break;
        case 3: {
            const bool first = s[2] != 0u;
            const bool probe_changed = s[0] != 0u;
            const f32 hysteresis = first ? 0.f : p.hysteresis;
            const f32 fast_hysteresis = std::max(0.f, hysteresis - p.change_hysteresis_drop);
            for (u32 t = tid; t < texels; t += threads) {
                if (incoming[t].w == 0.f) {
                    continue;
                }
                const math::Vec3 in{incoming[t].x, incoming[t].y, incoming[t].z};
                const u32 y = t / ir;
                const u32 x = t % ir;
                math::Vec3& texel = irr_tile[(y + 1u) * irr_stride + (x + 1u)];
                f32 h = hysteresis;
                if (!first) {
                    const f32 scale = std::max(std::max(max_component(texel), max_component(in)), p.change_floor);
                    if (probe_changed) {
                        h = std::min(hysteresis, p.probe_change_hysteresis);
                        kernel::scratch_atomic_add(&s[1], 1u);
                    } else if (max_component(abs_vec(in - texel)) > p.change_threshold * scale) {
                        h = fast_hysteresis;
                        kernel::scratch_atomic_add(&s[1], 1u);
                    }
                }
                texel = in * (1.f - h) + texel * h;
            }

            const u32 dr = p.depth_res;
            const u32 dist_stride = dr + 2u;
            math::Vec2* dist_tile =
                p.distance_moments + static_cast<usize>(probe) * (static_cast<usize>(dist_stride) * dist_stride);
            for (u32 t = tid; t < dr * dr; t += threads) {
                const math::Vec3& texel_dir = p.distance_texel_dirs[t];
                const f32 tx = texel_dir.x;
                const f32 ty = texel_dir.y;
                const f32 tz = texel_dir.z;
                f32 sum_d = 0.f;
                f32 sum_d2 = 0.f;
                f32 weight_sum = 0.f;
                for (u32 r = 0; r < ray_count; ++r) {
                    const math::Vec3& dir = p.ray_dirs[r];
                    const f32 c = tx * dir.x + ty * dir.y + tz * dir.z;
                    if (c > p.distance_min_cos) {
                        const f32 w = std::pow(c, p.distance_power);
                        const f32 d = std::min(std::fabs(distances[r]), p.max_distance);
                        sum_d += d * w;
                        sum_d2 += d * d * w;
                        weight_sum += w;
                    }
                }
                if (weight_sum <= 1e-12f) {
                    continue;
                }
                const math::Vec2 in{sum_d / weight_sum, sum_d2 / weight_sum};
                math::Vec2& texel = dist_tile[((t / dr) + 1u) * dist_stride + ((t % dr) + 1u)];
                texel = in * (1.f - hysteresis) + texel * hysteresis;
            }
            break;
        }
        default: {
            // Border rings read the blended interiors; one thread per atlas.
            const u32 dr = p.depth_res;
            const u32 dist_stride = dr + 2u;
            if (tid == 0u) {
                ddgi_cpu::copyOctahedralBorder(irr_tile, ir, irr_stride);
                if (s[1] != 0u) {
                    kernel::global_atomic_add(p.fast_response_texels, s[1]);
                }
                p.update_counts[probe] += 1u;
            }
            if (tid == (threads > 1u ? 1u : 0u)) {
                ddgi_cpu::copyOctahedralBorder(
                    p.distance_moments + static_cast<usize>(probe) * (static_cast<usize>(dist_stride) * dist_stride),
                    dr, dist_stride);
            }
            break;
        }
        }
    }
};

inline kernel::KernelLaunch make_blend_launch(u32 slots) {
    return kernel::KernelLaunch{kName, kernel::extent1(slots * kBlendThreads), {kBlendThreads, 1u, 1u}};
}

// ---------------------------------------------------------------------------------------------
// Probe state kernel (kStateName): relocation + classification
// ---------------------------------------------------------------------------------------------

/// Probe-state sub-pass of the same render-graph pass (after the blend).
inline constexpr const char* kStateName = "ddgi_probe_state";
inline constexpr kernel::Dim3 kStateWorkgroup{64u, 1u, 1u};
/// RTXGI's "no hit" sentinel for the closest-distance searches.
inline constexpr f32 kStateFar = 1e27f;

struct ProbeStateParams {
    kernel::Span<const u32> probe_indices;
    u32 probe_count = 0;
    kernel::Span<const math::Vec3> ray_dirs;
    kernel::Span<const f32> distance; ///< slot * rays + ray, signed (trace output)
    math::Vec4* probe_data = nullptr; ///< updated in place
    math::Vec3 spacing{1.f, 1.f, 1.f};
    f32 max_distance = 20.f;               ///< a distance >= this is a miss (no surface)
    f32 backface_distance_scale = 0.2f;    ///< undone for the closest-backface distance
    f32 min_frontface_distance = 0.1f;
    f32 backface_threshold = 0.25f;
    f32 max_offset = 0.45f;                ///< fraction of the spacing
    f32 relocation_step = 1.f;             ///< longest move toward the farthest front face per update
    bool relocation = false;
    bool classification = false;
};

/// New (offset, state) of one probe from its update's ray results. RTXGI ProbeRelocationCS then
/// ProbeClassificationCS, on all rays of the update (RTXGI uses its unrotated "fixed" rays; this volume's
/// ray set is rotated every update, so the decisions see a new sample of directions each time).
FUSE_HOST_DEVICE inline math::Vec4 update_probe_state(const ProbeStateParams& p,
                                                      const math::Vec3* dirs,
                                                      const f32* distances,
                                                      u32 ray_count,
                                                      const math::Vec4& data) {
    math::Vec3 offset{data.x, data.y, data.z};
    f32 state = data.w;
    u32 closest_back = 0xFFFFFFFFu;
    u32 closest_front = 0xFFFFFFFFu;
    u32 farthest_front = 0xFFFFFFFFu;
    f32 closest_back_distance = kStateFar;
    f32 closest_front_distance = kStateFar;
    f32 farthest_front_distance = 0.f;
    u32 backfaces = 0u;
    const f32 undo = p.backface_distance_scale > 0.f ? 1.f / p.backface_distance_scale : 1.f;
    for (u32 r = 0; r < ray_count; ++r) {
        const f32 d = distances[r];
        if (d < 0.f) {
            ++backfaces;
            const f32 full = -d * undo;
            if (full < closest_back_distance) {
                closest_back_distance = full;
                closest_back = r;
            }
        } else if (d < closest_front_distance) {
            closest_front_distance = d;
            closest_front = r;
        } else if (d > farthest_front_distance) {
            farthest_front_distance = d;
            farthest_front = r;
        }
    }
    const f32 backface_ratio = ray_count > 0u ? static_cast<f32>(backfaces) / static_cast<f32>(ray_count) : 0.f;
    const bool inside = backface_ratio > p.backface_threshold;

    if (p.relocation) {
        bool moved = false;
        math::Vec3 full{};
        if (closest_back != 0xFFFFFFFFu && inside) {
            // Inside geometry: step through the closest backface, half the minimum distance beyond it.
            const f32 step = closest_back_distance + p.min_frontface_distance * 0.5f;
            full = offset + dirs[closest_back] * step;
            moved = true;
        } else if (closest_front_distance < p.min_frontface_distance) {
            // Too close to a front face: move toward the farthest front face (never through it, at most
            // relocation_step per update), when that is away from the closest one.
            if (closest_front != 0xFFFFFFFFu && farthest_front != 0xFFFFFFFFu) {
                const math::Vec3& a = dirs[closest_front];
                const math::Vec3& b = dirs[farthest_front];
                if (a.x * b.x + a.y * b.y + a.z * b.z <= 0.f) {
                    full = offset + b * std::min(farthest_front_distance, p.relocation_step);
                    moved = true;
                }
            }
        } else if (closest_front_distance > p.min_frontface_distance) {
            // Clear of surfaces: drift back toward the grid position.
            const f32 len = std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
            if (len > 0.f) {
                const f32 margin = std::min(closest_front_distance - p.min_frontface_distance, len);
                const f32 inv = 1.f / len;
                full = offset + math::Vec3{-offset.x * inv, -offset.y * inv, -offset.z * inv} * margin;
                moved = true;
            }
        }
        if (moved) {
            const math::Vec3 n{full.x / p.spacing.x, full.y / p.spacing.y, full.z / p.spacing.z};
            if (n.x * n.x + n.y * n.y + n.z * n.z < p.max_offset * p.max_offset) {
                offset = full;
            }
        }
    }

    if (p.classification) {
        state = kProbeInactive;
        if (!inside) {
            // Active when a front face lies inside the probe's cell: the ray reaches it before the
            // nearest of the three planes one spacing away.
            for (u32 r = 0; r < ray_count; ++r) {
                const f32 d = distances[r];
                if (d < 0.f || d >= p.max_distance) {
                    continue;
                }
                const math::Vec3& dir = dirs[r];
                const f32 ax = std::fabs(dir.x);
                const f32 ay = std::fabs(dir.y);
                const f32 az = std::fabs(dir.z);
                const f32 px = ax > 0.f ? p.spacing.x / std::max(ax, 1e-6f) : kStateFar;
                const f32 py = ay > 0.f ? p.spacing.y / std::max(ay, 1e-6f) : kStateFar;
                const f32 pz = az > 0.f ? p.spacing.z / std::max(az, 1e-6f) : kStateFar;
                if (d <= std::min(px, std::min(py, pz))) {
                    state = kProbeActive;
                    break;
                }
            }
        }
    }
    return {offset.x, offset.y, offset.z, state};
}

struct ProbeStateKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const ProbeStateParams& p) const {
        const u32 slot = idx.global.x;
        if (slot >= p.probe_indices.size) {
            return;
        }
        const u32 probe = p.probe_indices[slot];
        if (probe >= p.probe_count) {
            return;
        }
        const u32 rays = p.ray_dirs.size;
        p.probe_data[probe] = update_probe_state(p, p.ray_dirs.data, p.distance.data + static_cast<usize>(slot) * rays, rays,
                                                 p.probe_data[probe]);
    }
};

inline kernel::KernelLaunch make_state_launch(u32 slots) {
    return kernel::KernelLaunch{kStateName, kernel::extent1(slots), kStateWorkgroup};
}

} // namespace fuse::renderer::ddgi_kernel
