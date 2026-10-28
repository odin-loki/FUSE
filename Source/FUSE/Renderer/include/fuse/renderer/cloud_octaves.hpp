#pragma once

namespace fuse::renderer {

/// True only for octave counts 1 through 8 inclusive.
[[nodiscard]] inline bool cloud_octaves_ok(int octaves) {
    return octaves >= 1 && octaves <= 8;
}

} // namespace fuse::renderer
