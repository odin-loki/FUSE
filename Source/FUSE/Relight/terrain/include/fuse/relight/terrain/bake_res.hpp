// Bake resolution: a texel count is valid only when it is a power of two in [64, 2048].
#pragma once

namespace fuse::relight {

/// True when `texels` is a power of two and lies in [64, 2048].
[[nodiscard]] inline bool bake_res_ok(int texels) {
    return texels >= 64 && texels <= 2048 && (texels & (texels - 1)) == 0;
}

} // namespace fuse::relight
