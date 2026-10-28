#pragma once

#include <cmath>

namespace fuse::net {

/// Clamp an RTT sample into [`minMs`, `maxMs`].
///
/// Bounds are ordered first: if `minMs` > `maxMs` they are swapped. A non-finite sample returns that
/// ordered minimum when it is finite, otherwise 0. A finite sample is clamped into the ordered range.
[[nodiscard]] inline float clamp_rtt_ms(float sample, float minMs, float maxMs) {
    if (minMs > maxMs) {
        const float orderedMin = maxMs;
        maxMs = minMs;
        minMs = orderedMin;
    }
    if (!std::isfinite(sample)) {
        return std::isfinite(minMs) ? minMs : 0.f;
    }
    if (sample < minMs) {
        return minMs;
    }
    if (maxMs < sample) {
        return maxMs;
    }
    return sample;
}

} // namespace fuse::net
