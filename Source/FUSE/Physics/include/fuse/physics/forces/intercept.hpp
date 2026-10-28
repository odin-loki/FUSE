#pragma once

#include <fuse/physics/math.hpp>

#include <cmath>
#include <optional>

namespace fuse::physics {

/// Earliest time t >= 0 at which a constant-speed shot from `shooter` meets a target that
/// starts at `target` and moves at constant `targetVelocity`.
///
/// |target + targetVelocity * t - shooter| = shotSpeed * t
/// is the quadratic a t^2 + b t + c = 0 with rel = target - shooter,
/// a = dot(v, v) - shotSpeed^2, b = 2 dot(rel, v), c = dot(rel, rel).
///
/// A non-positive or non-finite shot speed, or any non-finite vector component, returns
/// nullopt. A target already at the shooter returns 0. Two positive roots yield the
/// smaller one. A negative discriminant returns nullopt. A near-zero `a` (target speed
/// equals shot speed) uses the linear solution t = -c / b when b != 0 and t >= 0.
[[nodiscard]] inline std::optional<f32> intercept_time(vec3 shooter, vec3 target, vec3 targetVelocity,
                                                       f32 shotSpeed) {
    const bool vectorsFinite = std::isfinite(shooter.x) && std::isfinite(shooter.y) && std::isfinite(shooter.z) &&
                               std::isfinite(target.x) && std::isfinite(target.y) && std::isfinite(target.z) &&
                               std::isfinite(targetVelocity.x) && std::isfinite(targetVelocity.y) &&
                               std::isfinite(targetVelocity.z);
    if (!(shotSpeed > 0.f) || !std::isfinite(shotSpeed) || !vectorsFinite) {
        return std::nullopt;
    }

    const vec3 rel = target - shooter;
    const f32 c = rel.dot(rel);
    if (!std::isfinite(c)) {
        return std::nullopt;
    }
    // Already coincident: t = 0 satisfies the intercept equation for any finite velocity.
    if (c == 0.f) {
        return 0.f;
    }

    const f32 speedSq = targetVelocity.dot(targetVelocity);
    const f32 shotSq = shotSpeed * shotSpeed;
    if (!std::isfinite(speedSq) || !std::isfinite(shotSq) || !(shotSq > 0.f)) {
        return std::nullopt;
    }

    const f32 a = speedSq - shotSq;
    const f32 b = 2.f * rel.dot(targetVelocity);
    if (!std::isfinite(a) || !std::isfinite(b)) {
        return std::nullopt;
    }

    // |v| ~= shotSpeed makes the t^2 coefficient vanish; solve b t + c = 0 instead.
    const f32 speedScale = speedSq > shotSq ? speedSq : shotSq;
    const f32 aTolerance = 1.0e-6f * speedScale;
    if (std::fabs(a) <= aTolerance) {
        if (b == 0.f) {
            return std::nullopt;
        }
        const f32 t = -c / b;
        if (!std::isfinite(t) || t < 0.f) {
            return std::nullopt;
        }
        return t;
    }

    const f32 discriminant = b * b - 4.f * a * c;
    if (!std::isfinite(discriminant) || discriminant < 0.f) {
        return std::nullopt;
    }

    const f32 sqrtDisc = std::sqrt(discriminant);
    // Numerically stable roots: q = -0.5 * (b + sign(b) * sqrt(disc)), then q/a and c/q.
    const f32 q = -0.5f * (b + std::copysign(sqrtDisc, b == 0.f ? 1.f : b));
    if (!std::isfinite(q) || q == 0.f) {
        return std::nullopt;
    }

    const f32 tFromQ = q / a;
    const f32 tFromC = c / q;
    const bool firstOk = std::isfinite(tFromQ) && tFromQ >= 0.f;
    const bool secondOk = std::isfinite(tFromC) && tFromC >= 0.f;
    if (firstOk && secondOk) {
        return tFromQ < tFromC ? tFromQ : tFromC;
    }
    if (firstOk) {
        return tFromQ;
    }
    if (secondOk) {
        return tFromC;
    }
    return std::nullopt;
}

} // namespace fuse::physics
