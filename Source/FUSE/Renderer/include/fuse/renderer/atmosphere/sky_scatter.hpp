#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/atmosphere/atmosphere_params.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Mie single-scattering albedo: Mie extinction = mie_coeff / albedo (aerosols absorb ~10%).
static constexpr f32 kMieSingleScatteringAlbedo = 0.9f;

/// Optical-depth integrals along a ray, split by species (metres of scale-height-weighted path).
/// Multiply `rayleigh` by `rayleigh_coeff` and `mie` by the Mie extinction to get optical depth.
struct AtmosphereDensityPath {
    f32 rayleigh = 0.f;
    f32 mie = 0.f;
};

/// CPU reference scatter helpers — mirrors renderer/atmosphere/sky.cuh (B5.8).
/// Rayleigh phase 3/(16 pi) (1 + cos^2); integrates to 1 over the sphere.
f32 rayleigh_phase(f32 cos_theta);
/// Cornette-Shanks (Henyey-Greenstein family) Mie phase; integrates to 1 over the sphere.
f32 mie_phase(f32 cos_theta, f32 g);

/// Per-channel Mie extinction coefficient (1/m) at sea level.
f32 atmosphere_mie_extinction(const AtmosphereParams& params);

/// Height above the ground (metres) at distance `t` along a ray that starts at radius `r0`
/// (distance from the planet centre) with cos(zenith) `mu`. Cancellation-free in f32.
f32 atmosphere_height_along_ray(f32 r0, f32 mu, f32 t, const AtmosphereParams& params);

/// Distance from radius `r0` along cos(zenith) `mu` to the top of the atmosphere (0 when outside
/// and not entering).
f32 atmosphere_distance_to_top(f32 r0, f32 mu, const AtmosphereParams& params);

/// Distance to the ground sphere, or a negative value when the ray misses the planet.
f32 atmosphere_distance_to_ground(f32 r0, f32 mu, const AtmosphereParams& params);

/// Density-weighted path length along [0, length] from radius `r0`, cos(zenith) `mu`.
/// Uses `steps` segments with log-linear density interpolation (exact for a flat exponential slab).
AtmosphereDensityPath atmosphere_density_path(f32 r0, f32 mu, f32 length, u32 steps,
                                              const AtmosphereParams& params);

/// Per-channel optical depth for a density path (Rayleigh RGB + grey Mie extinction).
math::Vec3 atmosphere_optical_depth(const AtmosphereDensityPath& path, const AtmosphereParams& params);

/// Transmittance from radius `r0` along cos(zenith) `mu` to the top of the atmosphere;
/// zero when the planet occludes the ray.
math::Vec3 atmosphere_transmittance_to_top(f32 r0, f32 mu, u32 steps, const AtmosphereParams& params);

/// Sun transmittance at radius `r` for sun cos(zenith) `mu_sun`: transmittance along the sun ray
/// (clamped to grazing) times the fraction of the 0.5° disk above the planet horizon, so the planet
/// shadow is a continuous penumbra instead of a hard step.
math::Vec3 atmosphere_sun_transmittance(f32 r, f32 mu_sun, u32 steps, const AtmosphereParams& params);

/// Single-scattering in-scattered radiance (Rayleigh + Mie) along a view ray, per unit solar
/// irradiance. `ray_origin` is relative to the ground point under the camera (y = altitude in metres,
/// clamped to >= 1 m). Integrates `view_samples` along the view ray and `light_samples` towards the
/// sun. View samples follow t = t_end * u^2 (dense near the camera, where the haze is). The planet
/// shadows the sun and terminates below-horizon rays; the ground itself is not lit.
math::Vec3 compute_sky_inscatter(const math::Vec3& ray_origin, const math::Vec3& ray_dir,
                                 const math::Vec3& sun_direction, const AtmosphereParams& params);

/// Sky radiance per unit solar irradiance: in-scatter plus the transmitted, limb-darkened 0.5° sun disk.
math::Vec3 compute_sky_colour(const math::Vec3& ray_origin, const math::Vec3& ray_dir,
                              const math::Vec3& sun_direction, const AtmosphereParams& params);

} // namespace fuse::renderer
