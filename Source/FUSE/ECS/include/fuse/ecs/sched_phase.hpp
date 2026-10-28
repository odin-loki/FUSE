#pragma once

namespace fuse::ecs {

/// True when `phase` is in the inclusive range 0..7.
[[nodiscard]] inline constexpr bool sched_phase_ok(int phase) {
    return phase >= 0 && phase <= 7;
}

} // namespace fuse::ecs
