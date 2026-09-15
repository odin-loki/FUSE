#pragma once

#include <fuse/net/transport.hpp>
#include <fuse/types.hpp>

#include <cstdint>
#include <vector>

namespace fuse::net {

/// Snapshot of deterministic simulation state at a given frame.
struct GameSnapshot {
    u32 frame = 0;
    std::vector<byte> physics_state;
    std::vector<byte> ecs_state;
    u64 checksum = 0;
};

struct PlayerInput {
    u32 frame = 0;
    u32 player_id = 0;
    u32 buttons = 0;
    std::int16_t axis_lx = 0;
    std::int16_t axis_ly = 0;
    std::int16_t axis_rx = 0;
    std::int16_t axis_ry = 0;
};

} // namespace fuse::net
