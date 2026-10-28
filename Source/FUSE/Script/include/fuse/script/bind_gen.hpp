#pragma once

namespace fuse::script {

/// True only for generations 0 through 1000000 inclusive.
[[nodiscard]] inline bool bind_gen_ok(int generation) {
    return generation >= 0 && generation <= 1000000;
}

} // namespace fuse::script
