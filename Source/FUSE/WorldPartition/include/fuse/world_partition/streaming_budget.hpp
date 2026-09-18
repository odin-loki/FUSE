#pragma once

#include <fuse/types.hpp>

#include <algorithm>
#include <limits>

namespace fuse::world_partition {

/// Per-tick submission caps and resident memory budget (B7.6 budget stub).
struct StreamingBudget {
    u32 max_loads_per_tick = 1;
    u32 max_unloads_per_tick = 1;
    u32 max_async_in_flight = 4;
    u64 max_resident_bytes = 0; ///< 0 = unlimited byte budget
};

/// Budget pressure counters surfaced to tooling and tests (B7.6 deepen).
struct StreamingBudgetCounters {
    u32 rejected_loads = 0;   ///< Loads rejected after eviction could not free budget
    u32 budget_evictions = 0; ///< Resident cells queued for unload to make room
    u32 eviction_skipped = 0; ///< Budget pressure with no evictable resident candidate
    u64 bytes_evicted = 0;    ///< Resident bytes released by budget-driven evictions
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

[[nodiscard]] inline u32 resident_cell_headroom(u32 max_loaded_cells, u32 resident_count) {
    return resident_count < max_loaded_cells ? max_loaded_cells - resident_count : 0u;
}

[[nodiscard]] inline u64 clamp_incoming_bytes(u64 bytes_remaining, u64 incoming_bytes) {
    if (bytes_remaining == ~0ull || incoming_bytes == 0u) {
        return incoming_bytes;
    }
    return incoming_bytes < bytes_remaining ? incoming_bytes : bytes_remaining;
}

[[nodiscard]] inline bool is_at_cell_cap(u32 max_loaded_cells, u32 resident_count) {
    return resident_count >= max_loaded_cells;
}

[[nodiscard]] inline bool is_at_byte_cap(u64 max_resident_bytes, u64 resident_bytes) {
    return !byte_budget_unlimited(max_resident_bytes) && resident_bytes >= max_resident_bytes;
}

[[nodiscard]] inline bool would_exceed_cell_cap(u32 max_loaded_cells, u32 resident_count,
                                                u32 incoming_count = 1u) {
    if (incoming_count == 0u) {
        return false;
    }
    return !can_accept_resident_cell(max_loaded_cells, resident_count + incoming_count - 1u);
}

[[nodiscard]] inline bool needs_budget_eviction(u32 max_loaded_cells, u32 resident_count, u64 max_resident_bytes,
                                                u64 resident_bytes, u64 incoming_bytes) {
    return !can_accept_resident_cell(max_loaded_cells, resident_count) ||
           would_exceed_byte_budget(max_resident_bytes, resident_bytes, incoming_bytes);
}

[[nodiscard]] inline bool needs_budget_eviction_for_incoming(u32 max_loaded_cells, u32 resident_count,
                                                             u64 max_resident_bytes, u64 resident_bytes,
                                                             u64 incoming_bytes, u32 incoming_count = 1u) {
    return would_exceed_cell_cap(max_loaded_cells, resident_count, incoming_count) ||
           would_exceed_byte_budget(max_resident_bytes, resident_bytes, incoming_bytes);
}

[[nodiscard]] inline u32 clamp_eviction_batch(u32 requested, u32 headroom) {
    return requested < headroom ? requested : headroom;
}

[[nodiscard]] inline u32 clamp_pending_submits(u32 pending, u32 max_pending) {
    if (max_pending == 0u) {
        return pending;
    }
    return pending < max_pending ? pending : max_pending;
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

/// Guard: eviction score with negative unload priority clamped to zero.
[[nodiscard]] inline f32 eviction_score_for_guarded(f32 unload_distance_priority, u32 last_touch_tick,
                                                    u32 current_tick, EvictionPolicy policy) {
    return eviction_score_for(std::max(0.f, unload_distance_priority), last_touch_tick, current_tick, policy);
}

/// Guard: true when a budget eviction score is eligible for eviction (positive).
[[nodiscard]] inline bool is_positive_eviction_score(f32 score) { return score > 0.f; }

/// Resident bytes that must be freed before `incoming_bytes` can fit under the byte cap (0 when unlimited or fits).
[[nodiscard]] inline u64 eviction_byte_deficit(u64 max_resident_bytes, u64 resident_bytes, u64 incoming_bytes) {
    if (byte_budget_unlimited(max_resident_bytes) || incoming_bytes == 0u) {
        return 0u;
    }
    const u64 projected = resident_bytes + incoming_bytes;
    return projected > max_resident_bytes ? projected - max_resident_bytes : 0u;
}

/// Resident cells that must be evicted before `incoming_count` can fit under the cell cap.
[[nodiscard]] inline u32 resident_cell_deficit(u32 max_loaded_cells, u32 resident_count, u32 incoming_count = 1u) {
    if (incoming_count == 0u || can_accept_resident_cell(max_loaded_cells, resident_count + incoming_count - 1u)) {
        return 0u;
    }
    return (resident_count + incoming_count) - max_loaded_cells;
}

/// Budget-pressure eviction score — prefers residency focus distance over stream-out unload priority.
[[nodiscard]] inline f32 budget_eviction_score(f32 focus_distance, f32 unload_distance_priority, u32 last_touch_tick,
                                               u32 current_tick, EvictionPolicy policy) {
    switch (policy) {
    case EvictionPolicy::DistanceFromFocus:
        if (focus_distance >= 0.f && focus_distance > unload_distance_priority) {
            return focus_distance;
        }
        return unload_distance_priority;
    case EvictionPolicy::Lru:
        return static_cast<f32>(current_tick - last_touch_tick);
    }
    return unload_distance_priority;
}

/// Guard: budget eviction score; returns -1 when focus distance is invalid for distance policy.
[[nodiscard]] inline f32 budget_eviction_score_guarded(f32 focus_distance, f32 unload_distance_priority,
                                                       u32 last_touch_tick, u32 current_tick,
                                                       EvictionPolicy policy) {
    if (policy == EvictionPolicy::DistanceFromFocus && focus_distance < 0.f) {
        return -1.f;
    }
    return budget_eviction_score(focus_distance, unload_distance_priority, last_touch_tick, current_tick,
                                 policy);
}

/// Guard: budget eviction score with negative unload priority clamped before scoring.
[[nodiscard]] inline f32 budget_eviction_score_for_guarded(f32 focus_distance, f32 unload_distance_priority,
                                                            u32 last_touch_tick, u32 current_tick,
                                                            EvictionPolicy policy) {
    return budget_eviction_score_guarded(focus_distance, std::max(0.f, unload_distance_priority),
                                         last_touch_tick, current_tick, policy);
}

/// True when an incoming load (higher `priority` = closer) should evict a resident at `resident_focus_distance`.
[[nodiscard]] inline bool incoming_outranks_resident(f32 incoming_priority, f32 stream_in_radius,
                                                     f32 resident_focus_distance) {
    if (incoming_priority <= 0.f || stream_in_radius <= 0.f) {
        return false;
    }
    const f32 incoming_distance = stream_in_radius - incoming_priority;
    return incoming_distance < resident_focus_distance;
}

/// True when an incoming load outranks a resident cell for budget eviction (closer wins).
[[nodiscard]] inline bool incoming_outranks_eviction(f32 incoming_priority, f32 eviction_score) {
    if (incoming_priority <= 0.f) {
        return false;
    }
    if (incoming_priority >= std::numeric_limits<f32>::max()) {
        return true;
    }
    return incoming_priority >= eviction_score;
}

/// Guard: incoming outrank check requires a positive eviction score.
[[nodiscard]] inline bool incoming_outranks_eviction_guarded(f32 incoming_priority, f32 eviction_score) {
    return is_positive_eviction_score(eviction_score) &&
           incoming_outranks_eviction(incoming_priority, eviction_score);
}

/// True when a budget eviction candidate is eligible under distance policy pressure checks.
[[nodiscard]] inline bool can_evict_for_incoming(f32 incoming_priority, f32 eviction_score,
                                                EvictionPolicy policy) {
    if (!is_positive_eviction_score(eviction_score)) {
        return false;
    }
    if (policy != EvictionPolicy::DistanceFromFocus) {
        return true;
    }
    return incoming_outranks_eviction(incoming_priority, eviction_score);
}

/// Guard: rejects non-positive eviction scores before policy checks.
[[nodiscard]] inline bool can_evict_for_incoming_guarded(f32 incoming_priority, f32 eviction_score,
                                                         EvictionPolicy policy) {
    return is_positive_eviction_score(eviction_score) &&
           can_evict_for_incoming(incoming_priority, eviction_score, policy);
}

/// True when budget pressure exists and the residency set can supply an eviction candidate.
[[nodiscard]] inline bool can_attempt_budget_eviction(u32 max_loaded_cells, u32 resident_count,
                                                        u64 max_resident_bytes, u64 resident_bytes,
                                                        u64 incoming_bytes, bool has_eviction_candidate) {
    return needs_budget_eviction_for_incoming(max_loaded_cells, resident_count, max_resident_bytes,
                                              resident_bytes, incoming_bytes) &&
           has_eviction_candidate;
}

/// True when incoming load would exceed cell or byte caps (deficit helpers).
[[nodiscard]] inline bool needs_eviction_for_deficit(u32 max_loaded_cells, u32 resident_count,
                                                     u64 max_resident_bytes, u64 resident_bytes,
                                                     u64 incoming_bytes, u32 incoming_count = 1u) {
    return resident_cell_deficit(max_loaded_cells, resident_count, incoming_count) > 0u ||
           eviction_byte_deficit(max_resident_bytes, resident_bytes, incoming_bytes) > 0u;
}

} // namespace fuse::world_partition
