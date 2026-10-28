#pragma once

namespace fuse::editor {

enum class PlayPhase {
    Edit,
    Playing,
    Paused,
};

/// Legal play-session phase transitions.
/// Edit -> Playing, Playing -> Paused, Playing -> Edit, Paused -> Playing, Paused -> Edit.
/// Same-phase transitions and Edit -> Paused are rejected.
[[nodiscard]] inline bool play_phase_can_enter(PlayPhase from, PlayPhase to) {
    if (from == PlayPhase::Edit && to == PlayPhase::Playing) {
        return true;
    }
    if (from == PlayPhase::Playing && (to == PlayPhase::Paused || to == PlayPhase::Edit)) {
        return true;
    }
    if (from == PlayPhase::Paused && (to == PlayPhase::Playing || to == PlayPhase::Edit)) {
        return true;
    }
    return false;
}

} // namespace fuse::editor
