// ReSTIR PT sample count. A count is accepted only inside the inclusive range 1..32.
#pragma once

namespace fuse::relight {

/// True when `count` is in the inclusive range 1..32.
[[nodiscard]] inline bool restir_pt_count_ok(int count) {
    return count >= 1 && count <= 32;
}

} // namespace fuse::relight
