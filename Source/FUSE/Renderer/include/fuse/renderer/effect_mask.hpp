#pragma once

namespace fuse::renderer {

/// True only for masks 0 through 15 inclusive.
[[nodiscard]] inline bool effect_mask_ok(int mask) {
    return mask >= 0 && mask <= 15;
}

} // namespace fuse::renderer
