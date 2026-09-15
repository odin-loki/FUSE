#pragma once

#include <fuse/types.hpp>

namespace fuse::world_partition {

/// Per-tick submission caps for streaming load/unload queues (B7.6 budget stub).
struct StreamingBudget {
    u32 max_loads_per_tick = 1;
    u32 max_unloads_per_tick = 1;
    u32 max_async_in_flight = 4;
};

[[nodiscard]] inline u32 effective_tick_budget(u32 queued, u32 per_tick_cap, bool unlimited) {
    if (unlimited) {
        return queued;
    }
    return queued < per_tick_cap ? queued : per_tick_cap;
}

} // namespace fuse::world_partition
