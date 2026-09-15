#pragma once

#include <fuse/ecs/math/vec.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::animation {

using vec3 = fuse::ecs::vec3;
using quat = fuse::ecs::quat;
using mat4 = fuse::ecs::mat4;

struct vec2 {
    f32 x = 0.f;
    f32 y = 0.f;
};

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

inline mat4 mat4_from_trs(const vec3& translation, const quat& rotation, const vec3& scale) {
    const f32 xx = rotation.x * rotation.x;
    const f32 yy = rotation.y * rotation.y;
    const f32 zz = rotation.z * rotation.z;
    const f32 xy = rotation.x * rotation.y;
    const f32 xz = rotation.x * rotation.z;
    const f32 yz = rotation.y * rotation.z;
    const f32 wx = rotation.w * rotation.x;
    const f32 wy = rotation.w * rotation.y;
    const f32 wz = rotation.w * rotation.z;

    mat4 out{};
    out.data[0] = scale.x * (1.f - 2.f * (yy + zz));
    out.data[1] = scale.x * (2.f * (xy + wz));
    out.data[2] = scale.x * (2.f * (xz - wy));
    out.data[4] = scale.y * (2.f * (xy - wz));
    out.data[5] = scale.y * (1.f - 2.f * (xx + zz));
    out.data[6] = scale.y * (2.f * (yz + wx));
    out.data[8] = scale.z * (2.f * (xz + wy));
    out.data[9] = scale.z * (2.f * (yz - wx));
    out.data[10] = scale.z * (1.f - 2.f * (xx + yy));
    out.data[12] = translation.x;
    out.data[13] = translation.y;
    out.data[14] = translation.z;
    out.data[15] = 1.f;
    return out;
}

inline void decompose_trs(const mat4& matrix, vec3& out_translation, quat& out_rotation, vec3& out_scale) {
    out_translation = mat4_translation(matrix);
    out_scale = {matrix.data[0], matrix.data[5], matrix.data[10], 0.f};

    const f32 trace = matrix.data[0] + matrix.data[5] + matrix.data[10];
    if (trace > 0.f) {
        const f32 s = std::sqrt(trace + 1.f) * 2.f;
        out_rotation = {
            (matrix.data[9] - matrix.data[6]) / s,
            (matrix.data[2] - matrix.data[8]) / s,
            (matrix.data[4] - matrix.data[1]) / s,
            0.25f * s,
        };
    } else {
        out_rotation = {0.f, 0.f, 0.f, 1.f};
    }
}

} // namespace fuse::animation
