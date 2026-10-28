#pragma once

#include <fuse/math/vec.hpp>

#include <cmath>
#include <numbers>

namespace fuse::math {

/// True when `target` lies in the cone of `halfAngleRadians` and `range` from `origin`
/// along `forward`. `forward` is normalized. A target at `origin` is inside when the
/// cone parameters are valid, because the viewer occupies that point.
[[nodiscard]] inline bool in_view_cone(Vec3 origin, Vec3 forward, Vec3 target, f32 range,
                                        f32 halfAngleRadians) {
    const auto finiteVec = [](Vec3 v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    if (!finiteVec(origin) || !finiteVec(forward) || !finiteVec(target) || !std::isfinite(range) ||
        !std::isfinite(halfAngleRadians)) {
        return false;
    }

    constexpr f32 kPi = std::numbers::pi_v<f32>;
    if (!(range > 0.f) || !(halfAngleRadians > 0.f) || halfAngleRadians > kPi) {
        return false;
    }

    const Vec3 fwd = forward.normalized();
    if (fwd.dot(fwd) == 0.f) {
        return false;
    }

    const Vec3 offset = target - origin;
    const f32 distance = offset.length();
    if (!std::isfinite(distance)) {
        return false;
    }
    // Direction is undefined when the target sits on the viewer.
    if (!(distance > 0.f)) {
        return true;
    }

    const Vec3 dir = offset.normalized();
    if (dir.dot(dir) == 0.f) {
        return true;
    }

    return distance <= range && dir.dot(fwd) >= std::cos(halfAngleRadians);
}

} // namespace fuse::math
