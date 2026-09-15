#pragma once

#include <fuse/math/vec.hpp>

#include <cmath>

namespace fuse::math {

/// Quaternion (x, y, z, w) with w as the scalar part.
struct Quat {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
    f32 w = 1.f;

    Quat() = default;
    Quat(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}

    static Quat identity() { return {}; }

    f32 dot(const Quat& other) const {
        return x * other.x + y * other.y + z * other.z + w * other.w;
    }

    f32 length() const { return std::sqrt(dot(*this)); }

    Quat normalized() const {
        const f32 len = length();
        if (len < 1e-8f) {
            return identity();
        }
        return {x / len, y / len, z / len, w / len};
    }

    Quat conjugate() const { return {-x, -y, -z, w}; }

    Quat operator*(const Quat& other) const {
        return {
            w * other.x + x * other.w + y * other.z - z * other.y,
            w * other.y - x * other.z + y * other.w + z * other.x,
            w * other.z + x * other.y - y * other.x + z * other.w,
            w * other.w - x * other.x - y * other.y - z * other.z,
        };
    }

    Vec3 rotate(const Vec3& v) const {
        const Vec3 qv{x, y, z};
        const Vec3 t = cross(qv, v) * 2.f;
        return v + t * w + cross(qv, t);
    }
};

inline Quat fromAxisAngle(const Vec3& axis, f32 radians) {
    const Vec3 n = axis.normalized();
    const f32 half = radians * 0.5f;
    const f32 s = std::sin(half);
    return {n.x * s, n.y * s, n.z * s, std::cos(half)};
}

inline Quat slerp(const Quat& a, const Quat& b, f32 t) {
    Quat q0 = a.normalized();
    Quat q1 = b.normalized();

    f32 cos_theta = q0.dot(q1);
    if (cos_theta < 0.f) {
        q1 = {-q1.x, -q1.y, -q1.z, -q1.w};
        cos_theta = -cos_theta;
    }

    if (cos_theta > 0.9995f) {
        return Quat{
            q0.x + t * (q1.x - q0.x),
            q0.y + t * (q1.y - q0.y),
            q0.z + t * (q1.z - q0.z),
            q0.w + t * (q1.w - q0.w),
        }.normalized();
    }

    const f32 theta = std::acos(cos_theta);
    const f32 sin_theta = std::sin(theta);
    const f32 w0 = std::sin((1.f - t) * theta) / sin_theta;
    const f32 w1 = std::sin(t * theta) / sin_theta;

    return {
        q0.x * w0 + q1.x * w1,
        q0.y * w0 + q1.y * w1,
        q0.z * w0 + q1.z * w1,
        q0.w * w0 + q1.w * w1,
    };
}

} // namespace fuse::math
