#include <fuse/renderer/atmosphere/sky_scatter.hpp>

#include <cmath>

namespace fuse::renderer {
namespace {

constexpr f32 kPi = 3.14159265358979323846f;

f32 clamp01(f32 value) {
    return std::max(0.f, std::min(1.f, value));
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

math::Vec3 compute_sky_colour(const math::Vec3& /*ray_origin*/, const math::Vec3& ray_dir,
                              const math::Vec3& sun_direction, const AtmosphereParams& params) {
    const math::Vec3 view_dir = ray_dir.normalized();
    const math::Vec3 sun_dir = sun_direction.normalized();
    const f32 cos_theta = clamp01(view_dir.dot(sun_dir));

    const f32 rayleigh = rayleigh_phase(cos_theta);
    const f32 mie = mie_phase(cos_theta, params.mie_scatter_dir);

    math::Vec3 colour = params.rayleigh_coeff * rayleigh + math::Vec3{params.mie_coeff, params.mie_coeff, params.mie_coeff} * mie;

    const f32 view_elevation = clamp01(view_dir.y * 0.5f + 0.5f);
    const f32 sun_elevation = clamp01(sun_dir.y * 0.5f + 0.5f);
    const f32 horizon_factor = clamp01(1.f - std::fabs(view_dir.y));
    colour = colour + math::Vec3{0.05f, 0.08f, 0.15f} * horizon_factor;
    colour = colour + math::Vec3{0.35f, 0.18f, 0.04f} * horizon_factor * sun_elevation;
    colour = colour + params.rayleigh_coeff * sun_elevation * 1e5f;

    const f32 max_channel = std::max(colour.x, std::max(colour.y, colour.z));
    if (max_channel > 1e-6f) {
        colour = colour * (1.f / max_channel);
    }

    return colour;
}

} // namespace fuse::renderer
