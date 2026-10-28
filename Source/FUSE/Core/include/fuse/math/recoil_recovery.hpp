#pragma once

#include <fuse/math/vec.hpp>

#include <cmath>

namespace fuse::math {

[[nodiscard]] inline Vec2 recover_recoil(Vec2 offset, f32 recoverPerSec, f32 dt) {
    if (!std::isfinite(offset.x) || !std::isfinite(offset.y)) {
        return {};
    }
    if (!std::isfinite(dt) || !std::isfinite(recoverPerSec) || dt <= 0.f || recoverPerSec <= 0.f) {
        return offset;
    }
    const f32 step = recoverPerSec * dt;
    auto axis = [step](f32 value) {
        if (std::fabs(value) <= step) {
            return 0.f;
        }
        return value > 0.f ? value - step : value + step;
    };
    return {axis(offset.x), axis(offset.y)};
}

} // namespace fuse::math
