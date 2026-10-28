#pragma once

namespace fuse::renderer {

/// True only for denoise passes 0 through 4 inclusive.
[[nodiscard]] inline bool denoise_pass_ok(int pass) {
    return pass >= 0 && pass <= 4;
}

} // namespace fuse::renderer
