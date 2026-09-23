#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/gi/ddgi.hpp>
#include <fuse/renderer/material/material.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// CPU reference path for the DDGI probe trace + blend kernels (B5.6).
///
/// This is the host-side implementation of the math the CUDA kernels run: probe
/// ray generation (spherical Fibonacci with a per-update random rotation), tracing
/// against an analytic scene, octahedral irradiance + distance-moment texels with
/// hysteresis blending, the octahedral border-texel copy, and the Chebyshev-weighted
/// trilinear probe sample used by deferred shading. It runs without a GPU and is the
/// reference the device kernels are validated against.
///
/// Radiometric convention: irradiance texels store E / pi (the cosine-weighted mean
/// incoming radiance), so a Lambertian surface of albedo `rho` reflects
/// `rho * texel`; all `*Irradiance` query functions return E (texel * pi).

/// Lambertian surface description for the analytic probe-trace scene.
struct DdgiCpuSurface {
    fuse::math::Vec3 albedo{0.8f, 0.8f, 0.8f};
    /// Emitted radiance (W / (m^2 sr) in scene units), constant over the surface.
    fuse::math::Vec3 emissive{};
};

/// Diffuse albedo + emitted radiance from an authoring material (emissiveColor * emissiveIntensity).
DdgiCpuSurface ddgiSurfaceFromMaterial(const Material& material);

/// Axis-aligned solid box — walls, panels and blockers of the analytic scene.
struct DdgiCpuBox {
    fuse::math::Vec3 min{};
    fuse::math::Vec3 max{};
    DdgiCpuSurface surface{};
};

struct DdgiCpuHit {
    bool hit = false;
    bool backface = false;
    f32 t = 0.f;
    fuse::math::Vec3 position{};
    fuse::math::Vec3 normal{};
    u32 box_index = UINT32_MAX;
};

/// Analytic scene traced by probe rays: boxes, one directional sun and a uniform sky.
struct DdgiCpuScene {
    std::vector<DdgiCpuBox> boxes;
    /// Unit direction from the surface toward the sun.
    fuse::math::Vec3 sun_direction{0.f, 1.f, 0.f};
    /// Irradiance on a surface facing the sun (zero disables the sun).
    fuse::math::Vec3 sun_irradiance{};
    /// Radiance of rays that escape the scene.
    fuse::math::Vec3 sky_radiance{};

    u32 addBox(const fuse::math::Vec3& min, const fuse::math::Vec3& max, const DdgiCpuSurface& surface);
    /// Closest hit along `direction` (unit) within (t_min, t_max).
    bool intersect(const fuse::math::Vec3& origin,
                   const fuse::math::Vec3& direction,
                   f32 t_min,
                   f32 t_max,
                   DdgiCpuHit& out_hit) const;
    /// Any hit along `direction` (unit) within (t_min, t_max).
    bool occluded(const fuse::math::Vec3& origin, const fuse::math::Vec3& direction, f32 t_min, f32 t_max) const;
    /// Emitted + sun-lit (shadowed) Lambertian radiance leaving a front-face hit.
    fuse::math::Vec3 directRadiance(const DdgiCpuHit& hit) const;
};

