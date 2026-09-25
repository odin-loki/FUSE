#include <fuse/renderer/atmosphere/sky_scatter.hpp>
#include <fuse/renderer/atmosphere/sun_disk.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

constexpr f32 kPi = 3.14159265358979323846f;
/// Camera altitude clamp — keeps the view ray inside the atmosphere shell.
constexpr f32 kMinCameraAltitude = 1.f;
/// Samples used for the camera-to-sun transmittance applied to the sun disk.
constexpr u32 kSunDiskTransmittanceSteps = 64u;

/// Integral of exp(-h/H) over a segment of length `length` whose end heights are `h_a`, `h_b`,
/// assuming log-density varies linearly along the segment (exact for straight rays in a flat slab).
f32 logLinearSegment(f32 h_a, f32 h_b, f32 length, f32 scale_height) {
    const f32 density_a = std::exp(-h_a / scale_height);
    const f32 d = (h_b - h_a) / scale_height;
    if (std::fabs(d) < 1e-6f) {
        return length * density_a * (1.f - 0.5f * d);
    }
    return length * density_a * (-std::expm1(-d) / d);
}

AtmosphereDensityPath segmentPath(f32 h_a, f32 h_b, f32 length, const AtmosphereParams& params) {
    return {logLinearSegment(h_a, h_b, length, params.rayleigh_scale_h),
            logLinearSegment(h_a, h_b, length, params.mie_scale_h)};
}

math::Vec3 expNeg(const math::Vec3& tau) {
    return {std::exp(-tau.x), std::exp(-tau.y), std::exp(-tau.z)};
}

math::Vec3 mul(const math::Vec3& a, const math::Vec3& b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

/// Fraction of the solar disk above the geometric horizon when its centre sits `offset` disk radii
/// above it (circular-segment area). Keeps the planet shadow continuous instead of a hard step.
f32 visibleSunFraction(f32 offset) {
    const f32 d = std::max(-1.f, std::min(1.f, offset));
    return 0.5f + (d * std::sqrt(std::max(0.f, 1.f - d * d)) + std::asin(d)) / kPi;
}

} // namespace

f32 rayleigh_phase(f32 cos_theta) {
    return (3.f / (16.f * kPi)) * (1.f + cos_theta * cos_theta);
}

f32 mie_phase(f32 cos_theta, f32 g) {
    const f32 g2 = g * g;
    const f32 denom = std::pow(1.f + g2 - 2.f * g * cos_theta, 1.5f);
    if (denom < 1e-8f) {
        return 0.f;
    }
    return (3.f / (8.f * kPi)) * ((1.f - g2) * (1.f + cos_theta * cos_theta)) / ((2.f + g2) * denom);
}

f32 atmosphere_mie_extinction(const AtmosphereParams& params) {
    return params.mie_coeff / kMieSingleScatteringAlbedo;
}

f32 atmosphere_height_along_ray(f32 r0, f32 mu, f32 t, const AtmosphereParams& params) {
    // r(t)^2 - R^2 = (r0 - R)(r0 + R) + t (2 r0 mu + t); h = (r^2 - R^2) / (r + R).
    const f32 radius_ground = params.earth_radius;
    const f32 h0 = r0 - radius_ground;
    const f32 numerator = h0 * (r0 + radius_ground) + t * (2.f * r0 * mu + t);
    const f32 r_sq = r0 * r0 + t * (2.f * r0 * mu + t);
    const f32 r = std::sqrt(std::max(0.f, r_sq));
    return numerator / std::max(1.f, r + radius_ground);
}

f32 atmosphere_distance_to_top(f32 r0, f32 mu, const AtmosphereParams& params) {
    const f32 radius_top = params.atmo_radius;
    const f32 inside = (radius_top - r0) * (radius_top + r0);
    const f32 disc = inside + r0 * r0 * mu * mu;
    if (disc < 0.f) {
        return 0.f;
    }
    const f32 root = std::sqrt(disc);
    // Far root -r0 mu + root, written without cancellation for upward rays.
    const f32 t = (mu > 0.f) ? inside / std::max(1e-6f, r0 * mu + root) : -r0 * mu + root;
    return std::max(0.f, t);
}

