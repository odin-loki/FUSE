#pragma once

namespace fuse::hybrid {

/// True only for passes 0 through 4 inclusive.
[[nodiscard]] inline bool retire_pass_ok(int pass) {
    return pass >= 0 && pass <= 4;
}

} // namespace fuse::hybrid
