#pragma once

#include <fuse/ecs/entity.hpp>

#include <array>
#include <cmath>

namespace fuse::ecs {

using f32 = float;

/// Homogeneous 3-vector (w stored for SIMD-friendly layout).
struct vec3 {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
    f32 w = 0.f;
};

struct vec4 {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
    f32 w = 0.f;
};

/// Quaternion (x, y, z, w) with w as the scalar part.
struct quat {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
    f32 w = 1.f;
};

/// Column-major 4x4 matrix (OpenGL / Vulkan convention).
struct mat4 {
    std::array<f32, 16> data{};

    static mat4 identity() {
        mat4 m{};
        m.data[0] = 1.f;
        m.data[5] = 1.f;
        m.data[10] = 1.f;
        m.data[15] = 1.f;
        return m;
    }
};

} // namespace fuse::ecs