f32 atmosphere_distance_to_ground(f32 r0, f32 mu, const AtmosphereParams& params) {
    if (mu >= 0.f) {
        return -1.f;
    }
    const f32 radius_ground = params.earth_radius;
    const f32 above = (r0 - radius_ground) * (r0 + radius_ground);
    const f32 disc = r0 * r0 * mu * mu - above;
    if (disc < 0.f) {
        return -1.f;
    }
    // Near root -r0 mu - sqrt(disc) == above / (-r0 mu + sqrt(disc)).
    const f32 denom = -r0 * mu + std::sqrt(disc);
    if (denom <= 0.f) {
        return 0.f;
    }
    return std::max(0.f, above / denom);
}

AtmosphereDensityPath atmosphere_density_path(f32 r0, f32 mu, f32 length, u32 steps,
                                              const AtmosphereParams& params) {
    AtmosphereDensityPath path{};
    if (length <= 0.f) {
        return path;
    }
    const u32 segment_count = std::max(1u, steps);
    const f32 dt = length / static_cast<f32>(segment_count);
    f32 h_prev = atmosphere_height_along_ray(r0, mu, 0.f, params);
    for (u32 i = 1; i <= segment_count; ++i) {
        const f32 h_next = atmosphere_height_along_ray(r0, mu, dt * static_cast<f32>(i), params);
        const AtmosphereDensityPath segment = segmentPath(h_prev, h_next, dt, params);
        path.rayleigh += segment.rayleigh;
        path.mie += segment.mie;
        h_prev = h_next;
    }
    return path;
}

math::Vec3 atmosphere_optical_depth(const AtmosphereDensityPath& path, const AtmosphereParams& params) {
    const f32 mie = atmosphere_mie_extinction(params) * path.mie;
    return params.rayleigh_coeff * path.rayleigh + math::Vec3{mie, mie, mie};
}

math::Vec3 atmosphere_transmittance_to_top(f32 r0, f32 mu, u32 steps, const AtmosphereParams& params) {
    if (atmosphere_distance_to_ground(r0, mu, params) >= 0.f) {
        return {};
    }
    const f32 length = atmosphere_distance_to_top(r0, mu, params);
    return expNeg(atmosphere_optical_depth(atmosphere_density_path(r0, mu, length, steps, params), params));
}

math::Vec3 atmosphere_sun_transmittance(f32 r, f32 mu_sun, u32 steps, const AtmosphereParams& params) {
    // Horizon dip below the local horizontal: atan(sqrt(h (2R + h)) / R), cancellation-free.
    const f32 h = std::max(0.f, r - params.earth_radius);
    const f32 horizon_elevation = -std::atan(std::sqrt(h * (2.f * params.earth_radius + h)) / params.earth_radius);
    const f32 sun_elevation = std::asin(std::max(-1.f, std::min(1.f, mu_sun)));
    const f32 radius = sun_angular_radius_rad();
    const f32 fraction = visibleSunFraction((sun_elevation - horizon_elevation) / radius);
    if (fraction <= 0.f) {
        return {};
    }
    const f32 mu_clamped = std::max(mu_sun, std::sin(horizon_elevation + 1e-5f));
    const f32 length = atmosphere_distance_to_top(r, mu_clamped, params);
    const math::Vec3 transmittance =
        expNeg(atmosphere_optical_depth(atmosphere_density_path(r, mu_clamped, length, steps, params), params));
    return transmittance * fraction;
}

