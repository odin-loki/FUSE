#pragma once

namespace fuse::renderer {

/// True only for lobes 0 through 3 inclusive.
[[nodiscard]] inline bool brdf_lobe_ok(int lobe) {
    return lobe >= 0 && lobe <= 3;
}

} // namespace fuse::renderer
