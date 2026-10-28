#pragma once

namespace fuse::mechanics {

/// True when a provider stack count is in the inclusive range 0..99.
[[nodiscard]] inline bool provider_stack_ok(int count) {
    return count >= 0 && count <= 99;
}

} // namespace fuse::mechanics
