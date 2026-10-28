#pragma once

namespace fuse::relight::options {

/// True when `phase` is in the inclusive range 0..3.
[[nodiscard]] inline bool shutdown_phase_ok(int phase) {
    return phase >= 0 && phase <= 3;
}

} // namespace fuse::relight::options
