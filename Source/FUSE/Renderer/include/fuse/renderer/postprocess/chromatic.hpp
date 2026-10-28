#pragma once

#include <fuse/math/vec.hpp>

#include <cmath>

namespace fuse::renderer {

struct ChromaticOffset {
    math::Vec2 red{};
    math::Vec2 green{};
    math::Vec2 blue{};
};

/// UV offsets from the frame center. Red shifts outward, blue shifts inward, green stays put.
[[nodiscard]] inline ChromaticOffset chromatic_offset(f32 uvX, f32 uvY, f32 strength) {
    ChromaticOffset offset;
    if (!std::isfinite(uvX) || !std::isfinite(uvY) || !std::isfinite(strength) || strength <= 0.f) {
        return offset;
    }
    const f32 dx = uvX - 0.5f;
    const f32 dy = uvY - 0.5f;
    offset.red = {strength * dx, strength * dy};
    offset.blue = {-offset.red.x, -offset.red.y};
    return offset;
}

} // namespace fuse::renderer
