#pragma once

namespace fuse::editor {

/// Bit mask of editor docks. Each enumerator is one bit.
enum class EditorPanel {
    Hierarchy = 1,
    Inspector = 2,
    Console = 4,
    Viewport = 8,
};

/// True when `panel`'s bit is set in `mask`.
[[nodiscard]] inline bool panel_visible(unsigned mask, EditorPanel panel) {
    return (mask & static_cast<unsigned>(panel)) != 0u;
}

/// Flip `panel`'s bit. Every other bit is left as it was.
[[nodiscard]] inline unsigned panel_toggle(unsigned mask, EditorPanel panel) {
    return mask ^ static_cast<unsigned>(panel);
}

} // namespace fuse::editor
