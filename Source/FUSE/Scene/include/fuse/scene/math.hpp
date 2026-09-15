#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::scene {

using f32 = float;

struct vec3 {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;

    vec3() = default;
    vec3(f32 px, f32 py, f32 pz) : x(px), y(py), z(pz) {}

    vec3 operator+(const vec3& other) const { return {x + other.x, y + other.y, z + other.z}; }
    vec3 operator-(const vec3& other) const { return {x - other.x, y - other.y, z - other.z}; }
    vec3 operator*(f32 scale) const { return {x * scale, y * scale, z * scale}; }

    f32 dot(const vec3& other) const { return x * other.x + y * other.y + z * other.z; }
    f32 length() const { return std::sqrt(dot(*this)); }
    vec3 normalized() const {
        const f32 len = length();
        if (len <= 0.f) {
            return {};
        }
        return *this * (1.f / len);
    }
};

struct ivec3 {
    s32 x = 0;
    s32 y = 0;
    s32 z = 0;

    ivec3() = default;
    ivec3(s32 px, s32 py, s32 pz) : x(px), y(py), z(pz) {}

    bool operator==(const ivec3& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

inline vec3 toVec3(const ivec3& v) {
    return vec3(static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z));
}

} // namespace fuse::scene
