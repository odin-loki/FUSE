#pragma once

namespace fuse::script {

/// True only when source changed and the session is not playing.
/// Editing during play does not reload.
[[nodiscard]] inline bool hot_reload_due(bool sourceChanged, bool playing) {
    return sourceChanged && !playing;
}

} // namespace fuse::script
