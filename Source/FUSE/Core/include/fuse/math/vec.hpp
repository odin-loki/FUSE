#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::math {

struct Vec2 {
    f32 x = 0.f;
    f32 y = 0.f;

    Vec2() = default;
    FUSE_HOST_DEVICE Vec2(f32 x_, f32 y_) : x(x_), y(y_) {}

    FUSE_HOST_DEVICE Vec2 operator+(const Vec2& other) const { return {x + other.x, y + other.y}; }
    FUSE_HOST_DEVICE Vec2 operator-(const Vec2& other) const { return {x - other.x, y - other.y}; }
    FUSE_HOST_DEVICE Vec2 operator*(f32 scale) const { return {x * scale, y * scale}; }

    FUSE_HOST_DEVICE f32 dot(const Vec2& other) const { return x * other.x + y * other.y; }
    FUSE_HOST_DEVICE f32 length() const { return std::sqrt(dot(*this)); }
};

struct Vec3 {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;

    Vec3() = default;
    FUSE_HOST_DEVICE Vec3(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}

    FUSE_HOST_DEVICE Vec3 operator+(const Vec3& other) const { return {x + other.x, y + other.y, z + other.z}; }
    FUSE_HOST_DEVICE Vec3 operator-(const Vec3& other) const { return {x - other.x, y - other.y, z - other.z}; }
    FUSE_HOST_DEVICE Vec3 operator*(f32 scale) const { return {x * scale, y * scale, z * scale}; }

    FUSE_HOST_DEVICE f32 dot(const Vec3& other) const { return x * other.x + y * other.y + z * other.z; }

    FUSE_HOST_DEVICE f32 length() const { return std::sqrt(dot(*this)); }

    FUSE_HOST_DEVICE Vec3 normalized() const {
        const f32 len = length();
        if (len < 1e-8f) {
            return {};
        }
        return *this * (1.f / len);
    }
};

struct Vec4 {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
    f32 w = 0.f;

    Vec4() = default;
    FUSE_HOST_DEVICE Vec4(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
    FUSE_HOST_DEVICE Vec4(const Vec3& v, f32 w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
};

FUSE_HOST_DEVICE inline Vec2 operator*(f32 scale, const Vec2& v) { return v * scale; }
FUSE_HOST_DEVICE inline Vec3 operator*(f32 scale, const Vec3& v) { return v * scale; }

FUSE_HOST_DEVICE inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

} // namespace fuse::math
