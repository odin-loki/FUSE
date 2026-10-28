#pragma once

#include <cmath>

namespace fuse::compute {

/// Clamp a sphere-trace step into [minStep, maxStep].
///
/// Bounds are ordered first: if minStep > maxStep they are swapped. A non-finite step returns that
/// ordered minimum. The value is then clamped into the ordered range. A non-finite result is 0.
[[nodiscard]] inline float clamp_ray_step(float step, float minStep, float maxStep) {
    if (minStep > maxStep) {
        const float orderedMin = maxStep;
        maxStep = minStep;
        minStep = orderedMin;
    }

    float clamped = step;
    if (!std::isfinite(step) || step < minStep) {
        clamped = minStep;
    } else if (maxStep < step) {
        clamped = maxStep;
    }
    return std::isfinite(clamped) ? clamped : 0.f;
}

} // namespace fuse::compute
