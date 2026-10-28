#pragma once

#include <cstdint>

namespace fuse::ai {

/// True when both ids name the same team. Team 0 matches team 0.
[[nodiscard]] inline bool same_team(std::uint32_t a, std::uint32_t b) {
    return a == b;
}

} // namespace fuse::ai
