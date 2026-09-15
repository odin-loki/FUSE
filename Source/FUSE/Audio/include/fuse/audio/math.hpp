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

    float dot(const Vec3& other) const { return x * other.x + y * other.y + z * other.z; }
    float length() const { return std::sqrt(dot(*this)); }
    float distance(const Vec3& other) const { return (*this - other).length(); }

    Vec3 normalized() const {
        const float len = length();
        if (len < 1e-8f) {
            return {};
        }
        return *this * (1.f / len);
    }
};

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/// Right-handed listener basis from forward and up vectors.
struct ListenerBasis {
    Vec3 forward{};
    Vec3 right{};
    Vec3 up{};
};

inline ListenerBasis make_listener_basis(const Vec3& forward, const Vec3& up) {
    ListenerBasis basis;
    basis.forward = forward.normalized();
    basis.right = cross(basis.forward, up).normalized();
    basis.up = cross(basis.right, basis.forward).normalized();
    return basis;
}

/// Transform a world-space offset into listener-local space.
inline Vec3 to_listener_space(const Vec3& world_relative, const ListenerBasis& basis) {
    return {world_relative.dot(basis.right), world_relative.dot(basis.up),
            world_relative.dot(basis.forward)};
}

struct AABB {
    Vec3 min{};
    Vec3 max{};

    bool contains(const Vec3& point) const {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y
            && point.z >= min.z && point.z <= max.z;
    }
};

} // namespace fuse::audio
