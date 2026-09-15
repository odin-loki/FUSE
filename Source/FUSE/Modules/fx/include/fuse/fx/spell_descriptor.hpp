#pragma once

#include <fuse/fx/effect_descriptor.hpp>
#include <fuse/fx/fx_defs.hpp>
#include <fuse/types.hpp>

#include <array>
#include <string>
#include <vector>

namespace fuse::fx {

/// Spell phrase table — ore analogue: `afxMagicSpellData` phrase FX lists in `afxMagicSpell.h`.
struct SpellDescriptor {
    std::string id;
    float castingDuration = 0.f;
    float deliveryDuration = 0.f;
    float lingerDuration = 0.f;
    s32 castingLoops = 1;
    s32 deliveryLoops = 1;
    s32 lingerLoops = 1;
    std::array<std::vector<EffectEntry>, static_cast<std::size_t>(SpellPhase::Count)> phaseEntries;

    const std::vector<EffectEntry>& entriesFor(SpellPhase phase) const {
        return phaseEntries[static_cast<std::size_t>(phase)];
    }

    float durationFor(SpellPhase phase) const {
        switch (phase) {
        case SpellPhase::Casting:
            return castingDuration;
        case SpellPhase::Delivery:
            return deliveryDuration;
        case SpellPhase::Linger:
            return lingerDuration;
        default:
            return 0.f;
        }
    }

    static SpellDescriptor makeFireball();
};

} // namespace fuse::fx
