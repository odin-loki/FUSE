#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::gameplay {

enum class ParryResult { Miss, Parry, Late };

struct ParryWindow {
    f32 start = 0.f;
    f32 duration = 0.f;
};

/// Half-open interval [start, start + duration).
/// A non-positive duration or any non-finite time is a miss.
/// A hit before start is a miss; a hit at or after the end is late.
[[nodiscard]] inline ParryResult try_parry(ParryWindow window, f32 hitTime) {
    if (!(window.duration > 0.f) || !std::isfinite(window.start) || !std::isfinite(window.duration) ||
        !std::isfinite(hitTime)) {
        return ParryResult::Miss;
    }
    if (hitTime < window.start) {
        return ParryResult::Miss;
    }
    if (hitTime >= window.start + window.duration) {
        return ParryResult::Late;
    }
    return ParryResult::Parry;
}

} // namespace fuse::gameplay
