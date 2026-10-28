#pragma once

namespace fuse::mechanics {

/// Flip `current` on a rising press edge. Hold and release leave `current` unchanged.
inline bool toggle_on_press(bool current, bool pressed, bool wasPressed) {
    if (pressed && !wasPressed) {
        return !current;
    }
    return current;
}

} // namespace fuse::mechanics
