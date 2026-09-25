#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Physical sun apparent diameter per P5 §5.8 acceptance gate (0.5° total).
static constexpr f32 kSunApparentDiameterDegrees = 0.5f;

/// Linear limb-darkening coefficient `u` for the visible band: I(mu) / I(1) = 1 - u * (1 - mu),
/// where mu is the cosine of the emission angle on the solar surface.
static constexpr f32 kSunLimbDarkeningU = 0.6f;

/// Angular radius in radians (half apparent diameter).
f32 sun_angular_radius_rad();

/// Angular separation between view and sun directions in radians.
f32 sun_disk_angular_separation_rad(const math::Vec3& view_dir, const math::Vec3& sun_dir);

/// True when the view ray lies inside the sun disk cone.
bool is_inside_sun_disk(f32 angular_separation_rad, f32 angular_radius_rad = sun_angular_radius_rad());

/// Limb-darkened relative radiance: 1 at disk centre, `1 - kSunLimbDarkeningU` just inside the limb,
/// exactly 0 at and beyond the angular radius (hard physical edge at 0.25°).
f32 sun_disk_radiance_factor(f32 angular_separation_rad, f32 angular_radius_rad = sun_angular_radius_rad());

/// Solid-angle integral of `sun_disk_radiance_factor` over the disk (steradians):
/// pi * r^2 * (1 - u / 3) for the linear limb-darkening law (small-angle form).
f32 sun_disk_weighted_solid_angle(f32 angular_radius_rad = sun_angular_radius_rad());

/// Sun disk radiance (per unit solar irradiance at the observer) for a view ray: the limb-darkened
/// profile normalised so that its integral over the disk equals 1.
f32 sun_disk_radiance_per_irradiance(f32 angular_separation_rad,
                                     f32 angular_radius_rad = sun_angular_radius_rad());

/// Composite sun disk radiance over scattered sky colour.
math::Vec3 composite_sun_disk(const math::Vec3& sky_radiance, const math::Vec3& view_dir,
                              const math::Vec3& sun_dir, const math::Vec3& sun_color, f32 sun_intensity);

} // namespace fuse::renderer
