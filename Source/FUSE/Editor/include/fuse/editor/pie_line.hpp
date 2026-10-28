#pragma once

namespace fuse::editor {

/// True when `length` is in the inclusive range 1..256.
[[nodiscard]] inline constexpr bool pie_line_ok(int length) {
    return length >= 1 && length <= 256;
}

} // namespace fuse::editor
