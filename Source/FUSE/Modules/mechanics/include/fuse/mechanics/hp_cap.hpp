#pragma once

namespace fuse::mechanics {

/// True when an HP cap is in the inclusive range 1..100000.
[[nodiscard]] inline bool hp_cap_ok(int hp) {
    return hp >= 1 && hp <= 100000;
}

} // namespace fuse::mechanics
