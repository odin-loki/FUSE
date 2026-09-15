#pragma once

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
                  return a.focus_distance > b.focus_distance;
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

} // namespace fuse::terrain
