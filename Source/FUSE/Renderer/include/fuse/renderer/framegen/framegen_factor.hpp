#pragma once

namespace fuse::renderer {

/// True when `factor` is 1 or 2.
[[nodiscard]] inline bool framegen_factor_ok(int factor) {
    return factor == 1 || factor == 2;
}

} // namespace fuse::renderer
