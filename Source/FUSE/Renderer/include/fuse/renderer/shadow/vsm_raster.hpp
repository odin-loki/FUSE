// VSM raster extent: valid only when it is a power of two in [128, 1024].
#pragma once

namespace fuse::renderer {

/// True when `px` is a power of two and lies in [128, 1024].
[[nodiscard]] inline bool vsm_raster_ok(int px) {
    return px >= 128 && px <= 1024 && (px & (px - 1)) == 0;
}

} // namespace fuse::renderer
