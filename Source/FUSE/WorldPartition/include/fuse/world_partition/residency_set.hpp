#pragma once

#include <fuse/types.hpp>
#include <fuse/world_partition/grid_cell.hpp>
#include <fuse/world_partition/streaming_budget.hpp>

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace fuse::world_partition {

/// One resident cell tracked by focus distance from the streaming center.
struct ResidencyEntry {
    GridCoord coord{};
    f32 focus_distance = 0.f; ///< Planar distance from focus; farther cells evict first
};

/// Focus-distance-ordered resident cell set (B7.6 stub).
class ResidencySet {
public:
    /// Insert or refresh a resident cell. Returns false when `focus_distance` is negative.
    bool add(GridCoord coord, f32 focus_distance);

    /// Remove a resident cell. Returns false when the coord was not resident.
    bool remove(GridCoord coord);

    /// Update focus distance for an existing entry.
    bool update_focus_distance(GridCoord coord, f32 focus_distance);

    [[nodiscard]] bool contains(GridCoord coord) const;
    [[nodiscard]] f32 focus_distance_for(GridCoord coord) const;
    [[nodiscard]] u32 size() const { return static_cast<u32>(m_entries.size()); }
    [[nodiscard]] bool empty() const { return m_entries.empty(); }
    [[nodiscard]] bool has_eviction_candidate() const { return !m_entries.empty(); }

    /// Coord with the largest focus distance (evict first). Returns {0,0} when empty.
    [[nodiscard]] GridCoord pick_eviction_candidate() const;

    /// Return up to `max_count` coords sorted farthest-first (eviction order).
    [[nodiscard]] std::vector<GridCoord> collect_eviction_candidates(u32 max_count = 0) const;

    void clear();

private:
    [[nodiscard]] u32 find_index_(GridCoord coord) const;

    std::unordered_map<u64, u32> m_index;
    std::vector<ResidencyEntry> m_entries;
};

inline bool ResidencySet::add(GridCoord coord, f32 focus_distance) {
    if (!is_valid_grid_coord(coord) || focus_distance < 0.f) {
        return false;
    }

    const u64 key = grid_coord_key(coord);
    const auto it = m_index.find(key);
    if (it != m_index.end()) {
        m_entries[it->second].focus_distance = focus_distance;
        return true;
    }

    const u32 index = static_cast<u32>(m_entries.size());
    m_index.emplace(key, index);
    m_entries.push_back({coord, focus_distance});
    return true;
}

inline bool ResidencySet::remove(GridCoord coord) {
    if (!is_valid_grid_coord(coord)) {
        return false;
    }

    const u64 key = grid_coord_key(coord);
    const auto it = m_index.find(key);
    if (it == m_index.end()) {
        return false;
    }

    const u32 remove_index = it->second;
    m_index.erase(it);

    if (remove_index + 1u < m_entries.size()) {
        const ResidencyEntry moved = m_entries.back();
        m_entries[remove_index] = moved;
        m_index[grid_coord_key(moved.coord)] = remove_index;
    }
    m_entries.pop_back();
    return true;
}

inline bool ResidencySet::update_focus_distance(GridCoord coord, f32 focus_distance) {
    if (!is_valid_grid_coord(coord) || focus_distance < 0.f) {
        return false;
    }

    const u32 index = find_index_(coord);
    if (index == static_cast<u32>(-1)) {
        return false;
    }

    m_entries[index].focus_distance = focus_distance;
    return true;
}

inline bool ResidencySet::contains(GridCoord coord) const { return find_index_(coord) != static_cast<u32>(-1); }

inline f32 ResidencySet::focus_distance_for(GridCoord coord) const {
    const u32 index = find_index_(coord);
    return index == static_cast<u32>(-1) ? -1.f : m_entries[index].focus_distance;
}

inline GridCoord ResidencySet::pick_eviction_candidate() const {
    const std::vector<GridCoord> candidates = collect_eviction_candidates(1);
    return candidates.empty() ? kInvalidGridCoord : candidates.front();
}

inline std::vector<GridCoord> ResidencySet::collect_eviction_candidates(u32 max_count) const {
    if (m_entries.empty()) {
        return {};
    }

    std::vector<ResidencyEntry> sorted = m_entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const ResidencyEntry& a, const ResidencyEntry& b) {
                  if (a.focus_distance != b.focus_distance) {
                      return a.focus_distance > b.focus_distance;
                  }
                  return grid_coord_key(a.coord) > grid_coord_key(b.coord);
              });

    const u32 limit = max_count == 0u ? static_cast<u32>(sorted.size())
                                     : std::min(max_count, static_cast<u32>(sorted.size()));
    std::vector<GridCoord> out;
    out.reserve(limit);
    for (u32 i = 0; i < limit; ++i) {
        out.push_back(sorted[i].coord);
    }
    return out;
}

