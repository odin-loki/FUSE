#pragma once

#include <cmath>

namespace fuse::editor {

/// True when `ms` is finite and in [0, 100].
[[nodiscard]] inline bool idle_ms_ok(float ms) {
    return std::isfinite(ms) && ms >= 0.f && ms <= 100.f;
}

} // namespace fuse::editor
