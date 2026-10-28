#pragma once

namespace fuse::editor {

/// True when `depth` is in the inclusive range 1..256.
[[nodiscard]] inline constexpr bool console_history_ok(int depth) {
    return depth >= 1 && depth <= 256;
}

} // namespace fuse::editor
