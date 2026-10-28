#pragma once

#include <cmath>

namespace fuse::editor {

/// True when an inspector float edit should be recorded.
/// The absolute difference must be greater than epsilon. A negative epsilon is treated as 0.
/// If either value is non-finite, the edit is dirty unless both are non-finite the same way:
/// both NaN is not dirty, matching infinities are not dirty, and +inf versus -inf is dirty.
inline bool property_dirty(float before, float after, float epsilon) {
    if (std::isnan(before) || std::isnan(after)) {
        return !(std::isnan(before) && std::isnan(after));
    }
    if (!std::isfinite(before) || !std::isfinite(after)) {
        return before != after;
    }
    if (epsilon < 0.f) {
        epsilon = 0.f;
    }
    return std::fabs(before - after) > epsilon;
}

} // namespace fuse::editor
