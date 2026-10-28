#pragma once

namespace fuse::hybrid {

/// True only for edges 0 through 15 inclusive.
[[nodiscard]] inline bool cull_edge_ok(int edge) {
    return edge >= 0 && edge <= 15;
}

} // namespace fuse::hybrid
