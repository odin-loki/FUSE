#pragma once

#include <cmath>

namespace fuse::renderer::temporal {

/// True when temporal history must be dropped: a camera cut, the first frame, a non-finite timestep,
/// a timestep longer than half a second, or a negative timestep. A normal 16 ms step keeps history.
[[nodiscard]] inline bool history_should_reset(bool cameraCut, bool firstFrame, float deltaSeconds) {
    return cameraCut || firstFrame || !std::isfinite(deltaSeconds) || deltaSeconds > 0.5f || deltaSeconds < 0.f;
}

} // namespace fuse::renderer::temporal
