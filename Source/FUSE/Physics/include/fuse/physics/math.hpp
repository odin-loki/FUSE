#pragma once

#include <fuse/physics/types.hpp>

#include <cmath>

// Device-safe (FUSE_HOST_DEVICE): the physics compute kernels (broadphase_kernel.hpp) run this math on
// every kernel backend, including CUDA.

namespace fuse::physics {

struct vec2 {
    f32 x = 0.f;
    f32 y = 0.f;

    vec2() = default;
    FUSE_HOST_DEVICE vec2(f32 x_, f32 y_) : x(x_), y(y_) {}

    FUSE_HOST_DEVICE vec2 operator+(const vec2& other) const { return {x + other.x, y + other.y}; }
    FUSE_HOST_DEVICE vec2 operator-(const vec2& other) const { return {x - other.x, y - other.y}; }
    FUSE_HOST_DEVICE vec2 operator*(f32 scalar) const { return {x * scalar, y * scalar}; }

    FUSE_HOST_DEVICE f32 dot(const vec2& other) const { return x * other.x + y * other.y; }
    FUSE_HOST_DEVICE f32 length() const { return std::sqrt(dot(*this)); }
};

struct vec3 {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;

    vec3() = default;
    FUSE_HOST_DEVICE vec3(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}

    FUSE_HOST_DEVICE vec3 operator+(const vec3& other) const { return {x + other.x, y + other.y, z + other.z}; }
    FUSE_HOST_DEVICE vec3 operator-(const vec3& other) const { return {x - other.x, y - other.y, z - other.z}; }
    FUSE_HOST_DEVICE vec3 operator*(f32 scalar) const { return {x * scalar, y * scalar, z * scalar}; }
    FUSE_HOST_DEVICE vec3 operator/(f32 scalar) const { return {x / scalar, y / scalar, z / scalar}; }

    FUSE_HOST_DEVICE vec3& operator+=(const vec3& other) {
        x += other.x;
        y += other.y;
        z += other.z;
        return *this;
    }

    FUSE_HOST_DEVICE vec3& operator-=(const vec3& other) {
        x -= other.x;
        y -= other.y;
        z -= other.z;
        return *this;
    }

    FUSE_HOST_DEVICE f32 dot(const vec3& other) const { return x * other.x + y * other.y + z * other.z; }
    FUSE_HOST_DEVICE f32 length() const { return std::sqrt(dot(*this)); }
    FUSE_HOST_DEVICE vec3 cross(const vec3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }

    FUSE_HOST_DEVICE vec3 normalized() const {
        const f32 len = length();
        if (len < 1e-10f) {
            return {0.f, 1.f, 0.f};
        }
        return *this / len;
    }
};

struct ivec2 {
    s32 x = 0;
    s32 y = 0;
};

struct ivec3 {
    s32 x = 0;
    s32 y = 0;
    s32 z = 0;
};

struct quat {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
    f32 w = 1.f;
};

struct aabb {
    vec3 min{};
    vec3 max{};
};

} // namespace fuse::physics
