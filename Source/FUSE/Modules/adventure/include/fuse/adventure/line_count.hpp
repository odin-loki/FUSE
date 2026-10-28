#pragma once

namespace fuse::adventure {

/// True when a line count is in the inclusive range 1..256.
[[nodiscard]] inline bool line_count_ok(int lines) {
    return lines >= 1 && lines <= 256;
}

} // namespace fuse::adventure
