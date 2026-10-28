#pragma once

namespace fuse::renderer {

/// True only for views 0 through 3 inclusive.
[[nodiscard]] inline bool sky_view_ok(int view) {
    return view >= 0 && view <= 3;
}

} // namespace fuse::renderer