/// Tunables of the CPU probe update/sample path (defaults follow the common DDGI reference).
struct DdgiCpuConfig {
    /// Exponent applied to cos(texel, ray) when blending distance moments.
    f32 distance_power = 50.f;
    /// Relative per-texel change that counts as a lighting discontinuity (local changes).
    f32 change_threshold = 0.25f;
    /// Relative change of the probe's mean texel that counts as a discontinuity (global changes).
    f32 probe_change_threshold = 0.1f;
    /// Hysteresis for every texel of a probe whose mean changed past `probe_change_threshold`:
    /// the history no longer describes the lighting, so it is (by default) discarded.
    f32 probe_change_hysteresis = 0.f;
    /// Hysteresis is reduced by this amount on a detected discontinuity (fast response).
    f32 change_hysteresis_drop = 0.75f;
    /// Absolute floor (E/pi units) below which texel changes are never treated as discontinuities.
    f32 change_floor = 1e-3f;
    /// Hit distance multiplier for backface hits (probe inside geometry).
    f32 backface_distance_scale = 0.2f;
    /// Offset of the sample point along the surface normal, in world units.
    f32 normal_bias = 0.1f;
    /// Probe-weight crush threshold for the visibility weight.
    f32 weight_crush_threshold = 0.2f;
    /// Add probe-sampled indirect light at ray hits (infinite bounces through the volume).
    bool multi_bounce = true;
    /// Seed mixed with the frame index for the per-update ray rotation.
    u64 rotation_seed = 0x9E3779B97F4A7C15ull;
    /// Texel value (E/pi) before a probe's first update.
    fuse::math::Vec3 initial_irradiance{};
};

/// Row-major 3x3 rotation used to rotate the spherical Fibonacci ray set.
struct DdgiRayRotation {
    fuse::math::Vec3 row0{1.f, 0.f, 0.f};
    fuse::math::Vec3 row1{0.f, 1.f, 0.f};
    fuse::math::Vec3 row2{0.f, 0.f, 1.f};

    fuse::math::Vec3 apply(const fuse::math::Vec3& v) const { return {row0.dot(v), row1.dot(v), row2.dot(v)}; }
};

namespace ddgi_cpu {

/// i-th of n spherical Fibonacci directions (unit, near-uniform over the sphere).
fuse::math::Vec3 sphericalFibonacci(u32 index, u32 count);
/// Uniform random rotation derived from `seed` (unit quaternion -> matrix).
DdgiRayRotation randomRotation(u64 seed);
/// Rotation used for the probe rays of update `frame_index`.
DdgiRayRotation updateRotation(u64 seed, u32 frame_index);
/// Direction of the centre of interior texel (x, y) of a `res` x `res` octahedral tile.
fuse::math::Vec3 texelDirection(u32 x, u32 y, u32 res);
/// Fill the 1-texel border of a (res+2)^2 tile from its interior (octahedral wrap).
/// `stride` elements separate rows; T is copy-assignable.
template <typename T>
void copyOctahedralBorder(T* tile, u32 res, u32 stride) {
    const u32 last = res + 1u;
    for (u32 i = 1u; i <= res; ++i) {
        const u32 mirror = last - i;
        tile[0u * stride + i] = tile[1u * stride + mirror];
        tile[last * stride + i] = tile[res * stride + mirror];
        tile[i * stride + 0u] = tile[mirror * stride + 1u];
        tile[i * stride + last] = tile[mirror * stride + res];
    }
    tile[0u * stride + 0u] = tile[res * stride + res];
    tile[0u * stride + last] = tile[res * stride + 1u];
    tile[last * stride + 0u] = tile[1u * stride + res];
    tile[last * stride + last] = tile[1u * stride + 1u];
}

} // namespace ddgi_cpu

struct DdgiCpuUpdateStats {
    u32 probes_updated = 0;
    u32 rays_traced = 0;
    /// Texels whose hysteresis was reduced by change detection.
    u32 fast_response_texels = 0;
};

/// CPU probe volume: irradiance + distance atlases with bordered octahedral tiles.
class DdgiCpuVolume {
public:
    bool init(const DDGIDesc& desc, const DdgiCpuConfig& config = {});
    void reset();

    bool isReady() const { return m_ready; }
    const DDGIDesc& desc() const { return m_desc; }
    const DdgiCpuConfig& config() const { return m_config; }
    DdgiCpuConfig& config() { return m_config; }
    u32 probeCount() const { return m_probe_count; }
    /// Bordered tile edge in texels: irradiance_res + 2.
    u32 irradianceTileSize() const { return m_desc.irradiance_res + 2u; }
    /// Bordered tile edge in texels: depth_res + 2.
    u32 distanceTileSize() const { return m_desc.depth_res + 2u; }

