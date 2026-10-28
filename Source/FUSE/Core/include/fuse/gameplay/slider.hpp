#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::gameplay {

struct Slider {
    f32 position = 0.f;
    f32 speed = 0.f;
    f32 open = 0.f;
};

inline void tick(Slider& slider, f32 dt) {
    if (!std::isfinite(dt) || dt <= 0.f || !std::isfinite(slider.speed) || slider.speed <= 0.f ||
        !std::isfinite(slider.position) || !std::isfinite(slider.open)) {
        return;
    }
    f32 target = slider.open;
    if (target < 0.f) {
        target = 0.f;
    } else if (target > 1.f) {
        target = 1.f;
    }
    const f32 delta = target - slider.position;
    const f32 step = slider.speed * dt;
    if (std::fabs(delta) <= step) {
        slider.position = target;
    } else {
        slider.position += delta > 0.f ? step : -step;
    }
    if (slider.position < 0.f) {
        slider.position = 0.f;
    } else if (slider.position > 1.f) {
        slider.position = 1.f;
    }
}

[[nodiscard]] inline bool at_target(const Slider& slider) {
    return std::fabs(slider.position - slider.open) <= 1e-4f;
}

} // namespace fuse::gameplay
