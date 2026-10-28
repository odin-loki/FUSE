#pragma once

#include <cstdint>

namespace fuse::relight {

/// True when `count` is in the closed range [3, 1<<20].
[[nodiscard]] inline bool capture_verts_ok(std::uint32_t count) {
    return count >= 3u && count <= (1u << 20);
}

} // namespace fuse::relight
