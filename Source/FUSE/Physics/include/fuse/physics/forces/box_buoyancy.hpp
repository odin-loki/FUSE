#pragma once

#include <fuse/physics/math.hpp>

#include <cmath>

namespace fuse::physics {

struct BoxBuoyancy {
    vec3 force{};
    f32 submergedFraction = 0.f;
};

/// Axis-aligned box buoyancy against a horizontal water plane.
/// Submerged fraction is the fraction of the box height below the plane, clamped to 0..1.
/// Volume is width * depth * (2 * halfHeight). Displaced mass is density * volume * fraction.
/// Force is (0, -displacedMass * gravityY, 0) when density is positive. Non-positive extents
/// and non-finite inputs return a zero fraction and a zero force. Non-positive density still
/// reports the fraction and returns a zero force.
[[nodiscard]] inline BoxBuoyancy box_buoyancy(f32 centerY, f32 halfHeight, f32 waterY, f32 fluidDensity,
                                              f32 gravityY, f32 width, f32 depth) {
    BoxBuoyancy sample;
    if (!std::isfinite(centerY) || !std::isfinite(halfHeight) || !std::isfinite(waterY) ||
        !std::isfinite(fluidDensity) || !std::isfinite(gravityY) || !std::isfinite(width) ||
        !std::isfinite(depth) || halfHeight <= 0.f || width <= 0.f || depth <= 0.f) {
        return sample;
    }

    const f32 height = 2.f * halfHeight;
    const f32 submergedHeight = waterY - (centerY - halfHeight);
    if (submergedHeight <= 0.f) {
        sample.submergedFraction = 0.f;
    } else if (submergedHeight >= height) {
        sample.submergedFraction = 1.f;
    } else {
        sample.submergedFraction = submergedHeight / height;
    }

    if (fluidDensity <= 0.f || sample.submergedFraction <= 0.f) {
        return sample;
    }

    const f32 volume = width * depth * height;
    const f32 displacedMass = fluidDensity * volume * sample.submergedFraction;
    sample.force = vec3{0.f, -displacedMass * gravityY, 0.f};
    return sample;
}

} // namespace fuse::physics
