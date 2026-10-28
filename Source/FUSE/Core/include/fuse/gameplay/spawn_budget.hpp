#pragma once

// Concurrent spawn budget. `live` / `cap` is how many spawns are out.
// `tokens` is a bucket that refills in tick and pays for each successful try_spawn.
// cap == 0 rejects every spawn. release frees a live slot and does not refund a token.

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::gameplay {

struct SpawnBudget {
    u32 live = 0;
    u32 cap = 0;
    f32 tokens = 0.f;
    f32 tokensPerSec = 0.f;
    f32 tokenCap = 0.f;
};

/// Succeeds when cap is non-zero, live is below cap, and tokens is at least 1.
/// Increments live and spends one token. Otherwise leaves the budget unchanged.
/// A non-finite token count fails the `>= 1` check and must not spawn.
[[nodiscard]] inline bool try_spawn(SpawnBudget& budget) {
    if (budget.cap == 0u || budget.live >= budget.cap || !(budget.tokens >= 1.f)) {
        return false;
    }
    budget.live += 1u;
    budget.tokens -= 1.f;
    return true;
}

/// Frees one live slot. live stays at 0 when nothing is out.
inline void release(SpawnBudget& budget) {
    if (budget.live > 0u) {
        budget.live -= 1u;
    }
}

/// Adds tokensPerSec * dt, then clamps tokens up to tokenCap.
/// A non-positive or non-finite dt does nothing. A negative or non-finite
/// tokensPerSec adds nothing (treated as 0); a positive dt still clamps.
inline void tick(SpawnBudget& budget, f32 dt) {
    if (!std::isfinite(dt) || !(dt > 0.f)) {
        return;
    }
    const f32 rate = budget.tokensPerSec > 0.f ? budget.tokensPerSec : 0.f;
    budget.tokens += rate * dt;
    if (budget.tokens > budget.tokenCap) {
        budget.tokens = budget.tokenCap;
    }
}

} // namespace fuse::gameplay
