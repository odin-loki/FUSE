// VSM page size: a page extent is valid only when it is a power of two in [64, 512].
#pragma once

namespace fuse::renderer {

/// True when `px` is a power of two and lies in [64, 512].
[[nodiscard]] inline bool vsm_page_ok(int px) {
    return px >= 64 && px <= 512 && (px & (px - 1)) == 0;
}

} // namespace fuse::renderer
