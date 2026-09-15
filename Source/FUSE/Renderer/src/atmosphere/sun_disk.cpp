#include <fuse/renderer/atmosphere/sun_disk.hpp>

#include <cmath>

namespace fuse::renderer {
namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kDegreesToRadians = kPi / 180.f;

f32 smoothstep(f32 edge0, f32 edge1, f32 x) {
    const f32 t = std::max(0.f, std::min(1.f, (x - edge0) / std::max(1e-8f, edge1 - edge0)));
    return t * t * (3.f - 2.f * t);
}

} // namespace

f32 sun_angular_radius_rad() {
    return (kSunApparentDiameterDegrees * 0.5f) * kDegreesToRadians;
}

f32 sun_disk_angular_separation_rad(const math::Vec3& view_dir, const math::Vec3& sun_dir) {
    const f32 cos_theta = std::max(-1.f, std::min(1.f, view_dir.normalized().dot(sun_dir.normalized())));
    return std::acos(cos_theta);
}

bool is_inside_sun_disk(f32 angular_separation_rad, f32 angular_radius_rad) {
    return angular_separation_rad <= angular_radius_rad;
}

f32 sun_disk_radiance_factor(f32 angular_separation_rad, f32 angular_radius_rad) {
    if (angular_separation_rad >= angular_radius_rad) {
        return 0.f;
    }

    const f32 t = 1.f - (angular_separation_rad / std::max(1e-8f, angular_radius_rad));
    return smoothstep(0.f, 1.f, t);
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
