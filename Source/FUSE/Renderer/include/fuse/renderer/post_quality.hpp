#pragma once

namespace fuse::renderer {

/// True only for qualities 0 through 3 inclusive.
[[nodiscard]] inline bool post_quality_ok(int quality) {
    return quality >= 0 && quality <= 3;
}

} // namespace fuse::renderer
