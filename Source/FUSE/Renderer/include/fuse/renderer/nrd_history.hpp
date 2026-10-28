#pragma once

namespace fuse::renderer {

/// True only for lengths 1 through 16.
[[nodiscard]] inline bool nrd_history_ok(int length) {
    return length >= 1 && length <= 16;
}

} // namespace fuse::renderer
