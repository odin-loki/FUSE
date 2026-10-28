#pragma once

namespace fuse::renderer {

/// True only for tiers 0 through 3 inclusive.
[[nodiscard]] inline bool tier_index_ok(int tier) {
    return tier >= 0 && tier <= 3;
}

} // namespace fuse::renderer
