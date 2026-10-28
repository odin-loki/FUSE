#pragma once

#include <cmath>
#include <string_view>

namespace fuse::editor {

/// True when two command labels should merge inside a coalesce window.
/// Labels must be equal and both non-empty. Elapsed time must be finite, at least
/// zero, and no greater than the window. The window must be finite and greater than
/// zero. Non-finite times do not coalesce.
[[nodiscard]] inline bool commands_coalesce(std::string_view previous, std::string_view next,
                                            float elapsedSeconds, float windowSeconds) {
    if (previous.empty() || next.empty() || previous != next) {
        return false;
    }
    if (!std::isfinite(elapsedSeconds) || !std::isfinite(windowSeconds)) {
        return false;
    }
    if (windowSeconds <= 0.f) {
        return false;
    }
    return elapsedSeconds >= 0.f && elapsedSeconds <= windowSeconds;
}

} // namespace fuse::editor
