#pragma once

#include <cstdint>

namespace fuse::adventure {

/// Different teams may always hit. The same team may hit only when friendly fire is on.
[[nodiscard]] inline bool friendly_hit_allowed(std::uint32_t attackerTeam,
                                               std::uint32_t targetTeam,
                                               bool friendlyFire) {
    return attackerTeam != targetTeam || friendlyFire;
}

} // namespace fuse::adventure
