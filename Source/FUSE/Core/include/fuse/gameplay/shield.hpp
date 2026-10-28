#pragma once

// Shield absorbs incoming damage before anything else. Emptying it breaks the pool and
// starts rechargeDelay. Tick counts that delay down and, once it expires, refills toward max.

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::gameplay {

struct ShieldPool {
    f32 current = 0.f;
    f32 max = 0.f;
    f32 rechargePerSec = 0.f;
    f32 rechargeDelay = 0.f;
    f32 delayLeft = 0.f;
    bool broken = false;
};

/// Spends shield first. Returns the amount the shield absorbed, not any spill.
/// Non-positive or non-finite amounts absorb nothing. Hitting zero breaks the shield
/// and arms the recharge delay.
[[nodiscard]] inline f32 absorb(ShieldPool& pool, f32 amount) {
    if (!std::isfinite(amount) || amount <= 0.f) {
        return 0.f;
    }
    if (!std::isfinite(pool.current) || pool.current <= 0.f) {
        return 0.f;
    }
    const f32 used = pool.current < amount ? pool.current : amount;
    pool.current -= used;
    if (pool.current <= 0.f) {
        pool.current = 0.f;
        pool.broken = true;
        pool.delayLeft = pool.rechargeDelay;
    }
    return used;
}

/// Counts delayLeft down. A broken shield does not recharge while that delay is still
/// positive. Time left in the step after the delay hits zero clears broken and recharges
/// toward max. A non-positive timestep does nothing.
inline void tick(ShieldPool& pool, f32 dt) {
    if (!std::isfinite(dt) || dt <= 0.f) {
        return;
    }

    f32 rechargeDt = dt;
    if (std::isfinite(pool.delayLeft) && pool.delayLeft > 0.f) {
        if (pool.delayLeft > dt) {
            pool.delayLeft -= dt;
            if (pool.broken) {
                return;
            }
        } else {
            const f32 leftover = dt - pool.delayLeft;
            pool.delayLeft = 0.f;
            if (pool.broken) {
                rechargeDt = leftover;
            }
        }
    } else if (pool.delayLeft < 0.f || !std::isfinite(pool.delayLeft)) {
        pool.delayLeft = 0.f;
    }

    if (pool.broken) {
        if (pool.delayLeft > 0.f) {
            return;
        }
        pool.broken = false;
    }

    if (!(rechargeDt > 0.f) || !std::isfinite(pool.rechargePerSec) || pool.rechargePerSec <= 0.f ||
        !std::isfinite(pool.max)) {
        return;
    }
    if (!std::isfinite(pool.current)) {
        pool.current = 0.f;
    }
    if (pool.current >= pool.max) {
        return;
    }
    pool.current += pool.rechargePerSec * rechargeDt;
    if (!std::isfinite(pool.current) || pool.current > pool.max) {
        pool.current = pool.max;
    }
}

} // namespace fuse::gameplay
