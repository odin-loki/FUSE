#pragma once

namespace fuse::renderer {

/// True only for pixel counts 1 through 4096 inclusive.
[[nodiscard]] inline bool readback_px_ok(int px) {
    return px >= 1 && px <= 4096;
}

} // namespace fuse::renderer
