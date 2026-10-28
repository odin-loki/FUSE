#pragma once

#include <string_view>

namespace fuse::editor {

/// True when `side` is exactly "Left", "Right", "Top", or "Bottom".
[[nodiscard]] inline bool dock_side_ok(std::string_view side) {
    return side == "Left" || side == "Right" || side == "Top" || side == "Bottom";
}

} // namespace fuse::editor
