#pragma once

namespace fuse::renderer {

/// True only for passes 0 through 8 inclusive.
[[nodiscard]] inline bool phase2_pass_ok(int pass) {
    return pass >= 0 && pass <= 8;
}

} // namespace fuse::renderer
