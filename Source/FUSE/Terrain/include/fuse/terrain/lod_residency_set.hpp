#pragma once

#include <fuse/terrain/lod_residency_budget.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace fuse::terrain {

/// Sentinel returned by `pick_eviction_candidate` when the set is empty.
inline constexpr u32 kInvalidChunkIndex = ~0u;

/// One resident terrain chunk tracked by planar focus distance from the camera.
struct LodResidencyEntry {
    u32 chunk_index = kInvalidChunkIndex;
    f32 focus_distance = 0.f; ///< Planar distance from focus; farther chunks evict first
};

/// Focus-distance-ordered resident chunk set (B7.5 stub — mirrors B7.6 `ResidencySet`).
class LodResidencySet {
public:
    /// Insert or refresh a resident chunk. Returns false when `focus_distance` is negative.
    bool add(u32 chunk_index, f32 focus_distance);

    /// Remove a resident chunk. Returns false when the index was not resident.
    bool remove(u32 chunk_index);

    /// Update focus distance for an existing entry.
    bool update_focus_distance(u32 chunk_index, f32 focus_distance);

    [[nodiscard]] bool contains(u32 chunk_index) const;
    [[nodiscard]] f32 focus_distance_for(u32 chunk_index) const;
    [[nodiscard]] u32 size() const { return static_cast<u32>(m_entries.size()); }
    [[nodiscard]] bool empty() const { return m_entries.empty(); }
    [[nodiscard]] bool has_eviction_candidate() const { return !m_entries.empty(); }

    /// Chunk with the largest focus distance (evict first). Returns `kInvalidChunkIndex` when empty.
    [[nodiscard]] u32 pick_eviction_candidate() const;

    /// Return up to `max_count` chunk indices sorted farthest-first (eviction order).
    [[nodiscard]] std::vector<u32> collect_eviction_candidates(u32 max_count = 0) const;

    void clear();

private:
    [[nodiscard]] u32 find_index_(u32 chunk_index) const;

    std::unordered_map<u32, u32> m_index;
    std::vector<LodResidencyEntry> m_entries;
};

inline bool LodResidencySet::add(u32 chunk_index, f32 focus_distance) {
    if (focus_distance < 0.f) {
        return false;
    }

    const auto it = m_index.find(chunk_index);
    if (it != m_index.end()) {
        m_entries[it->second].focus_distance = focus_distance;
        return true;
    }

    const u32 index = static_cast<u32>(m_entries.size());
    m_index.emplace(chunk_index, index);
    m_entries.push_back({chunk_index, focus_distance});
    return true;
}

inline bool LodResidencySet::remove(u32 chunk_index) {
    const auto it = m_index.find(chunk_index);
    if (it == m_index.end()) {
        return false;
    }

    const u32 remove_index = it->second;
    m_index.erase(it);

    if (remove_index + 1u < m_entries.size()) {
        const LodResidencyEntry moved = m_entries.back();
        m_entries[remove_index] = moved;
        m_index[moved.chunk_index] = remove_index;
    }
    m_entries.pop_back();
    return true;
}

inline bool LodResidencySet::update_focus_distance(u32 chunk_index, f32 focus_distance) {
    if (focus_distance < 0.f) {
        return false;
    }

    const u32 index = find_index_(chunk_index);
    if (index == kInvalidChunkIndex) {
        return false;
    }

    m_entries[index].focus_distance = focus_distance;
    return true;
}

inline bool LodResidencySet::contains(u32 chunk_index) const {
    return find_index_(chunk_index) != kInvalidChunkIndex;
}

inline f32 LodResidencySet::focus_distance_for(u32 chunk_index) const {
    const u32 index = find_index_(chunk_index);
    return index == kInvalidChunkIndex ? -1.f : m_entries[index].focus_distance;
}

inline u32 LodResidencySet::pick_eviction_candidate() const {
    const std::vector<u32> candidates = collect_eviction_candidates(1);
    return candidates.empty() ? kInvalidChunkIndex : candidates.front();
}

inline std::vector<u32> LodResidencySet::collect_eviction_candidates(u32 max_count) const {
    if (m_entries.empty()) {
        return {};
    }

    std::vector<LodResidencyEntry> sorted = m_entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const LodResidencyEntry& a, const LodResidencyEntry& b) {
                  if (a.focus_distance != b.focus_distance) {
                      return a.focus_distance > b.focus_distance;
                  }
                  return a.chunk_index > b.chunk_index;
              });

    const u32 limit = max_count == 0u ? static_cast<u32>(sorted.size())
                                     : std::min(max_count, static_cast<u32>(sorted.size()));
    std::vector<u32> out;
    out.reserve(limit);
    for (u32 i = 0; i < limit; ++i) {
        out.push_back(sorted[i].chunk_index);
    }
    return out;
}

inline void LodResidencySet::clear() {
    m_index.clear();
    m_entries.clear();
}

inline u32 LodResidencySet::find_index_(u32 chunk_index) const {
    const auto it = m_index.find(chunk_index);
    return it != m_index.end() ? it->second : kInvalidChunkIndex;
}

/// Stub: register a resident chunk; rejects invalid focus distance.
[[nodiscard]] inline bool try_add_resident(LodResidencySet& set, u32 chunk_index, f32 focus_distance) {
    return set.add(chunk_index, focus_distance);
}