inline void ResidencySet::clear() {
    m_index.clear();
    m_entries.clear();
}

inline u32 ResidencySet::find_index_(GridCoord coord) const {
    const auto it = m_index.find(grid_coord_key(coord));
    return it != m_index.end() ? it->second : static_cast<u32>(-1);
}

/// Stub: register a resident cell; rejects invalid coords and focus distance.
[[nodiscard]] inline bool try_add_resident(ResidencySet& set, GridCoord coord, f32 focus_distance) {
    if (!is_valid_grid_coord(coord)) {
        return false;
    }
    return set.add(coord, focus_distance);
}

/// Stub: evict a cell from the resident set; rejects invalid coords.
[[nodiscard]] inline bool try_remove_resident(ResidencySet& set, GridCoord coord) {
    if (!is_valid_grid_coord(coord)) {
        return false;
    }
    return set.remove(coord);
}

/// Stub: register residency after a successful async load completes on the game thread.
[[nodiscard]] inline bool apply_residency_on_load_complete(ResidencySet& set, GridCoord coord,
                                                            f32 focus_distance, bool success) {
    if (!is_valid_grid_coord(coord)) {
        return false;
    }
    return success ? try_add_resident(set, coord, focus_distance) : false;
}

/// Stub: clear residency after a successful async unload completes on the game thread.
[[nodiscard]] inline bool apply_residency_on_unload_complete(ResidencySet& set, GridCoord coord,
                                                              bool success) {
    if (!is_valid_grid_coord(coord)) {
        return false;
    }
    return success ? try_remove_resident(set, coord) : false;
}

/// Empty-set guard: returns `kInvalidGridCoord` when no eviction candidate exists.
[[nodiscard]] inline GridCoord pick_eviction_candidate_guarded(const ResidencySet& set) {
    return set.has_eviction_candidate() ? set.pick_eviction_candidate() : kInvalidGridCoord;
}

/// Guard: returns false when coord is invalid or not resident.
[[nodiscard]] inline bool contains_resident_guarded(const ResidencySet& set, GridCoord coord) {
    return is_valid_grid_coord(coord) && set.contains(coord);
}

/// Guard: clears residency set; returns false when already empty.
[[nodiscard]] inline bool clear_residency_guarded(ResidencySet& set) {
    if (set.empty()) {
        return false;
    }
    set.clear();
    return true;
}

/// Empty-set guard: returns false when the residency set has no eviction candidate.
[[nodiscard]] inline bool has_residency_eviction_candidate_guarded(const ResidencySet& set) {
    return set.has_eviction_candidate();
}

/// Guard: returns -1 when coord is invalid or not resident.
[[nodiscard]] inline f32 focus_distance_for_guarded(const ResidencySet& set, GridCoord coord) {
    if (!is_valid_grid_coord(coord) || !set.contains(coord)) {
        return -1.f;
    }
    return set.focus_distance_for(coord);
}

