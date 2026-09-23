#include <fuse/renderer/atmosphere/sun_disk.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kDegreesToRadians = kPi / 180.f;

} // namespace

f32 sun_angular_radius_rad() {
    return (kSunApparentDiameterDegrees * 0.5f) * kDegreesToRadians;
}

f32 sun_disk_angular_separation_rad(const math::Vec3& view_dir, const math::Vec3& sun_dir) {
    // atan2(|a x b|, a . b) stays accurate for the sub-degree separations the disk edge needs;
    // acos(dot) loses most of its precision there in f32.
    const math::Vec3 a = view_dir.normalized();
    const math::Vec3 b = sun_dir.normalized();
    const f32 sin_theta = math::cross(a, b).length();
    const f32 cos_theta = a.dot(b);
    return std::atan2(sin_theta, cos_theta);
}

bool is_inside_sun_disk(f32 angular_separation_rad, f32 angular_radius_rad) {
    return angular_separation_rad < angular_radius_rad;
}

f32 sun_disk_radiance_factor(f32 angular_separation_rad, f32 angular_radius_rad) {
    if (angular_radius_rad <= 0.f || angular_separation_rad >= angular_radius_rad) {
        return 0.f;
    }

    const f32 rho = std::max(0.f, angular_separation_rad) / angular_radius_rad;
    const f32 mu = std::sqrt(std::max(0.f, 1.f - rho * rho));
    return 1.f - kSunLimbDarkeningU * (1.f - mu);
}

f32 sun_disk_weighted_solid_angle(f32 angular_radius_rad) {
    return kPi * angular_radius_rad * angular_radius_rad * (1.f - kSunLimbDarkeningU / 3.f);
}

f32 sun_disk_radiance_per_irradiance(f32 angular_separation_rad, f32 angular_radius_rad) {
    const f32 weight = sun_disk_weighted_solid_angle(angular_radius_rad);
    if (weight <= 0.f) {
        return 0.f;
    }
    return sun_disk_radiance_factor(angular_separation_rad, angular_radius_rad) / weight;
}

math::Vec3 composite_sun_disk(const math::Vec3& sky_radiance, const math::Vec3& view_dir,
                              const math::Vec3& sun_dir, const math::Vec3& sun_color, f32 sun_intensity) {
    const f32 separation = sun_disk_angular_separation_rad(view_dir, sun_dir);
    const f32 disk_factor = sun_disk_radiance_factor(separation);
    if (disk_factor <= 0.f) {
        return sky_radiance;
    }

    const math::Vec3 disk_radiance = sun_color * (sun_intensity * disk_factor);
    return sky_radiance + disk_radiance;
}

} // namespace fuse::renderer
