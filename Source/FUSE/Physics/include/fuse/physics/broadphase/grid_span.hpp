#pragma once

namespace fuse::physics {

/// True when a grid span is in the inclusive range 1..1024.
[[nodiscard]] inline bool grid_span_ok(int span) {
    return span >= 1 && span <= 1024;
}

} // namespace fuse::physics
