#pragma once

namespace fuse::ecs {

/// True when `orderA` is scheduled before `orderB`.
[[nodiscard]] constexpr bool system_before(int orderA, int orderB) {
    return orderA < orderB;
}

/// Schedule phase: 0 input (order < 0), 1 simulate (order < 100), 2 present.
[[nodiscard]] constexpr int system_phase(int order) {
    if (order < 0) {
        return 0;
    }
    if (order < 100) {
        return 1;
    }
    return 2;
}

} // namespace fuse::ecs
