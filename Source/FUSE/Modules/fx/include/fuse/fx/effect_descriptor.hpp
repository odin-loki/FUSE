#pragma once

#include <fuse/fx/fx_defs.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::fx {

/// Per-effect timing — ore analogue: `afxEffectTimingData` in `afxEffectWrapper.h`.
struct EffectTiming {
    float delay = 0.f;
    float lifetime = 0.f;
    float fadeIn = 0.f;
    float fadeOut = 0.f;
    float residueLifetime = 0.f;
    float residueFade = 0.f;
};

/// One wrapped effect entry in a phrase list — ore analogue: `afxEffectWrapperData`.
struct EffectEntry {
    std::string effectTypeId;
    EffectTiming timing;
    EffectCondition conditions = EffectCondition::Enabled;
};

/// Standalone timed effect bundle — ore analogue: `afxEffectronData` in `afxEffectron.h`.
struct EffectDescriptor {
    std::string id;
    float duration = 0.f;
    s32 loopCount = 1;
    std::vector<EffectEntry> entries;

    static EffectDescriptor makeSparkBurst();
};

} // namespace fuse::fx
