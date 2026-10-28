// Post pass: a pass index is valid only when it lies in [0, 8].
#pragma once

namespace fuse::relight {

/// True when `pass` lies in [0, 8].
[[nodiscard]] inline bool post_pass_ok(int pass) {
    return pass >= 0 && pass <= 8;
}

} // namespace fuse::relight
