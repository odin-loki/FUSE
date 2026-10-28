#pragma once

// Stamina pool. spend rejects a non-finite, non-positive, or unaffordable amount and otherwise
// subtracts it and sets delayLeft to regenDelay. tick ignores a non-finite or non-positive dt.
// While delayLeft > 0, tick subtracts dt and does not regen, including the tick that reaches 0.
// When a tick begins with delayLeft already at 0, current restores toward max by regenPerSec * dt.
// regenPerSec < 0 is treated as 0. current is clamped to 0..max. max <= 0 keeps current at 0.

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::gameplay {

struct StaminaPool {
    f32 current = 0.f;
    f32 max = 0.f;
    f32 regenPerSec = 0.f;
    f32 regenDelay = 0.f;
    f32 delayLeft = 0.f;
};

/// False when amount is non-finite, non-positive, or greater than current.
[[nodiscard]] inline bool spend(StaminaPool& pool, f32 amount) {
    if (!std::isfinite(amount) || amount <= 0.f || pool.current < amount) {
        return false;
    }
    pool.current -= amount;
    pool.delayLeft = pool.regenDelay;
    return true;
}

/// Counts down the regen delay, then restores current toward max.
inline void tick(StaminaPool& pool, f32 dt) {
    if (!std::isfinite(dt) || dt <= 0.f) {
        return;
    }

    if (pool.delayLeft > 0.f) {
        pool.delayLeft -= dt;
        if (pool.delayLeft < 0.f) {
            pool.delayLeft = 0.f;
        }
    } else {
        f32 rate = pool.regenPerSec;
        if (rate < 0.f) {
            rate = 0.f;
        }
        if (std::isfinite(rate)) {
            pool.current += rate * dt;
        }
    }

    if (pool.max <= 0.f) {
        pool.current = 0.f;
        return;
    }
    if (pool.current < 0.f) {
        pool.current = 0.f;
    } else if (pool.current > pool.max) {
        pool.current = pool.max;
    }
}

} // namespace fuse::gameplay