math::Vec3 compute_sky_inscatter(const math::Vec3& ray_origin, const math::Vec3& ray_dir,
                                 const math::Vec3& sun_direction, const AtmosphereParams& params) {
    const math::Vec3 view_dir = ray_dir.normalized();
    const math::Vec3 sun_dir = sun_direction.normalized();
    const f32 max_altitude = std::max(kMinCameraAltitude, params.atmo_radius - params.earth_radius - 1.f);
    const f32 altitude = std::max(kMinCameraAltitude, std::min(max_altitude, ray_origin.y));
    const f32 r0 = params.earth_radius + altitude;
    const f32 mu = view_dir.y;
    const f32 nu = view_dir.dot(sun_dir);

    const f32 t_ground = atmosphere_distance_to_ground(r0, mu, params);
    const f32 t_end = (t_ground >= 0.f) ? t_ground : atmosphere_distance_to_top(r0, mu, params);
    if (t_end <= 0.f) {
        return {};
    }
    const u32 view_samples = std::max(1u, params.view_samples);
    const f32 inv_samples = 1.f / static_cast<f32>(view_samples);
    const f32 phase_r = rayleigh_phase(nu);
    const f32 phase_m = mie_phase(nu, params.mie_scatter_dir);
    const math::Vec3 beta_r = params.rayleigh_coeff * phase_r;
    const f32 beta_m = params.mie_coeff * phase_m;

    // Quadratic sample distribution t = t_end * u^2: dense near the camera where the thin Mie layer
    // and the lower Rayleigh layer sit (uniform steps put the first sample above most of the haze).
    const auto distanceAt = [&](f32 u) { return t_end * u * u; };

    math::Vec3 radiance{};
    AtmosphereDensityPath view_path{};
    f32 t_start = 0.f;
    f32 h_start = atmosphere_height_along_ray(r0, mu, 0.f, params);
    for (u32 i = 0; i < view_samples; ++i) {
        const f32 t_mid = distanceAt((static_cast<f32>(i) + 0.5f) * inv_samples);
        const f32 t_next = distanceAt(static_cast<f32>(i + 1u) * inv_samples);
        const f32 dt = t_next - t_start;
        const f32 h_mid = atmosphere_height_along_ray(r0, mu, t_mid, params);
        const f32 h_next = atmosphere_height_along_ray(r0, mu, t_next, params);

        // Optical depth from the camera to this sample point.
        const AtmosphereDensityPath partial = segmentPath(h_start, h_mid, t_mid - t_start, params);
        AtmosphereDensityPath to_mid = view_path;
        to_mid.rayleigh += partial.rayleigh;
        to_mid.mie += partial.mie;
        const math::Vec3 view_transmittance = expNeg(atmosphere_optical_depth(to_mid, params));

        // Sun transmittance from the sample point, including the partially set sun disk.
        const f32 r_mid = params.earth_radius + h_mid;
        const f32 mu_sun = std::max(-1.f, std::min(1.f, (r0 * sun_dir.y + t_mid * nu) / r_mid));
        const math::Vec3 sun_transmittance =
            atmosphere_sun_transmittance(r_mid, mu_sun, params.light_samples, params);

        const f32 density_r = std::exp(-std::max(0.f, h_mid) / params.rayleigh_scale_h);
        const f32 density_m = std::exp(-std::max(0.f, h_mid) / params.mie_scale_h);
        const math::Vec3 scattering = beta_r * density_r + math::Vec3{beta_m, beta_m, beta_m} * density_m;
        radiance = radiance + mul(mul(view_transmittance, sun_transmittance), scattering) * dt;

        const AtmosphereDensityPath full = segmentPath(h_start, h_next, dt, params);
        view_path.rayleigh += full.rayleigh;
        view_path.mie += full.mie;
        t_start = t_next;
        h_start = h_next;
    }
    return radiance;
}

math::Vec3 compute_sky_colour(const math::Vec3& ray_origin, const math::Vec3& ray_dir,
                              const math::Vec3& sun_direction, const AtmosphereParams& params) {
    math::Vec3 colour = compute_sky_inscatter(ray_origin, ray_dir, sun_direction, params);

    const f32 separation = sun_disk_angular_separation_rad(ray_dir, sun_direction);
    const f32 disk = sun_disk_radiance_per_irradiance(separation);
    if (disk > 0.f) {
        const f32 altitude = std::max(kMinCameraAltitude, ray_origin.y);
        const f32 r0 = std::min(params.atmo_radius - 1.f, params.earth_radius + altitude);
        const math::Vec3 transmittance = atmosphere_transmittance_to_top(
            r0, ray_dir.normalized().y, kSunDiskTransmittanceSteps, params);
        colour = colour + transmittance * disk;
    }
    return colour;
}

} // namespace fuse::renderer
