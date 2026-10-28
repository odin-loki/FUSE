#pragma once

// Faction reputation in [-100, 100]. Unknown factions read as 0.
// add ignores an empty faction and a non-finite delta, then clamps into range.
// standing: Hostile at or below -50, Unfriendly below 0, Neutral at 0,
// Friendly below 50, Honored otherwise.

#include <fuse/types.hpp>

#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace fuse::gameplay {

enum class Standing {
    Hostile,
    Unfriendly,
    Neutral,
    Friendly,
    Honored,
};

class ReputationTable {
public:
    void add(std::string_view faction, f32 delta) {
        if (faction.empty() || !std::isfinite(delta)) {
            return;
        }
        f32& slot = m_values[std::string(faction)];
        const f32 next = slot + delta;
        if (!std::isfinite(next)) {
            slot = delta > 0.f ? 100.f : -100.f;
            return;
        }
        if (next < -100.f) {
            slot = -100.f;
        } else if (next > 100.f) {
            slot = 100.f;
        } else {
            slot = next;
        }
    }

    [[nodiscard]] f32 get(std::string_view faction) const {
        const auto it = m_values.find(faction);
        if (it == m_values.end()) {
            return 0.f;
        }
        return it->second;
    }

    [[nodiscard]] Standing standing(std::string_view faction) const {
        const f32 value = get(faction);
        if (value <= -50.f) {
            return Standing::Hostile;
        }
        if (value < 0.f) {
            return Standing::Unfriendly;
        }
        if (value == 0.f) {
            return Standing::Neutral;
        }
        if (value < 50.f) {
            return Standing::Friendly;
        }
        return Standing::Honored;
    }

private:
    std::map<std::string, f32, std::less<>> m_values;
};

} // namespace fuse::gameplay