/// Pick the farthest coord from `candidates` eligible for budget eviction under `policy`.
/// Returns `kInvalidGridCoord` and leaves `out_score` at -1 when no candidate qualifies.
template <typename ScoreFn>
[[nodiscard]] inline GridCoord pick_budget_eviction_candidate(const std::vector<GridCoord>& candidates,
                                                              ScoreFn&& score_fn, f32 incoming_priority,
                                                              EvictionPolicy policy, f32& out_score) {
    out_score = -1.f;
    for (const GridCoord coord : candidates) {
        if (!is_valid_grid_coord(coord)) {
            continue;
        }
        const f32 score = score_fn(coord);
        if (!can_evict_for_incoming(incoming_priority, score, policy)) {
            continue;
        }
        out_score = score;
        return coord;
    }
    return kInvalidGridCoord;
}

/// Return coords from `candidates` eligible for budget eviction, preserving farthest-first order.
template <typename ScoreFn>
[[nodiscard]] inline std::vector<GridCoord> collect_budget_eviction_candidates(
    const std::vector<GridCoord>& candidates, ScoreFn&& score_fn, f32 incoming_priority,
    EvictionPolicy policy) {
    std::vector<GridCoord> eligible;
    eligible.reserve(candidates.size());
    for (const GridCoord coord : candidates) {
        if (!is_valid_grid_coord(coord)) {
            continue;
        }
        const f32 score = score_fn(coord);
        if (can_evict_for_incoming(incoming_priority, score, policy)) {
            eligible.push_back(coord);
        }
    }
    return eligible;
}

/// Empty-set guard: pick budget eviction candidate from a residency set.
template <typename ScoreFn>
[[nodiscard]] inline GridCoord pick_budget_eviction_candidate_from_set(const ResidencySet& set,
                                                                        ScoreFn&& score_fn,
                                                                        f32 incoming_priority,
                                                                        EvictionPolicy policy,
                                                                        f32& out_score) {
    if (!set.has_eviction_candidate()) {
        out_score = -1.f;
        return kInvalidGridCoord;
    }
    return pick_budget_eviction_candidate(set.collect_eviction_candidates(), score_fn, incoming_priority,
                                          policy, out_score);
}

/// True when the residency set has at least one eligible budget eviction candidate.
template <typename ScoreFn>
[[nodiscard]] inline bool has_budget_eviction_candidate(const ResidencySet& set, ScoreFn&& score_fn,
                                                         f32 incoming_priority, EvictionPolicy policy) {
    f32 score = -1.f;
    const GridCoord picked =
        pick_budget_eviction_candidate_from_set(set, score_fn, incoming_priority, policy, score);
    return is_valid_grid_coord(picked) && is_budget_eviction_score_eligible(score);
}

/// True when budget pressure exists and the residency set can supply an eviction candidate.
[[nodiscard]] inline bool can_attempt_budget_eviction_from_set(u32 max_loaded_cells, u32 resident_count,
                                                                u64 max_resident_bytes, u64 resident_bytes,
                                                                u64 incoming_bytes, const ResidencySet& set) {
    return can_attempt_budget_eviction(max_loaded_cells, resident_count, max_resident_bytes, resident_bytes,
                                       incoming_bytes, set.has_eviction_candidate());
}

/// Combined unload rank for budget-driven eviction (B7.6 deepen).
[[nodiscard]] inline f32 eviction_unload_priority(f32 streaming_priority, f32 stored_priority,
                                                   f32 focus_distance, f32 unload_distance_priority,
                                                   u32 last_touch_tick, u32 current_tick,
                                                   EvictionPolicy policy) {
    const f32 budget_score = budget_eviction_score(focus_distance, unload_distance_priority,
                                                    last_touch_tick, current_tick, policy);
    return rank_budget_unload_priority(streaming_priority, stored_priority, focus_distance, budget_score);
}

} // namespace fuse::world_partition
