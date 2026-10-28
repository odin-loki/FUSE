// FUSE Relight: a radiance-cache cell id is valid only in [1, 256].
#pragma once

namespace fuse::relight {

/// True when `cell` lies in the closed interval [1, 256].
[[nodiscard]] inline bool rc_cell_ok(int cell) {
    return cell >= 1 && cell <= 256;
}

} // namespace fuse::relight
