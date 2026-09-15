#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/atmosphere/atmosphere_params.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// CPU reference scatter helpers — mirrors renderer/atmosphere/sky.cuh (B5.8 stub).
f32 rayleigh_phase(f32 cos_theta);
f32 mie_phase(f32 cos_theta, f32 g);

/// Single-scattering sky integral along a view ray (CPU stub; CUDA kernel deferred).
math::Vec3 compute_sky_colour(const math::Vec3& ray_origin, const math::Vec3& ray_dir,
                              const math::Vec3& sun_direction, const AtmosphereParams& params);

} // namespace fuse::renderer
