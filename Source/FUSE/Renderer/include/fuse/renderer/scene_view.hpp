#pragma once

namespace fuse::renderer {

/// True only for views 0 through 7 inclusive.
[[nodiscard]] inline bool scene_view_ok(int view) {
    return view >= 0 && view <= 7;
}

} // namespace fuse::renderer
