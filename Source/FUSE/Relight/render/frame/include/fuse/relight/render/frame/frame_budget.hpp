// Frame budget: a budget is valid only when it lies in [1, 8].
#pragma once

namespace fuse::relight {

/// True when `frames` lies in [1, 8].
[[nodiscard]] inline bool frame_budget_ok(int frames) {
    return frames >= 1 && frames <= 8;
}

} // namespace fuse::relight
