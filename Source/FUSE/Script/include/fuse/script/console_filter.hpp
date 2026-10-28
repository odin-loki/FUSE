#pragma once

namespace fuse::script {

/// True when a script-console message at `messageLevel` passes `minimumLevel`.
/// Both levels are clamped into 0..4 before the compare. Negative becomes 0. Above 4 becomes 4.
[[nodiscard]] inline bool console_shows(int messageLevel, int minimumLevel) {
    const auto clampLevel = [](int level) {
        if (level < 0) {
            return 0;
        }
        if (level > 4) {
            return 4;
        }
        return level;
    };
    return clampLevel(messageLevel) >= clampLevel(minimumLevel);
}

} // namespace fuse::script
