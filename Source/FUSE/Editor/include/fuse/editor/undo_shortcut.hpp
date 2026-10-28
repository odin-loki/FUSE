#pragma once

#include <string_view>

namespace fuse::editor {

/// True when `shortcut` is exactly "Ctrl+Z" or "Ctrl+Y".
[[nodiscard]] inline bool undo_shortcut_ok(std::string_view shortcut) {
    return shortcut == "Ctrl+Z" || shortcut == "Ctrl+Y";
}

} // namespace fuse::editor
