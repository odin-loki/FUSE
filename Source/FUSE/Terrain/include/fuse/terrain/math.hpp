#pragma once

#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::terrain {

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
        if (len <= 1e-8f) {
            return {};
        }
        return *this * (1.f / len);
    }
};

struct ivec2 {
    s32 x = 0;
    s32 y = 0;

    ivec2() = default;
    ivec2(s32 px, s32 py) : x(px), y(py) {}

    bool operator==(const ivec2& other) const { return x == other.x && y == other.y; }
};

struct AABB {
    vec3 min{};
    vec3 max{};

    vec3 center() const {
        return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
    }

    bool contains(const vec3& point) const {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }
};

} // namespace fuse::terrain
