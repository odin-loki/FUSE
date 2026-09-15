#pragma once

#include <fuse/types.hpp>
#include <fuse/world_partition/grid_cell.hpp>

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
    if (focus_distance < 0.f) {
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
    if (focus_distance < 0.f) {
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
    return candidates.empty() ? GridCoord{} : candidates.front();
}

inline std::vector<GridCoord> ResidencySet::collect_eviction_candidates(u32 max_count) const {
    if (m_entries.empty()) {
        return {};
    }

    std::vector<ResidencyEntry> sorted = m_entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const ResidencyEntry& a, const ResidencyEntry& b) {
                  return a.focus_distance > b.focus_distance;
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

/// Stub: register a resident cell; rejects invalid focus distance.
[[nodiscard]] inline bool try_add_resident(ResidencySet& set, GridCoord coord, f32 focus_distance) {
    return set.add(coord, focus_distance);
}

/// Stub: evict a cell from the resident set.
[[nodiscard]] inline bool try_remove_resident(ResidencySet& set, GridCoord coord) {
    return set.remove(coord);
}

} // namespace fuse::world_partition
