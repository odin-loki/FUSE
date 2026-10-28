// CPU alpha cutoff. Classifies a coverage value against two thresholds.
//
// alpha <= transparentAt is Transparent. alpha >= opaqueAt is Opaque. A value
// strictly between the thresholds is Unknown. When transparentAt > opaqueAt the
// thresholds are swapped, then each is clamped into 0..1. A non-finite alpha is
// Unknown. An equal pair classifies the shared value as Transparent.
#pragma once

#include <cmath>

namespace fuse::relight::render::pathtrace::omm {

enum class OpacityState { Transparent, Unknown, Opaque };

/// Classify `alpha`. `transparentAt` and `opaqueAt` are the cutoff pair. See the file comment.
[[nodiscard]] inline OpacityState opacity_state(float alpha, float transparentAt, float opaqueAt) {
    if (transparentAt > opaqueAt) {
        const float swapped = transparentAt;
        transparentAt = opaqueAt;
        opaqueAt = swapped;
    }
    const auto clamp01 = [](float x) {
        if (!(x >= 0.f)) {
            return 0.f;
        }
        if (x > 1.f) {
            return 1.f;
        }
        return x;
    };
    transparentAt = clamp01(transparentAt);
    opaqueAt = clamp01(opaqueAt);
    if (!std::isfinite(alpha)) {
        return OpacityState::Unknown;
    }
    if (alpha <= transparentAt) {
        return OpacityState::Transparent;
    }
    if (alpha >= opaqueAt) {
        return OpacityState::Opaque;
    }
    return OpacityState::Unknown;
}

} // namespace fuse::relight::render::pathtrace::omm
