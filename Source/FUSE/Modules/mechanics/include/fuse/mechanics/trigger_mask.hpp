#pragma once

namespace fuse::mechanics {

/// True only for bits 0 through 31 inclusive.
[[nodiscard]] inline bool trigger_mask_ok(int bit) {
    return bit >= 0 && bit <= 31;
}

} // namespace fuse::mechanics
