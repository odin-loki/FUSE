// Path throughput cutoff. A path keeps tracing only while its throughput can still change the image.
#pragma once

#include <cmath>

namespace fuse::relight::render::pathtrace {

/// True when the path should continue.
/// Positive finite cutoff: `throughput` is finite and `throughput >= cutoff`.
/// Cutoff <= 0: `throughput` is finite and `throughput > 0`.
/// A non-finite throughput stops the path.
[[nodiscard]] inline bool path_continues(float throughput, float cutoff) {
    if (!std::isfinite(throughput)) {
        return false;
    }
    if (cutoff <= 0.f) {
        return throughput > 0.f;
    }
    if (!std::isfinite(cutoff)) {
        return false;
    }
    return throughput >= cutoff;
}

} // namespace fuse::relight::render::pathtrace
