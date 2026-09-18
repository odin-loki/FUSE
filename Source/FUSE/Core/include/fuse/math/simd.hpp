#pragma once

#include <fuse/math/aabb.hpp>
#include <fuse/math/mat.hpp>
#include <fuse/math/plane.hpp>
#include <fuse/math/vec.hpp>

#include <array>
#include <cmath>

#if defined(FUSE_MATH_SIMD_SCALAR)
// Force scalar lane math even when SSE is available (tests / parity).
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define FUSE_MATH_HAS_SSE 1
#endif

namespace fuse::math::simd {

/// Reports whether the lane backend uses SSE intrinsics (false for scalar stub).
inline bool hasSseBackend() {
#if defined(FUSE_MATH_HAS_SSE)
    return true;
#else
    return false;
#endif
}

/// Four-wide float lane — scalar fallback or SSE `__m128` when available.
struct alignas(16) Float4 {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
    f32 w = 0.f;

#if defined(FUSE_MATH_HAS_SSE)
    __m128 simd() const { return _mm_loadu_ps(&x); }
    static Float4 fromSimd(__m128 value) {
        Float4 result{};
        _mm_storeu_ps(&result.x, value);
        return result;
    }
#endif

    Float4() = default;
    Float4(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
    explicit Float4(const Vec4& v) : x(v.x), y(v.y), z(v.z), w(v.w) {}

    Vec4 toVec4() const { return {x, y, z, w}; }
};

/// Column-major 4×4 matrix stored as four lane columns.
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

namespace detail {

inline Float4 multiplyColumnScalar(const Mat4& matrix, const Float4& vector) {
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

#if defined(FUSE_MATH_HAS_SSE)
inline Float4 multiplyColumnSse(const Mat4& matrix, const Float4& vector) {
    const __m128 vx = _mm_set1_ps(vector.x);
    const __m128 vy = _mm_set1_ps(vector.y);
    const __m128 vz = _mm_set1_ps(vector.z);
    const __m128 vw = _mm_set1_ps(vector.w);

    __m128 acc = _mm_mul_ps(matrix.cols[0].simd(), vx);
    acc = _mm_add_ps(acc, _mm_mul_ps(matrix.cols[1].simd(), vy));
    acc = _mm_add_ps(acc, _mm_mul_ps(matrix.cols[2].simd(), vz));
    acc = _mm_add_ps(acc, _mm_mul_ps(matrix.cols[3].simd(), vw));
    return Float4::fromSimd(acc);
}
#endif

} // namespace detail

inline Float4 multiplyColumn(const Mat4& matrix, const Float4& vector) {
#if defined(FUSE_MATH_HAS_SSE)
    return detail::multiplyColumnSse(matrix, vector);
#else
    return detail::multiplyColumnScalar(matrix, vector);
#endif
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

namespace detail {

inline Vec3 safeNormalized(const Vec3& vector, const Vec3& fallback) {
    const f32 len = vector.length();
    if (len < 1e-8f) {
        return fallback;
    }
    return vector * (1.f / len);
}

} // namespace detail

/// Gram–Schmidt orthonormalization of the upper 3×3 block; translation column preserved.
inline Mat4 orthonormalize(const Mat4& matrix) {
    Vec3 x{matrix.at(0, 0), matrix.at(1, 0), matrix.at(2, 0)};
    Vec3 y{matrix.at(0, 1), matrix.at(1, 1), matrix.at(2, 1)};
    Vec3 z{matrix.at(0, 2), matrix.at(1, 2), matrix.at(2, 2)};

    x = detail::safeNormalized(x, {1.f, 0.f, 0.f});
    y = detail::safeNormalized(y - x * x.dot(y), {0.f, 1.f, 0.f});
    z = cross(x, y);
    z = detail::safeNormalized(z, {0.f, 0.f, 1.f});
    y = cross(z, x).normalized();

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
    const fuse::math::Mat4 scalar = matrix.toScalar();
    const f32 r00 = scalar.data[0];
    const f32 r01 = scalar.data[4];
    const f32 r02 = scalar.data[8];
    const f32 tx = scalar.data[12];
    const f32 r10 = scalar.data[1];
    const f32 r11 = scalar.data[5];
    const f32 r12 = scalar.data[9];
    const f32 ty = scalar.data[13];
    const f32 r20 = scalar.data[2];
    const f32 r21 = scalar.data[6];
    const f32 r22 = scalar.data[10];
    const f32 tz = scalar.data[14];

    Mat4 result = Mat4::identity();
    result.cols[0].x = r00;
    result.cols[0].y = r01;
    result.cols[0].z = r02;
    result.cols[1].x = r10;
    result.cols[1].y = r11;
    result.cols[1].z = r12;
    result.cols[2].x = r20;
    result.cols[2].y = r21;
    result.cols[2].z = r22;
    result.cols[3].x = -(r00 * tx + r10 * ty + r20 * tz);
    result.cols[3].y = -(r01 * tx + r11 * ty + r21 * tz);
    result.cols[3].z = -(r02 * tx + r12 * ty + r22 * tz);
    return result;
}

inline Vec3 transformPoint(const Mat4& matrix, const Vec3& point) {
    const Float4 result = multiplyColumn(matrix, Float4{point.x, point.y, point.z, 1.f});
    return {result.x, result.y, result.z};
}

inline AABB transformAabb(const Mat4& matrix, const AABB& box) {
    if (box.isEmpty()) {
        return box;
    }
    return fuse::math::transformAabb(matrix.toScalar(), box);
}

inline AABB mergeAabb(const AABB& a, const AABB& b) {
    return fuse::math::mergeAabb(a, b);
}

inline f32 rayIntersectAabb(const AABB& box, const Vec3& origin, const Vec3& direction) {
    if (box.isEmpty()) {
        return -1.f;
    }
    return box.rayIntersect(origin, direction);
}

inline f32 planeSignedDistance(const Vec4& plane, const Vec3& point) {
    return fuse::math::planeSignedDistance(plane, point);
}

inline bool isDegeneratePlane(const Vec4& plane, f32 epsilon = 1e-8f) {
    return fuse::math::isDegeneratePlane(plane, epsilon);
}

inline PlaneSide classifyPoint(const Vec4& plane, const Vec3& point, f32 epsilon = 1e-5f) {
    return fuse::math::classifyPoint(plane, point, epsilon);
}

inline PlaneSide classifyAabb(const Vec4& plane, const AABB& box) {
    return fuse::math::classifyAabb(plane, box);
}

inline bool clipSegmentAgainstPlane(const Vec4& plane, Vec3& a, Vec3& b, f32 epsilon = 1e-5f) {
    return fuse::math::clipSegmentAgainstPlane(plane, a, b, epsilon);
}

inline u32 clipPolygonAgainstPlane(const Vec4& plane, const Vec3* input, u32 inputCount, Vec3* output,
                                   u32 maxOutput, f32 epsilon = 1e-5f) {
    return fuse::math::clipPolygonAgainstPlane(plane, input, inputCount, output, maxOutput, epsilon);
}

inline bool isAffine(const fuse::math::Mat4& matrix, f32 epsilon = 1e-5f) {
    return fuse::math::isAffine(matrix, epsilon);
}

inline f32 uniformScaleUpper3x3(const fuse::math::Mat4& matrix, f32 epsilon = 1e-4f) {
    return fuse::math::uniformScaleUpper3x3(matrix, epsilon);
}

inline bool tryInverseRigid(const fuse::math::Mat4& matrix, fuse::math::Mat4& out, f32 epsilon = 1e-4f) {
    return fuse::math::tryInverseRigid(matrix, out, epsilon);
}

inline bool tryNormalizePlane(Vec4& plane, f32 epsilon = 1e-8f) {
    return fuse::math::tryNormalizePlane(plane, epsilon);
}

inline Vec4 makePlaneFromNormalAndPoint(const Vec3& normal, const Vec3& point) {
    return fuse::math::makePlaneFromNormalAndPoint(normal, point);
}

inline bool rayIntervalAabb(const AABB& box, const Vec3& origin, const Vec3& direction, f32& tEnter,
                            f32& tExit) {
    return box.rayInterval(origin, direction, tEnter, tExit);
}

inline bool rayIntervalClampedAabb(const AABB& box, const Vec3& origin, const Vec3& direction, f32 tMin,
                                   f32 tMax, f32& tEnter, f32& tExit) {
    return box.rayIntervalClamped(origin, direction, tMin, tMax, tEnter, tExit);
}

inline bool rayHitsAabb(const AABB& box, const Vec3& origin, const Vec3& direction, f32 tMin = 0.f,
                        f32 tMax = std::numeric_limits<f32>::max()) {
    return box.rayHits(origin, direction, tMin, tMax);
}

inline bool tryPlaneSignedDistance(const Vec4& plane, const Vec3& point, f32& distance,
                                   f32 epsilon = 1e-8f) {
    return fuse::math::tryPlaneSignedDistance(plane, point, distance, epsilon);
}

inline bool tryClassifyPoint(const Vec4& plane, const Vec3& point, PlaneSide& side, f32 epsilon = 1e-5f) {
    return fuse::math::tryClassifyPoint(plane, point, side, epsilon);
}

inline bool rayIntersectPlane(const Vec4& plane, const Vec3& origin, const Vec3& direction, f32& t,
                              f32 epsilon = 1e-8f) {
    return fuse::math::rayIntersectPlane(plane, origin, direction, t, epsilon);
}

inline bool isRigid(const fuse::math::Mat4& matrix, f32 epsilon = 1e-4f) {
    return fuse::math::isRigid(matrix, epsilon);
}

inline Vec3 extractTranslation(const fuse::math::Mat4& matrix, f32 epsilon = 1e-5f) {
    return fuse::math::extractTranslation(matrix, epsilon);
}

inline bool tryExtractTranslation(const fuse::math::Mat4& matrix, Vec3& out, f32 epsilon = 1e-5f) {
    return fuse::math::tryExtractTranslation(matrix, out, epsilon);
}

inline bool tryTransformAabb(const fuse::math::Mat4& matrix, const AABB& box, AABB& out,
                             f32 epsilon = 1e-4f) {
    return fuse::math::tryTransformAabb(matrix, box, out, epsilon);
}

inline bool tryRayIntervalAabb(const AABB& box, const Vec3& origin, const Vec3& direction, f32& tEnter,
                             f32& tExit) {
    return box.tryRayInterval(origin, direction, tEnter, tExit);
}

inline bool tryRayIntervalClampedAabb(const AABB& box, const Vec3& origin, const Vec3& direction, f32 tMin,
                                      f32 tMax, f32& tEnter, f32& tExit) {
    return box.tryRayIntervalClamped(origin, direction, tMin, tMax, tEnter, tExit);
}

inline bool tryRayHitsAabb(const AABB& box, const Vec3& origin, const Vec3& direction, f32 tMin = 0.f,
                           f32 tMax = std::numeric_limits<f32>::max()) {
    return box.tryRayHits(origin, direction, tMin, tMax);
}

inline bool tryRayIntersectAabb(const AABB& box, const Vec3& origin, const Vec3& direction, f32& t) {
    return box.tryRayIntersect(origin, direction, t);
}

inline bool tryClassifyAabb(const Vec4& plane, const AABB& box, PlaneSide& side, f32 epsilon = 1e-8f) {
    return fuse::math::tryClassifyAabb(plane, box, side, epsilon);
}

inline bool tryClipSegmentAgainstPlane(const Vec4& plane, Vec3& a, Vec3& b, f32 epsilon = 1e-5f) {
    return fuse::math::tryClipSegmentAgainstPlane(plane, a, b, epsilon);
}

inline bool tryClipPolygonAgainstPlane(const Vec4& plane, const Vec3* input, u32 inputCount, Vec3* output,
                                       u32 maxOutput, u32& outCount, f32 epsilon = 1e-5f) {
    return fuse::math::tryClipPolygonAgainstPlane(plane, input, inputCount, output, maxOutput, outCount,
                                                 epsilon);
}

inline Vec3 transformDirection(const Mat4& matrix, const Vec3& direction) {
    return fuse::math::transformDirection(matrix.toScalar(), direction);
}

} // namespace fuse::math::simd
