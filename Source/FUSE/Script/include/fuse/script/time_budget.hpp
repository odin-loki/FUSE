#pragma once

#include <cmath>

namespace fuse::script {

/// True when elapsed `milliseconds` exceeds `budgetMs`.
/// Non-finite milliseconds returns true.
/// A non-positive or non-finite budget returns true when milliseconds is greater than 0,
/// and false when milliseconds is 0 or negative and finite.
/// Otherwise true when milliseconds is greater than budgetMs.
[[nodiscard]] inline bool over_budget(float milliseconds, float budgetMs) {
    if (!std::isfinite(milliseconds)) {
        return true;
    }
    if (!(budgetMs > 0.f) || !std::isfinite(budgetMs)) {
        return milliseconds > 0.f;
    }
    return milliseconds > budgetMs;
}

} // namespace fuse::script
