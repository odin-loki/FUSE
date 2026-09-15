#pragma once

#include <fuse/math/aabb.hpp>
#include <fuse/math/mat.hpp>
#include <fuse/math/plane.hpp>
#include <fuse/math/vec.hpp>

#include <array>
#include <cmath>

namespace fuse::math::simd {

/// Four-wide float lane — scalar CPU stub (future `__m128` backend).
struct alignas(16) Float4 {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
    f32 w = 0.f;

    Float4() = default;
    Float4(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
    explicit Float4(const Vec4& v) : x(v.x), y(v.y), z(v.z), w(v.w) {}

    Vec4 toVec4() const { return {x, y, z, w}; }
};

/// Column-major 4×4 matrix stored as four lane columns (SIMD stub layout).
struct alignas(16) Mat4 {
    std::array<Float4, 4> cols{};

    static Mat4 identity() {
        Mat4 m{};
        m.cols[0] = {1.f, 0.f, 0.f, 0.f};
        m.cols[1] = {0.f, 1.f, 0.f, 0.f};
        m.cols[2] = {0.f, 0.f, 1.f, 0.f};
        m.cols[3] = {0.f, 0.f, 0.f, 1.f};
        return m;
    }

    static Mat4 fromScalar(const fuse::math::Mat4& matrix) {
        Mat4 result{};
        for (u32 col = 0; col < 4; ++col) {
            result.cols[col] = Float4{
                matrix.at(0, col),
                matrix.at(1, col),
                matrix.at(2, col),
                matrix.at(3, col),
            };
        }
        return result;
    }

    fuse::math::Mat4 toScalar() const {
        fuse::math::Mat4 result{};
        for (u32 col = 0; col < 4; ++col) {
            result.at(0, col) = cols[col].x;
            result.at(1, col) = cols[col].y;
            result.at(2, col) = cols[col].z;
            result.at(3, col) = cols[col].w;
        }
        return result;
    }

    f32 at(u32 row, u32 col) const {
        const Float4& column = cols[col];
        switch (row) {
        case 0:
            return column.x;
        case 1:
            return column.y;
        case 2:
            return column.z;
        default:
            return column.w;
        }
    }
};

inline Float4 multiplyColumn(const Mat4& matrix, const Float4& vector) {
    Float4 result{};
    result.x = matrix.cols[0].x * vector.x + matrix.cols[1].x * vector.y + matrix.cols[2].x * vector.z +
               matrix.cols[3].x * vector.w;
    result.y = matrix.cols[0].y * vector.x + matrix.cols[1].y * vector.y + matrix.cols[2].y * vector.z +
               matrix.cols[3].y * vector.w;
    result.z = matrix.cols[0].z * vector.x + matrix.cols[1].z * vector.y + matrix.cols[2].z * vector.z +
               matrix.cols[3].z * vector.w;
    result.w = matrix.cols[0].w * vector.x + matrix.cols[1].w * vector.y + matrix.cols[2].w * vector.z +
               matrix.cols[3].w * vector.w;
    return result;
}

inline Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 result{};
    for (u32 col = 0; col < 4; ++col) {
        result.cols[col] = multiplyColumn(a, b.cols[col]);
    }
    return result;
}

inline bool isOrthogonalUpper3x3(const Mat4& matrix, f32 epsilon = 1e-4f) {
    const Vec3 x{matrix.at(0, 0), matrix.at(1, 0), matrix.at(2, 0)};
    const Vec3 y{matrix.at(0, 1), matrix.at(1, 1), matrix.at(2, 1)};
    const Vec3 z{matrix.at(0, 2), matrix.at(1, 2), matrix.at(2, 2)};

    const f32 xy = std::fabs(x.dot(y));
    const f32 xz = std::fabs(x.dot(z));
    const f32 yz = std::fabs(y.dot(z));
    const f32 xLen = x.length();
    const f32 yLen = y.length();
    const f32 zLen = z.length();

    if (xLen < epsilon || yLen < epsilon || zLen < epsilon) {
        return false;
    }

    const f32 unitTolerance = epsilon * std::max(1.f, std::max(xLen, std::max(yLen, zLen)));
    if (std::fabs(xLen - 1.f) > unitTolerance || std::fabs(yLen - 1.f) > unitTolerance ||
        std::fabs(zLen - 1.f) > unitTolerance) {
        return false;
    }

    return xy <= unitTolerance && xz <= unitTolerance && yz <= unitTolerance;
}

/// Gram–Schmidt orthonormalization of the upper 3×3 block (CPU stub).
inline Mat4 orthonormalize(const Mat4& matrix) {
    Vec3 x{matrix.at(0, 0), matrix.at(1, 0), matrix.at(2, 0)};
    Vec3 y{matrix.at(0, 1), matrix.at(1, 1), matrix.at(2, 1)};
    Vec3 z{matrix.at(0, 2), matrix.at(1, 2), matrix.at(2, 2)};

    x = x.normalized();
    y = (y - x * x.dot(y)).normalized();
    z = cross(x, y).normalized();

    Mat4 result = matrix;
    result.cols[0].x = x.x;
    result.cols[0].y = x.y;
    result.cols[0].z = x.z;
    result.cols[1].x = y.x;
    result.cols[1].y = y.y;
    result.cols[1].z = y.z;
    result.cols[2].x = z.x;
    result.cols[2].y = z.y;
    result.cols[2].z = z.z;
    return result;
}

inline Mat4 inverseAffine(const Mat4& matrix) {
    return Mat4::fromScalar(fuse::math::inverseAffine(matrix.toScalar()));
}

inline Vec3 transformPoint(const Mat4& matrix, const Vec3& point) {
    const Float4 result = multiplyColumn(matrix, Float4{point.x, point.y, point.z, 1.f});
    return {result.x, result.y, result.z};
}

inline AABB transformAabb(const Mat4& matrix, const AABB& box) {
    return fuse::math::transformAabb(matrix.toScalar(), box);
}

inline AABB mergeAabb(const AABB& a, const AABB& b) {
    return a.merge(b);
}

inline f32 rayIntersectAabb(const AABB& box, const Vec3& origin, const Vec3& direction) {
    return box.rayIntersect(origin, direction);
}

} // namespace fuse::math::simd
