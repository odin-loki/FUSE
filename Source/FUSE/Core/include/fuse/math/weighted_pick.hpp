#pragma once

#include <fuse/types.hpp>

#include <cmath>
#include <cstddef>
#include <optional>

namespace fuse::math {

/// `u` is a normalized sample in 0..1. Negative and non-finite weights count as zero.
/// u == 1 lands on the last positive weight. No RNG.
[[nodiscard]] inline std::optional<std::size_t> weighted_index(const f32* weights, std::size_t count, f32 u) {
    if (weights == nullptr || count == 0) {
        return std::nullopt;
    }
    f32 total = 0.f;
    std::size_t lastPositive = 0;
    bool any = false;
    for (std::size_t i = 0; i < count; ++i) {
        const f32 w = (std::isfinite(weights[i]) && weights[i] > 0.f) ? weights[i] : 0.f;
        if (w > 0.f) {
            total += w;
            lastPositive = i;
            any = true;
        }
    }
    if (!any || !std::isfinite(total) || total <= 0.f) {
        return std::nullopt;
    }
    if (!std::isfinite(u)) {
        return std::nullopt;
    }
    if (u < 0.f) {
        u = 0.f;
    } else if (u > 1.f) {
        u = 1.f;
    }
    if (u >= 1.f) {
        return lastPositive;
    }
    const f32 target = u * total;
    f32 cumulative = 0.f;
    for (std::size_t i = 0; i < count; ++i) {
        const f32 w = (std::isfinite(weights[i]) && weights[i] > 0.f) ? weights[i] : 0.f;
        cumulative += w;
        if (w > 0.f && cumulative >= target) {
            return i;
        }
    }
    return lastPositive;
}

} // namespace fuse::math
