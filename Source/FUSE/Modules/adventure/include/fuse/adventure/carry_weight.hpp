#pragma once

// Carry weight. can_carry is true when limit, current, and added are finite,
// limit is positive, current and added are non-negative, and current + added
// stays within the limit. carry_left is unused capacity, or 0 when the inputs
// cannot describe a positive limit. A negative current counts as 0.

#include <cmath>

namespace fuse::adventure {

[[nodiscard]] inline bool can_carry(float current, float added, float limit) {
    if (!std::isfinite(current) || !std::isfinite(added) || !std::isfinite(limit)) {
        return false;
    }
    if (!(limit > 0.f) || current < 0.f || added < 0.f) {
        return false;
    }
    return current + added <= limit;
}

[[nodiscard]] inline float carry_left(float current, float limit) {
    if (!std::isfinite(current) || !std::isfinite(limit) || !(limit > 0.f)) {
        return 0.f;
    }
    if (current < 0.f) {
        current = 0.f;
    }
    if (!(current < limit)) {
        return 0.f;
    }
    return limit - current;
}

} // namespace fuse::adventure