    /// Rolling update: schedule `probes_per_frame` probes for `frame_index` and update them.
    DdgiCpuUpdateStats update(const DdgiCpuScene& scene, u32 frame_index);
    /// Trace + blend an explicit probe list with the ray rotation for `frame_index`.
    DdgiCpuUpdateStats updateProbes(const DdgiCpuScene& scene,
                                    const u32* probe_indices,
                                    u32 probe_count,
                                    u32 frame_index);

    /// Number of blends applied to a probe since init.
    u32 probeUpdateCount(u32 probe_index) const;
    /// Bordered-tile texel (E/pi) of a probe; x, y in [0, res+2).
    fuse::math::Vec3 irradianceTexel(u32 probe_index, u32 x, u32 y) const;
    /// Bordered-tile distance moments (mean, mean^2) of a probe.
    fuse::math::Vec2 distanceTexel(u32 probe_index, u32 x, u32 y) const;
    /// Mean of a probe's interior irradiance texels (E/pi).
    fuse::math::Vec3 probeMeanTexel(u32 probe_index) const;
    /// Mean of a probe's interior distance moments.
    fuse::math::Vec2 probeMeanDistance(u32 probe_index) const;

    /// Bilinear octahedral read of one probe: irradiance E for surface normal `direction`.
    fuse::math::Vec3 probeIrradiance(u32 probe_index, const fuse::math::Vec3& direction) const;
    /// Bilinear octahedral read of one probe's distance moments along `direction`.
    fuse::math::Vec2 probeDistance(u32 probe_index, const fuse::math::Vec3& direction) const;
    /// World-space irradiance E at a surface point: trilinear over 8 probes with
    /// backface (wrap) and Chebyshev visibility weights.
    fuse::math::Vec3 sampleIrradiance(const fuse::math::Vec3& position, const fuse::math::Vec3& normal) const;

private:
    fuse::math::Vec3 traceRadiance(const DdgiCpuScene& scene,
                                   const fuse::math::Vec3& origin,
                                   const fuse::math::Vec3& direction,
                                   f32& out_distance) const;
    void blendProbe(u32 probe_index,
                    const fuse::math::Vec3* ray_dirs,
                    const fuse::math::Vec3* radiance,
                    const f32* distances,
                    u32 ray_count,
                    DdgiCpuUpdateStats& stats);
    usize irradianceOffset(u32 probe_index) const;
    usize distanceOffset(u32 probe_index) const;

    DDGIDesc m_desc{};
    DdgiCpuConfig m_config{};
    u32 m_probe_count = 0;
    std::vector<fuse::math::Vec3> m_irradiance;
    std::vector<fuse::math::Vec2> m_distance;
    std::vector<u32> m_update_counts;
    std::vector<fuse::math::Vec3> m_scratch_dirs;
    std::vector<fuse::math::Vec3> m_scratch_radiance;
    std::vector<f32> m_scratch_distance;
    std::vector<fuse::math::Vec3> m_scratch_texel_dirs;
    std::vector<fuse::math::Vec3> m_scratch_distance_dirs;
    std::vector<fuse::math::Vec3> m_scratch_incoming;
    std::vector<u8> m_scratch_valid;
    bool m_ready = false;
};

namespace ddgi_cpu {

/// Pack the volume's bordered irradiance tiles into the GPU atlas layout
/// (`ddgi_util::irradianceAtlasWidth` x `Height`, `ProbeGridLayout::probeIrradianceAtlasOrigin`) as
/// RGBA16F half bits (alpha 1). Texels are E/pi, as the CPU reference stores them.
bool packIrradianceAtlasRgba16f(const DdgiCpuVolume& volume, std::vector<u16>& out);
/// Pack the bordered distance-moment tiles (mean, mean^2) into the depth atlas layout as RG16F bits.
bool packDistanceAtlasRg16f(const DdgiCpuVolume& volume, std::vector<u16>& out);

} // namespace ddgi_cpu

} // namespace fuse::renderer
