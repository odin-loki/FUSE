#pragma once

// Whole frame budgets spanned by one frame. Independent of the world runtime.

#include <cmath>
#include <cstdint>
#include <limits>

namespace fuse::world3d {

/// Whole budgets in `frameSeconds` (`floor(frameSeconds / budgetSeconds)`).
/// A non-finite or non-positive budget returns 0. A non-positive frameSeconds returns 0.
/// Counts above UINT32_MAX saturate.
[[nodiscard]] inline std::uint32_t hitch_frames(float frameSeconds, float budgetSeconds) {
    if (!std::isfinite(budgetSeconds) || !(budgetSeconds > 0.f)) {
        return 0u;
    }
    if (!(frameSeconds > 0.f)) {
        return 0u;
    }
    const double frames =
        std::floor(static_cast<double>(frameSeconds) / static_cast<double>(budgetSeconds));
    constexpr double kLimit = static_cast<double>(std::numeric_limits<std::uint32_t>::max()) + 1.0;
    if (!(frames < kLimit)) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(frames);
}

} // namespace fuse::world3d
