#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::gameplay {

struct Cooldown {
    f32 remaining = 0.f;
    f32 duration = 0.f;
};

inline void start(Cooldown& cooldown, f32 duration) {
    if (!std::isfinite(duration) || duration <= 0.f) {
        cooldown.remaining = 0.f;
        cooldown.duration = 0.f;
        return;
    }
    cooldown.duration = duration;
    cooldown.remaining = duration;
}

inline void tick(Cooldown& cooldown, f32 dt) {
    if (!std::isfinite(dt) || dt <= 0.f) {
        return;
    }
    cooldown.remaining = cooldown.remaining - dt;
    if (cooldown.remaining < 0.f) {
        cooldown.remaining = 0.f;
    }
}

[[nodiscard]] inline bool ready(const Cooldown& cooldown) {
    return cooldown.remaining <= 0.f;
}

[[nodiscard]] inline bool try_trigger(Cooldown& cooldown) {
    if (!ready(cooldown) || cooldown.duration <= 0.f) {
        return false;
    }
    start(cooldown, cooldown.duration);
    return true;
}

} // namespace fuse::gameplay
