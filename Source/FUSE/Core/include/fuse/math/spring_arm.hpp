#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::math {

/// Scalar spring. `value` is position; `velocity` is its time derivative.
struct Spring1D {
    f32 value = 0.f;
    f32 velocity = 0.f;
};

/// Advance a critically damped spring toward a stationary `target`.
///
/// Closed form: the analytic critically damped harmonic oscillator (damping
/// ratio zeta = 1). Error y = value - target obeys y'' + 2*w*y' + w^2*y = 0.
/// That ODE has a repeated characteristic root r = -w, so
///   y(t) = (y0 + (v0 + w*y0)*t) * exp(-w*t)
///   v(t) = (v0 - w*(v0 + w*y0)*t) * exp(-w*t)
/// `omega` is the angular frequency w in radians per second. This is the
/// stationary-target case of the critically damped spring (Ryan Juckett,
/// "Damped Springs"; the same update as FMath::CriticallyDampedSmoothing).
/// dt <= 0, omega <= 0, and any non-finite input return `state` unchanged.
FUSE_HOST_DEVICE inline Spring1D step_spring(Spring1D state, f32 target, f32 omega, f32 dt) {
    if (!(dt > 0.f) || !(omega > 0.f) || !std::isfinite(dt) || !std::isfinite(omega) ||
        !std::isfinite(target) || !std::isfinite(state.value) || !std::isfinite(state.velocity)) {
        return state;
    }

    const f32 y0 = state.value - target;
    if (!std::isfinite(y0)) {
        return state;
    }

    const f32 decay = std::exp(-omega * dt);
    const f32 b = state.velocity + omega * y0;
    const f32 y = (y0 + b * dt) * decay;
    const f32 velocity = (state.velocity - omega * b * dt) * decay;
    // A very long step underflows exp(-w*t) to 0. An overflowing polynomial then
    // yields NaN; the closed-form limit is the settled spring.
    if (!std::isfinite(y) || !std::isfinite(velocity)) {
        return decay == 0.f ? Spring1D{target, 0.f} : state;
    }
    return Spring1D{target + y, velocity};
}

} // namespace fuse::math
