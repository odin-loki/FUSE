#pragma once

#include <cmath>

namespace fuse::math {

/// Linear remap of `v` from [in0, in1] onto [out0, out1], unclamped.
/// Any non-finite argument returns `out0` when `out0` is finite, otherwise 0.
/// Equal input endpoints return `out0`.
[[nodiscard]] inline float remap_span(float v, float in0, float in1, float out0, float out1) {
    if (!std::isfinite(v) || !std::isfinite(in0) || !std::isfinite(in1) || !std::isfinite(out0) ||
        !std::isfinite(out1)) {
        return std::isfinite(out0) ? out0 : 0.f;
    }
    if (in0 == in1) {
        return out0;
    }
    return out0 + (v - in0) / (in1 - in0) * (out1 - out0);
}

} // namespace fuse::math
