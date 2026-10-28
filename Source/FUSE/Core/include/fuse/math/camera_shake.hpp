#pragma once

#include <fuse/math/vec.hpp>

#include <cmath>

namespace fuse::math {

struct ShakeState {
    f32 trauma = 0.f;
    u32 frame = 0u;
};

namespace detail {

/// Non-finite values become 0. Finite values clamp to [0, 1].
FUSE_HOST_DEVICE inline f32 clampTrauma(f32 trauma) {
    if (!std::isfinite(trauma) || trauma < 0.f) {
        return 0.f;
    }
    if (trauma > 1.f) {
        return 1.f;
    }
    return trauma;
}

/// Integer hash (Stafford / Murmur-inspired finalizer). Not rand().
FUSE_HOST_DEVICE inline u32 shakeHash(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// 23-bit hash mapped onto [-1, 1).
FUSE_HOST_DEVICE inline f32 hashSigned(u32 x) {
    const u32 bits = shakeHash(x) >> 9;
    return static_cast<f32>(bits) * (1.f / 4194304.f) - 1.f;
}

/// Weyl hash of `frame`. The high 24 bits change on every successive frame,
/// and the power-of-two scale keeps those integers on distinct floats in [-1, 1).
FUSE_HOST_DEVICE inline f32 frameWeylSigned(u32 frame) {
    const u32 bits = (frame * 0x9E3779B9u) >> 8;
    return static_cast<f32>(bits) * (1.f / 8388608.f) - 1.f;
}

} // namespace detail

/// Adds `amount` and clamps trauma to [0, 1]. A non-finite amount contributes 0.
FUSE_HOST_DEVICE inline void add_trauma(ShakeState& state, f32 amount) {
    if (!std::isfinite(amount)) {
        amount = 0.f;
    }
    state.trauma = detail::clampTrauma(state.trauma + amount);
}

/// Moves trauma toward 0 by `dt * decayPerSec` when both are finite and positive.
/// `dt <= 0` (and non-finite `dt`) leaves trauma unchanged. Otherwise the result is clamped to [0, 1].
FUSE_HOST_DEVICE inline void decay_trauma(ShakeState& state, f32 dt, f32 decayPerSec) {
    if (!std::isfinite(dt) || !(dt > 0.f)) {
        return;
    }
    f32 trauma = state.trauma;
    if (std::isfinite(trauma) && std::isfinite(decayPerSec) && decayPerSec > 0.f) {
        trauma -= dt * decayPerSec;
    }
    state.trauma = detail::clampTrauma(trauma);
}

/// Scale rule: offset = trauma^2 * direction(frame).
/// `direction` is a deterministic hash of `state.frame` (not rand()) with each component in [-1, 1],
/// so the offset magnitude scales with trauma^2. Trauma 0 yields the zero vector.
/// Same state in, same offset out. Successive frames hash to different directions.
FUSE_HOST_DEVICE inline Vec3 shake_offset(const ShakeState& state) {
    if (!std::isfinite(state.trauma) || !(state.trauma > 0.f)) {
        return {};
    }
    const f32 amplitude = state.trauma * state.trauma;
    const f32 x = detail::hashSigned(state.frame ^ 0xA511E9B3u);
    const f32 y = detail::hashSigned(state.frame ^ 0x63D83595u);
    const f32 z = detail::frameWeylSigned(state.frame);
    return {x * amplitude, y * amplitude, z * amplitude};
}

} // namespace fuse::math
