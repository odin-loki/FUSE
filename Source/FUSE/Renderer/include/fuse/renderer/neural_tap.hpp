#pragma once

namespace fuse::renderer {

/// True only for taps 0 through 7 inclusive.
[[nodiscard]] inline bool neural_tap_ok(int tap) {
    return tap >= 0 && tap <= 7;
}

} // namespace fuse::renderer
