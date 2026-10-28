// BSDF lobe index. A lobe is valid only when it lies in [0, 7].
#pragma once

namespace fuse::relight {

/// True when `lobe` lies in the inclusive range 0..7.
[[nodiscard]] inline bool bsdf_lobe_ok(int lobe) {
    return lobe >= 0 && lobe <= 7;
}

} // namespace fuse::relight
