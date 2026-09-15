#pragma once

#include <fuse/ecs/math/vec.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::animation {

using vec3 = fuse::ecs::vec3;
using quat = fuse::ecs::quat;
using mat4 = fuse::ecs::mat4;

inline vec3 lerp(const vec3& a, const vec3& b, f32 t) {
    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        0.f,
    };
}

inline quat lerp(const quat& a, const quat& b, f32 t) {
    f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    quat rhs = b;
    if (dot < 0.f) {
        rhs = {-b.x, -b.y, -b.z, -b.w};
        dot = -dot;
    }

    if (dot > 0.9995f) {
        return {
            a.x + t * (rhs.x - a.x),
            a.y + t * (rhs.y - a.y),
            a.z + t * (rhs.z - a.z),
            a.w + t * (rhs.w - a.w),
        };
    }

    const f32 theta = std::acos(std::clamp(dot, -1.f, 1.f));
    const f32 sinTheta = std::sin(theta);
    const f32 w0 = std::sin((1.f - t) * theta) / sinTheta;
    const f32 w1 = std::sin(t * theta) / sinTheta;
    return {
        w0 * a.x + w1 * rhs.x,
        w0 * a.y + w1 * rhs.y,
        w0 * a.z + w1 * rhs.z,
        w0 * a.w + w1 * rhs.w,
    };
}

inline vec3 mat4_translation(const mat4& m) {
    return {m.data[12], m.data[13], m.data[14], 1.f};
}

inline mat4 mat4_multiply(const mat4& a, const mat4& b) {
    mat4 out{};
    for (u32 col = 0; col < 4; ++col) {
        for (u32 row = 0; row < 4; ++row) {
            f32 sum = 0.f;
            for (u32 k = 0; k < 4; ++k) {
                sum += a.data[k * 4 + row] * b.data[col * 4 + k];
            }
            out.data[col * 4 + row] = sum;
        }
    }
    return out;
}

} // namespace fuse::animation
