#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::renderer {

/// 4x4 Bayer threshold in (0.5/16, 15.5/16).
[[nodiscard]] inline f32 bayer4(u32 x, u32 y) {
    static constexpr u32 kMatrix[4][4] = {
        {0u, 8u, 2u, 10u},
        {12u, 4u, 14u, 6u},
        {3u, 11u, 1u, 9u},
        {15u, 7u, 13u, 5u},
    };
    return (static_cast<f32>(kMatrix[y & 3u][x & 3u]) + 0.5f) / 16.f;
}

[[nodiscard]] inline bool dither_pixel(f32 value, u32 x, u32 y) {
    if (!std::isfinite(value)) {
        return false;
    }
    return value > bayer4(x, y);
}

} // namespace fuse::renderer
