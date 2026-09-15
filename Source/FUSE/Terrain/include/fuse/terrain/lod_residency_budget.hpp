#pragma once

#include <fuse/types.hpp>

#include <algorithm>

namespace fuse::terrain {

/// Per-tick resident chunk caps mirrored from `TerrainDesc` (B7.5 budget stub).
struct LodResidencyBudget {
    u32 max_resident_chunks = 0; ///< 0 = unlimited resident chunks
    u32 max_async_in_flight = 4;
    u32 max_loads_per_tick = 1;
};

/// Budget pressure counters tracked by `ChunkGrid` (mirrors B7.6 `StreamingBudgetCounters`).
struct LodResidencyBudgetCounters {
    u32 rejected_loads = 0;   ///< Loads rejected after eviction could not free a slot
    u32 budget_evictions = 0; ///< Resident chunks queued for unload to make room
};

/// Clamp a discrete LOD index to the valid range for a terrain description.
[[nodiscard]] inline u32 clamp_lod_level(u32 lod, u32 max_lod_levels) {
    if (max_lod_levels == 0u) {
        return 0u;
    }
    return std::min(lod, max_lod_levels - 1u);
}

[[nodiscard]] inline bool resident_cap_unlimited(u32 max_resident_chunks) { return max_resident_chunks == 0u; }

[[nodiscard]] inline u32 resident_chunk_headroom(u32 max_resident_chunks, u32 resident_count) {
    if (resident_cap_unlimited(max_resident_chunks)) {
        return ~0u;
    }
    return resident_count < max_resident_chunks ? max_resident_chunks - resident_count : 0u;
}

[[nodiscard]] inline bool can_accept_resident_chunk(u32 max_resident_chunks, u32 resident_count) {
    return resident_cap_unlimited(max_resident_chunks) || resident_count < max_resident_chunks;
}

[[nodiscard]] inline bool is_at_resident_cap(u32 max_resident_chunks, u32 resident_count) {
    return !resident_cap_unlimited(max_resident_chunks) && resident_count >= max_resident_chunks;
}

[[nodiscard]] inline u32 effective_tick_budget(u32 queued, u32 per_tick_cap) {
    if (per_tick_cap == 0u) {
        return queued;
    }
    return queued < per_tick_cap ? queued : per_tick_cap;
}

[[nodiscard]] inline u32 clamp_pending_submits(u32 pending, u32 max_pending) {
    if (max_pending == 0u) {
        return pending;
    }
    return pending < max_pending ? pending : max_pending;
}

/// True when an incoming load (higher `priority` = closer) should evict a resident at `resident_focus_distance`.
[[nodiscard]] inline bool incoming_outranks_resident(f32 incoming_priority, f32 load_radius, f32 resident_focus_distance) {
    if (incoming_priority <= 0.f || load_radius <= 0.f) {
        return false;
    }
    const f32 incoming_distance = load_radius - incoming_priority;
    return incoming_distance < resident_focus_distance;
}

} // namespace fuse::terrain
