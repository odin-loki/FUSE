#pragma once

#include <fuse/types.hpp>

#include <algorithm>
#include <limits>

namespace fuse::terrain {

/// Per-tick resident chunk caps mirrored from `TerrainDesc` (B7.5 budget stub).
struct LodResidencyBudget {
    u32 max_resident_chunks = 0; ///< 0 = unlimited resident chunks
    u32 max_async_in_flight = 4;
    u32 max_loads_per_tick = 1;
};

/// Eviction ordering when resident caps are exceeded (B7.5 deepen — mirrors B7.6).
enum class LodEvictionPolicy : u8 {
    DistanceFromFocus, ///< Farther chunks beyond load radius evict first
    Lru,               ///< Least-recently touched resident chunks evict first
};

/// Budget pressure counters tracked by `ChunkGrid` (mirrors B7.6 `StreamingBudgetCounters`).
struct LodResidencyBudgetCounters {
    u32 rejected_loads = 0;   ///< Loads rejected after eviction could not free a slot
    u32 budget_evictions = 0; ///< Resident chunks queued for unload to make room
    u32 eviction_skipped = 0; ///< Cap pressure with no evictable resident candidate
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

[[nodiscard]] inline bool needs_budget_eviction(u32 max_resident_chunks, u32 resident_count) {
    return is_at_resident_cap(max_resident_chunks, resident_count);
}

[[nodiscard]] inline bool would_exceed_resident_cap(u32 max_resident_chunks, u32 resident_count,
                                                    u32 incoming_count = 1u) {
    if (resident_cap_unlimited(max_resident_chunks) || incoming_count == 0u) {
        return false;
    }
    return resident_count + incoming_count > max_resident_chunks;
}

[[nodiscard]] inline bool needs_budget_eviction_for_incoming(u32 max_resident_chunks, u32 resident_count,
                                                             u32 incoming_count = 1u) {
    return would_exceed_resident_cap(max_resident_chunks, resident_count, incoming_count);
}

[[nodiscard]] inline u32 effective_tick_budget(u32 queued, u32 per_tick_cap) {
    if (per_tick_cap == 0u) {
        return queued;
    }
    return queued < per_tick_cap ? queued : per_tick_cap;
}

[[nodiscard]] inline u32 clamp_loads_per_tick(u32 queued, const LodResidencyBudget& budget) {
    return effective_tick_budget(queued, budget.max_loads_per_tick);
}

[[nodiscard]] inline u32 clamp_pending_submits(u32 pending, u32 max_pending) {
    if (max_pending == 0u) {
        return pending;
    }
    return pending < max_pending ? pending : max_pending;
}

[[nodiscard]] inline bool can_submit_async_load(u32 in_flight, u32 max_async_in_flight) {
    return max_async_in_flight == 0u || in_flight < max_async_in_flight;
}

[[nodiscard]] inline bool is_at_async_in_flight_cap(u32 in_flight, u32 max_async_in_flight) {
    return !can_submit_async_load(in_flight, max_async_in_flight);
}

[[nodiscard]] inline u32 async_in_flight_headroom(u32 max_async_in_flight, u32 in_flight) {
    if (max_async_in_flight == 0u) {
        return ~0u;
    }
    return in_flight < max_async_in_flight ? max_async_in_flight - in_flight : 0u;
}

[[nodiscard]] inline bool pending_submit_cap_unlimited(u32 max_pending_submits) {
    return max_pending_submits == 0u;
}

[[nodiscard]] inline u32 pending_submit_headroom(u32 max_pending_submits, u32 pending_submits) {
    if (pending_submit_cap_unlimited(max_pending_submits)) {
        return ~0u;
    }
    return pending_submits < max_pending_submits ? max_pending_submits - pending_submits : 0u;
}

[[nodiscard]] inline bool would_exceed_pending_submit_cap(u32 max_pending_submits, u32 pending_submits) {
    if (pending_submit_cap_unlimited(max_pending_submits)) {
        return false;
    }
    return pending_submits >= max_pending_submits;
}

/// Guard async submit when in-flight or buffered completions would exceed the pending cap.
[[nodiscard]] inline bool can_submit_async_load_guarded(u32 in_flight, u32 pending_submits,
                                                          u32 max_async_in_flight) {
    return can_submit_async_load(in_flight, max_async_in_flight) &&
           !would_exceed_pending_submit_cap(max_async_in_flight, pending_submits);
}

/// Combine streaming-volume unload priority with any stored chunk priority (B7.5 deepen).
[[nodiscard]] inline f32 effective_chunk_unload_priority(f32 stream_priority, f32 stored_priority) {
    return std::max(stream_priority, stored_priority);
}

/// Rank unload pressure for eviction queue ordering (B7.5 deepen — mirrors B7.6).
[[nodiscard]] inline f32 rank_chunk_unload_priority(f32 stream_priority, f32 stored_priority,
                                                      f32 focus_distance) {
    return std::max({stream_priority, stored_priority, focus_distance});
}

/// Merge unload rank with a budget eviction score for queue ordering (B7.5 deepen).
[[nodiscard]] inline f32 rank_budget_chunk_unload_priority(f32 stream_priority, f32 stored_priority,
                                                             f32 focus_distance, f32 budget_score) {
    return std::max(rank_chunk_unload_priority(stream_priority, stored_priority, focus_distance), budget_score);
}

[[nodiscard]] inline u32 clamp_eviction_batch(u32 requested, u32 headroom) {
    return requested < headroom ? requested : headroom;
}

/// Higher score evicts sooner. Distance policy uses focus distance; LRU uses age.
[[nodiscard]] inline f32 eviction_score_for(f32 focus_distance, u32 last_touch_tick, u32 current_tick,
                                            LodEvictionPolicy policy) {
    switch (policy) {
    case LodEvictionPolicy::DistanceFromFocus:
        return focus_distance;
    case LodEvictionPolicy::Lru:
        return static_cast<f32>(current_tick - last_touch_tick);
    }
    return focus_distance;
}

/// Budget-pressure eviction score — prefers residency focus distance over unload priority.
[[nodiscard]] inline f32 budget_eviction_score(f32 focus_distance, f32 unload_priority, u32 last_touch_tick,
                                               u32 current_tick, LodEvictionPolicy policy) {
    switch (policy) {
    case LodEvictionPolicy::DistanceFromFocus:
        if (focus_distance >= 0.f && focus_distance > unload_priority) {
            return focus_distance;
        }
        return unload_priority;
    case LodEvictionPolicy::Lru:
        return static_cast<f32>(current_tick - last_touch_tick);
    }
    return unload_priority;
}

/// Guard: budget eviction score; returns -1 when focus distance is invalid for distance policy.
[[nodiscard]] inline f32 budget_eviction_score_guarded(f32 focus_distance, f32 unload_priority, u32 last_touch_tick,
                                                       u32 current_tick, LodEvictionPolicy policy) {
    if (policy == LodEvictionPolicy::DistanceFromFocus && focus_distance < 0.f) {
        return -1.f;
    }
    return budget_eviction_score(focus_distance, unload_priority, last_touch_tick, current_tick, policy);
}

/// True when an incoming load (higher `priority` = closer) should evict a resident at `resident_focus_distance`.
[[nodiscard]] inline bool incoming_outranks_resident(f32 incoming_priority, f32 load_radius,
                                                     f32 resident_focus_distance) {
    if (incoming_priority <= 0.f || load_radius <= 0.f) {
        return false;
    }
    const f32 incoming_distance = load_radius - incoming_priority;
    return incoming_distance < resident_focus_distance;
}

/// True when an incoming load outranks a resident eviction score (closer / higher priority wins).
[[nodiscard]] inline bool incoming_outranks_eviction(f32 incoming_priority, f32 eviction_score) {
    if (incoming_priority <= 0.f) {
        return false;
    }
    if (incoming_priority >= std::numeric_limits<f32>::max()) {
        return true;
    }
    return incoming_priority >= eviction_score;
}

/// True when a budget eviction candidate is eligible under distance policy pressure checks.
[[nodiscard]] inline bool can_evict_for_incoming(f32 incoming_priority, f32 resident_focus_distance,
                                                   f32 load_radius, LodEvictionPolicy policy) {
    if (resident_focus_distance <= 0.f) {
        return false;
    }
    if (policy != LodEvictionPolicy::DistanceFromFocus) {
        return true;
    }
    return incoming_outranks_resident(incoming_priority, load_radius, resident_focus_distance);
}

/// True when resident cap pressure exists and the residency set can supply an eviction candidate.
[[nodiscard]] inline bool can_attempt_budget_eviction(u32 max_resident_chunks, u32 resident_count,
                                                        bool has_eviction_candidate,
                                                        u32 incoming_count = 1u) {
    return needs_budget_eviction_for_incoming(max_resident_chunks, resident_count, incoming_count) &&
           has_eviction_candidate;
}

/// True when an eviction score is positive (eligible for budget eviction ordering).
[[nodiscard]] inline bool is_positive_eviction_score(f32 score) { return score > 0.f; }

} // namespace fuse::terrain
