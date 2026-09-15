#pragma once

#include <fuse/types.hpp>

namespace fuse::fx {

/// Spell phrase slots — ore analogue: `afxMagicSpellDefs` in `Engine/source/afx/afxMagicSpell.h`.
enum class SpellPhase : u8 {
    Casting = 0,
    Launch,
    Delivery,
    Impact,
    Linger,
    Count
};

/// Cast lifecycle — ore analogue: `afxMagicSpell` state machine in `afxMagicSpell.h`.
enum class CastState : u8 {
    Inactive = 0,
    Casting,
    Delivery,
    Linger,
    Cleanup,
    Done
};

/// Socket attachment target — ore analogue: AFX constraint / effectron host kinds.
enum class FxSocketKind {
    Sprite2D,
    Shape3D,
};

/// Transient world decoration — ore analogue: `afxResidueMgr` in `afxResidueMgr.h`.
enum class ResidualKind : u8 {
    Zodiac,
    Model,
};

/// Minimal effect condition flags — ore analogue: `afxEffectDefs` exec/impact bits.
enum class EffectCondition : u32 {
    None = 0,
    Enabled = 1u << 0,
    ImpactedSomething = 1u << 31,
    ImpactedTarget = 1u << 30,
};

inline EffectCondition operator|(EffectCondition a, EffectCondition b) {
    return static_cast<EffectCondition>(static_cast<u32>(a) | static_cast<u32>(b));
}

inline bool hasCondition(EffectCondition mask, EffectCondition flag) {
    return (static_cast<u32>(mask) & static_cast<u32>(flag)) != 0;
}

} // namespace fuse::fx
