#pragma once

#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::audio {

struct Vec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& other) const { return {x + other.x, y + other.y, z + other.z}; }
    Vec3 operator-(const Vec3& other) const { return {x - other.x, y - other.y, z - other.z}; }
    Vec3 operator*(float scale) const { return {x * scale, y * scale, z * scale}; }

    float length() const { return std::sqrt(x * x + y * y + z * z); }
    float distance(const Vec3& other) const { return (*this - other).length(); }
};

struct AABB {
    Vec3 min{};
    Vec3 max{};

    bool contains(const Vec3& point) const {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y
            && point.z >= min.z && point.z <= max.z;
    }
};

} // namespace fuse::audio
