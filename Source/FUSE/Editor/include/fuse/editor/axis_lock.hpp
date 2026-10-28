#pragma once

#include <cmath>

namespace fuse::editor {

enum class LockAxis { X, Y, Z };

struct AxisDelta {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

/// Copies `delta` onto `axis` and leaves the other components at 0.
/// A non-finite `delta` returns all zeros.
[[nodiscard]] inline AxisDelta lock_axis(LockAxis axis, float delta) {
    if (!std::isfinite(delta)) {
        return {};
    }
    AxisDelta out{};
    switch (axis) {
    case LockAxis::X:
        out.x = delta;
        break;
    case LockAxis::Y:
        out.y = delta;
        break;
    case LockAxis::Z:
        out.z = delta;
        break;
    }
    return out;
}

} // namespace fuse::editor
