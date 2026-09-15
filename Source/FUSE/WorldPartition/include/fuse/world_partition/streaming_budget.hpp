#pragma once

#include <fuse/types.hpp>

#include <algorithm>

namespace fuse::world_partition {

/// Per-tick submission caps and resident memory budget (B7.6 budget stub).
struct StreamingBudget {
    u32 max_loads_per_tick = 1;
    u32 max_unloads_per_tick = 1;
    u32 max_async_in_flight = 4;
    u64 max_resident_bytes = 0; ///< 0 = unlimited byte budget
};

/// Eviction ordering when resident caps are exceeded (B7.6 stub).
enum class EvictionPolicy : u8 {
    DistanceFromFocus, ///< Farther cells beyond stream-out radius evict first
    Lru,               ///< Least-recently touched resident cells evict first
};

[[nodiscard]] inline u32 effective_tick_budget(u32 queued, u32 per_tick_cap, bool unlimited) {
    if (unlimited) {
        return queued;
    }
    return queued < per_tick_cap ? queued : per_tick_cap;
}

[[nodiscard]] inline bool byte_budget_unlimited(u64 max_resident_bytes) {
    return max_resident_bytes == 0u;
}

[[nodiscard]] inline u64 bytes_remaining(u64 max_resident_bytes, u64 resident_bytes) {
    if (byte_budget_unlimited(max_resident_bytes)) {
        return ~0ull;
    }
    return resident_bytes < max_resident_bytes ? max_resident_bytes - resident_bytes : 0u;
}

[[nodiscard]] inline bool would_exceed_byte_budget(u64 max_resident_bytes, u64 resident_bytes, u64 incoming) {
    if (byte_budget_unlimited(max_resident_bytes) || incoming == 0u) {
        return false;
    }
    return resident_bytes + incoming > max_resident_bytes;
}

[[nodiscard]] inline bool can_accept_resident_cell(u32 max_loaded_cells, u32 resident_count) {
    return resident_count < max_loaded_cells;
}

/// Higher score evicts sooner. Distance policy uses unload distance priority; LRU uses age.
[[nodiscard]] inline f32 eviction_score_for(f32 unload_distance_priority, u32 last_touch_tick, u32 current_tick,
                                          EvictionPolicy policy) {
    switch (policy) {
    case EvictionPolicy::DistanceFromFocus:
        return unload_distance_priority;
    case EvictionPolicy::Lru:
        return static_cast<f32>(current_tick - last_touch_tick);
    }
    return unload_distance_priority;
}

} // namespace fuse::world_partition
