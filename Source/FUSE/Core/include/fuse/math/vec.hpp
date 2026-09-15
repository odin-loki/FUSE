#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::math {

struct Vec2 {
    f32 x = 0.f;
    f32 y = 0.f;

    Vec2() = default;
    Vec2(f32 x_, f32 y_) : x(x_), y(y_) {}
};

struct Vec3 {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;

    Vec3() = default;
    Vec3(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& other) const { return {x + other.x, y + other.y, z + other.z}; }
    Vec3 operator-(const Vec3& other) const { return {x - other.x, y - other.y, z - other.z}; }
    Vec3 operator*(f32 scale) const { return {x * scale, y * scale, z * scale}; }

    f32 dot(const Vec3& other) const { return x * other.x + y * other.y + z * other.z; }

    f32 length() const { return std::sqrt(dot(*this)); }

    Vec3 normalized() const {
        const f32 len = length();
        if (len < 1e-8f) {
            return {};
        }
        return *this * (1.f / len);
    }
};

inline Vec3 operator*(f32 scale, const Vec3& v) { return v * scale; }

} // namespace fuse::math
