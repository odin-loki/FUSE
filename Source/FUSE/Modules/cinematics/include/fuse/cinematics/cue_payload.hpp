#pragma once

// Ore: Verve VScriptEventTrack / VSoundEffectTrack cue dispatch payloads (CPU stubs)

#include <fuse/cinematics/types.hpp>

#include <string>

namespace fuse::cinematics {

enum class CuePayloadKind {
    None,
    ScriptHook,
    AudioClip,
    Custom,
};

/// Typed cue payload stub carried with each enqueued `CueEntry`.
struct CuePayload {
    CuePayloadKind kind = CuePayloadKind::None;
    std::string hook_id;
    std::string asset_id;
    std::string custom_key;
    std::string custom_value;
};

} // namespace fuse::cinematics
