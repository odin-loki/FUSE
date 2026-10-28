#pragma once

// Weighted loot table. `roll` takes a 0..1 sample and does not draw random numbers.
// Empty ids are ignored. Weights <= 0 are stored as 0 and are never selected.

#include <fuse/math/weighted_pick.hpp>
#include <fuse/types.hpp>

#include <optional>
#include <string>
#include <vector>

namespace fuse::gameplay {

struct LootEntry {
    std::string id;
    f32 weight;
};

class LootTable {
public:
    void add(std::string id, f32 weight) {
        if (id.empty()) {
            return;
        }
        if (weight <= 0.f) {
            weight = 0.f;
        }
        m_entries.push_back(LootEntry{std::move(id), weight});
    }

    /// `u` is a normalized sample in 0..1. Empty when nothing is selectable.
    [[nodiscard]] std::string roll(f32 u) const {
        std::vector<f32> weights;
        weights.reserve(m_entries.size());
        for (const LootEntry& entry : m_entries) {
            weights.push_back(entry.weight);
        }
        const std::optional<std::size_t> index =
            fuse::math::weighted_index(weights.empty() ? nullptr : weights.data(), weights.size(), u);
        if (!index.has_value()) {
            return {};
        }
        return m_entries[*index].id;
    }

private:
    std::vector<LootEntry> m_entries;
};

} // namespace fuse::gameplay
