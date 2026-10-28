#pragma once

namespace fuse::platform {

/// True when `pages` is in the inclusive range 0..3.
[[nodiscard]] inline bool guard_page_ok(int pages) {
    return pages >= 0 && pages <= 3;
}

} // namespace fuse::platform
