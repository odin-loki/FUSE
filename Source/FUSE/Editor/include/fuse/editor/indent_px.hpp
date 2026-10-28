#pragma once

namespace fuse::editor {

/// True when indent pixels are in [0, 16].
[[nodiscard]] inline bool indent_px_ok(int px) {
    return px >= 0 && px <= 16;
}

} // namespace fuse::editor
