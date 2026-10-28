#pragma once

#include <fuse/math/vec.hpp>

namespace fuse::renderer {

/// Radical inverse in `base`. Index 0 and any base below 2 return 0.
[[nodiscard]] inline f32 halton(u32 index, u32 base) {
    if (index == 0u || base < 2u) {
        return 0.f;
    }

    f32 result = 0.f;
    f32 fraction = 1.f;
    while (index > 0u) {
        fraction /= static_cast<f32>(base);
        result += fraction * static_cast<f32>(index % base);
        index /= base;
    }
    return result;
}

/// Halton (2, 3) sample. x is base 2, y is base 3.
[[nodiscard]] inline fuse::math::Vec2 halton2(u32 index) {
    return {halton(index, 2u), halton(index, 3u)};
}

} // namespace fuse::renderer
