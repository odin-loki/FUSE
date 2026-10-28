#pragma once

#include <fuse/types.hpp>

namespace fuse::gameplay {

struct Magazine {
    u32 rounds = 0;
    u32 capacity = 0;
    f32 reloadRemaining = 0.f;
    f32 reloadDuration = 0.f;
};

/// Consumes one round when rounds remain and no reload is in progress.
[[nodiscard]] inline bool try_fire(Magazine& mag) {
    if (mag.rounds > 0u && mag.reloadRemaining <= 0.f) {
        --mag.rounds;
        return true;
    }
    return false;
}

/// Starts a reload. False when full, already reloading, or reloadDuration is not positive.
[[nodiscard]] inline bool start_reload(Magazine& mag) {
    if (mag.rounds >= mag.capacity || !(mag.reloadRemaining <= 0.f) || !(mag.reloadDuration > 0.f)) {
        return false;
    }
    mag.reloadRemaining = mag.reloadDuration;
    return true;
}

/// Advances an in-progress reload. dt <= 0 leaves the magazine unchanged.
/// When the timer reaches zero, the magazine refills to capacity.
inline void tick_reload(Magazine& mag, f32 dt) {
    if (!(dt > 0.f) || !(mag.reloadRemaining > 0.f)) {
        return;
    }
    if (dt >= mag.reloadRemaining) {
        mag.reloadRemaining = 0.f;
        mag.rounds = mag.capacity;
        return;
    }
    mag.reloadRemaining -= dt;
}

} // namespace fuse::gameplay