/// Stub: evict a chunk from the resident set.
[[nodiscard]] inline bool try_remove_resident(LodResidencySet& set, u32 chunk_index) {
    return set.remove(chunk_index);
}

/// Stub: register residency after a successful async load completes on the game thread.
[[nodiscard]] inline bool apply_residency_on_load_complete(LodResidencySet& set, u32 chunk_index,
                                                              f32 focus_distance, bool success) {
    return success ? try_add_resident(set, chunk_index, focus_distance) : false;
}

/// Stub: clear residency after a successful async unload completes on the game thread.
[[nodiscard]] inline bool apply_residency_on_unload_complete(LodResidencySet& set, u32 chunk_index,
                                                              bool success) {
    return success ? try_remove_resident(set, chunk_index) : false;
}

/// Empty-set guard: returns `kInvalidChunkIndex` when no eviction candidate exists.
[[nodiscard]] inline u32 pick_eviction_candidate_guarded(const LodResidencySet& set) {
    return set.has_eviction_candidate() ? set.pick_eviction_candidate() : kInvalidChunkIndex;
}

/// Pick the farthest chunk from `candidates` eligible for budget eviction under `policy`.
/// Returns `kInvalidChunkIndex` and leaves `out_score` at -1 when no candidate qualifies.
template <typename ScoreFn>
[[nodiscard]] inline u32 pick_budget_eviction_candidate(const std::vector<u32>& candidates, ScoreFn&& score_fn,
                                                        f32 incoming_priority, f32 load_radius,
                                                        LodEvictionPolicy policy, f32& out_score) {
    out_score = -1.f;
    for (const u32 chunk_index : candidates) {
        if (chunk_index == kInvalidChunkIndex) {
            continue;
        }
        const f32 score = score_fn(chunk_index);
        if (!can_evict_for_incoming(incoming_priority, score, load_radius, policy)) {
            continue;
        }
        out_score = score;
        return chunk_index;
    }
    return kInvalidChunkIndex;
}

/// Return chunk indices from `candidates` eligible for budget eviction, preserving farthest-first order.
template <typename ScoreFn>
[[nodiscard]] inline std::vector<u32> collect_budget_eviction_candidates(const std::vector<u32>& candidates,
                                                                         ScoreFn&& score_fn,
                                                                         f32 incoming_priority, f32 load_radius,
                                                                         LodEvictionPolicy policy) {
    std::vector<u32> eligible;
    eligible.reserve(candidates.size());
    for (const u32 chunk_index : candidates) {
        if (chunk_index == kInvalidChunkIndex) {
            continue;
        }
        const f32 score = score_fn(chunk_index);
        if (can_evict_for_incoming(incoming_priority, score, load_radius, policy)) {
            eligible.push_back(chunk_index);
        }
    }
    return eligible;
}

/// Empty-set guard: pick budget eviction candidate from a residency set.
template <typename ScoreFn>
[[nodiscard]] inline u32 pick_budget_eviction_candidate_from_set(const LodResidencySet& set, ScoreFn&& score_fn,
                                                                 f32 incoming_priority, f32 load_radius,
                                                                 LodEvictionPolicy policy, f32& out_score) {
    if (!set.has_eviction_candidate()) {
        out_score = -1.f;
        return kInvalidChunkIndex;
    }
    return pick_budget_eviction_candidate(set.collect_eviction_candidates(), score_fn, incoming_priority,
                                          load_radius, policy, out_score);
}

/// True when the residency set has at least one eligible budget eviction candidate.
template <typename ScoreFn>
[[nodiscard]] inline bool has_budget_eviction_candidate(const LodResidencySet& set, ScoreFn&& score_fn,
                                                         f32 incoming_priority, f32 load_radius,
                                                         LodEvictionPolicy policy) {
    f32 score = -1.f;
    const u32 picked =
        pick_budget_eviction_candidate_from_set(set, score_fn, incoming_priority, load_radius, policy, score);
    return picked != kInvalidChunkIndex && score > 0.f;
}

/// Guard: returns false when chunk index is invalid or not resident.
[[nodiscard]] inline bool contains_resident_guarded(const LodResidencySet& set, u32 chunk_index) {
    return chunk_index != kInvalidChunkIndex && set.contains(chunk_index);
}

/// Guard: clears residency set; returns false when already empty.
[[nodiscard]] inline bool clear_residency_guarded(LodResidencySet& set) {
    if (set.empty()) {
        return false;
    }
    set.clear();
    return true;
}

/// Empty-set guard: true when the residency set tracks at least one resident cell.
[[nodiscard]] inline bool has_residency_guarded(const LodResidencySet& set) {
    return !set.empty();
}

/// Guard: removes a resident chunk; returns false when index is invalid or not resident.
[[nodiscard]] inline bool remove_resident_guarded(LodResidencySet& set, u32 chunk_index) {
    return try_remove_resident(set, chunk_index);
}

/// Combined unload rank for budget-driven eviction (B7.5 deepen).
[[nodiscard]] inline f32 eviction_unload_priority(f32 streaming_priority, f32 stored_priority,
                                                   f32 focus_distance, f32 unload_distance_priority,
                                                   u32 last_touch_tick, u32 current_tick,
                                                   LodEvictionPolicy policy) {
    const f32 budget_score = budget_eviction_score(focus_distance, unload_distance_priority, last_touch_tick,
                                                    current_tick, policy);
    return rank_budget_unload_priority(streaming_priority, stored_priority, focus_distance, budget_score);
}

} // namespace fuse::terrain
