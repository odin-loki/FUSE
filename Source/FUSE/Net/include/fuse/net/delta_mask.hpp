#pragma once

namespace fuse::net {

/// True only for masks in the closed range 0..63.
[[nodiscard]] inline bool delta_mask_ok(int mask) noexcept {
    return mask >= 0 && mask <= 63;
}

} // namespace fuse::net
