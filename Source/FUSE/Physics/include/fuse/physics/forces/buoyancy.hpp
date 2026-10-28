#pragma once

#include <fuse/physics/math.hpp>

#include <cmath>

namespace fuse::physics {

struct BuoyancySample {
    vec3 force{};
    f32 submergedFraction = 0.f;
};

/// Sphere buoyancy against a horizontal water plane.
/// Submerged fraction is the spherical-cap volume over the sphere volume:
/// h = radius - (centerY - waterY), fraction = h^2 * (3r - h) / (4 r^3), clamped to 0..1.
/// Fully submerged force is (0, -density * volume * gravityY, 0).
[[nodiscard]] inline BuoyancySample sphere_buoyancy(f32 centerY, f32 radius, f32 waterY, f32 fluidDensity,
                                                    f32 gravityY) {
    BuoyancySample sample;
    if (!std::isfinite(centerY) || !std::isfinite(radius) || !std::isfinite(waterY) ||
        !std::isfinite(fluidDensity) || !std::isfinite(gravityY) || radius <= 0.f) {
        return sample;
    }

    const f32 h = radius - (centerY - waterY);
    if (h <= 0.f) {
        sample.submergedFraction = 0.f;
    } else if (h >= 2.f * radius) {
        sample.submergedFraction = 1.f;
    } else {
        const f32 r3 = radius * radius * radius;
        sample.submergedFraction = (h * h * (3.f * radius - h)) / (4.f * r3);
    }

    if (fluidDensity <= 0.f || sample.submergedFraction <= 0.f) {
        return sample;
    }

    constexpr f32 kPi = 3.14159265f;
    const f32 volume = (4.f / 3.f) * kPi * radius * radius * radius;
    const f32 displacedMass = fluidDensity * volume * sample.submergedFraction;
    sample.force = vec3{0.f, -displacedMass * gravityY, 0.f};
    return sample;
}

} // namespace fuse::physics
