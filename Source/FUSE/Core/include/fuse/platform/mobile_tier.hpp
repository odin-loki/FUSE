#pragma once

namespace fuse::platform {

/// True when `tier` is in the closed range 0..2.
[[nodiscard]] inline bool mobile_tier_ok(int tier) {
    return tier >= 0 && tier <= 2;
}

} // namespace fuse::platform
