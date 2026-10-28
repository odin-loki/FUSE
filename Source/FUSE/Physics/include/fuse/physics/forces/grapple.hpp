#pragma once

#include <fuse/physics/math.hpp>

#include <cmath>

namespace fuse::physics {

struct Grapple {
    vec3 anchor{};
    f32 restLength = 0.f;
    f32 stiffness = 0.f;
};

/// Slack tether. While distance d from the anchor is within restLength the force is zero.
/// Past that, F = normalize(anchor - position) * stiffness * (d - restLength), toward the anchor.
/// Non-positive stiffness, a negative rest length, a zero direction, or any non-finite input
/// returns a zero force.
[[nodiscard]] inline vec3 grapple_force(const Grapple& g, vec3 position) {
    if (!std::isfinite(g.anchor.x) || !std::isfinite(g.anchor.y) || !std::isfinite(g.anchor.z) ||
        !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
        !std::isfinite(g.restLength) || !std::isfinite(g.stiffness)) {
        return {};
    }
    if (!(g.stiffness > 0.f) || g.restLength < 0.f) {
        return {};
    }

    const vec3 delta = g.anchor - position;
    const f32 distSq = delta.dot(delta);
    if (!std::isfinite(distSq) || !(distSq > 0.f)) {
        return {};
    }
    const f32 d = std::sqrt(distSq);
    if (!std::isfinite(d) || d <= g.restLength) {
        return {};
    }

    const f32 scale = g.stiffness * (d - g.restLength) / d;
    if (!std::isfinite(scale)) {
        return {};
    }
    return delta * scale;
}

} // namespace fuse::physics
