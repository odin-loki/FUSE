#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Physical sun apparent diameter per P5 §5.8 acceptance gate (0.5° total).
static constexpr f32 kSunApparentDiameterDegrees = 0.5f;

/// Angular radius in radians (half apparent diameter).
f32 sun_angular_radius_rad();

/// Angular separation between view and sun directions in radians.
f32 sun_disk_angular_separation_rad(const math::Vec3& view_dir, const math::Vec3& sun_dir);

/// True when the view ray lies inside the sun disk cone.
bool is_inside_sun_disk(f32 angular_separation_rad, f32 angular_radius_rad = sun_angular_radius_rad());

/// Smooth radiance factor — 1 at disk centre, 0 outside angular radius (CPU stub).
f32 sun_disk_radiance_factor(f32 angular_separation_rad, f32 angular_radius_rad = sun_angular_radius_rad());

/// Composite sun disk radiance over scattered sky colour.
math::Vec3 composite_sun_disk(const math::Vec3& sky_radiance, const math::Vec3& view_dir,
                              const math::Vec3& sun_dir, const math::Vec3& sun_color, f32 sun_intensity);

} // namespace fuse::renderer
