#pragma once

#include <fuse/math/mat.hpp>
#include <fuse/math/vec.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace fuse::math {

/// Axis-aligned bounding box in world space.
struct AABB {
    Vec3 min{};
    Vec3 max{};

    static AABB fromCenterExtents(const Vec3& center, const Vec3& halfExtents) {
        return {center - halfExtents, center + halfExtents};
    }

    Vec3 center() const { return (min + max) * 0.5f; }

    Vec3 extents() const { return (max - min) * 0.5f; }

    f32 surfaceArea() const {
        const Vec3 e = extents();
        return 8.f * (e.x * e.y + e.y * e.z + e.x * e.z);
    }

    /// True when any axis has `min > max` (inverted / unset bounds).
    bool isEmpty() const {
        return min.x > max.x || min.y > max.y || min.z > max.z;
    }

    bool isValid() const { return !isEmpty(); }

    bool contains(const Vec3& point) const {
        if (isEmpty()) {
            return false;
        }
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }

    bool overlaps(const AABB& other) const {
        if (isEmpty() || other.isEmpty()) {
            return false;
        }
        return min.x <= other.max.x && max.x >= other.min.x && min.y <= other.max.y &&
               max.y >= other.min.y && min.z <= other.max.z && max.z >= other.min.z;
    }

    AABB merge(const AABB& other) const {
        if (isEmpty()) {
            return other;
        }
        if (other.isEmpty()) {
            return *this;
        }
        return {{std::min(min.x, other.min.x), std::min(min.y, other.min.y), std::min(min.z, other.min.z)},
                {std::max(max.x, other.max.x), std::max(max.y, other.max.y), std::max(max.z, other.max.z)}};
    }

    /// Slab ray interval. Returns false on miss; writes parametric entry/exit into `[tEnter, tExit]`.
    bool rayInterval(const Vec3& origin, const Vec3& direction, f32& tEnter, f32& tExit) const {
        if (isEmpty()) {
            return false;
        }

        const f32 dirLenSq = direction.dot(direction);
        if (dirLenSq < 1e-16f) {
            if (!contains(origin)) {
                return false;
            }
            tEnter = 0.f;
            tExit = 0.f;
            return true;
        }

        f32 tmin = 0.f;
        f32 tmax = std::numeric_limits<f32>::max();

        const f32 origins[3] = {origin.x, origin.y, origin.z};
        const f32 directions[3] = {direction.x, direction.y, direction.z};
        const f32 mins[3] = {min.x, min.y, min.z};
        const f32 maxs[3] = {max.x, max.y, max.z};

        for (u32 axis = 0; axis < 3; ++axis) {
            const f32 dir = directions[axis];
            const f32 origin_axis = origins[axis];
            const f32 min_axis = mins[axis];
            const f32 max_axis = maxs[axis];

            if (std::abs(dir) < 1e-8f) {
                if (origin_axis < min_axis || origin_axis > max_axis) {
                    return false;
                }
                continue;
            }

            const f32 inv_dir = 1.f / dir;
            f32 t1 = (min_axis - origin_axis) * inv_dir;
            f32 t2 = (max_axis - origin_axis) * inv_dir;
            if (t1 > t2) {
                std::swap(t1, t2);
            }
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmax < tmin) {
                return false;
            }
        }

        if (tmax < 0.f) {
            return false;
        }

        tEnter = tmin >= 0.f ? tmin : tmax;
        tExit = tmax;
        return true;
    }

    /// Slab ray intersection. Returns -1 when there is no hit.
    f32 rayIntersect(const Vec3& origin, const Vec3& direction) const {
        f32 tEnter = -1.f;
        f32 tExit = -1.f;
        if (!rayInterval(origin, direction, tEnter, tExit)) {
            return -1.f;
        }
        return tEnter;
    }

    /// Ray interval clamped to `[tMin, tMax]`. Returns false on miss or when the clamp range is inverted.
    bool rayIntervalClamped(const Vec3& origin, const Vec3& direction, f32 tMin, f32 tMax, f32& tEnter,
                            f32& tExit) const {
        if (tMin > tMax) {
            return false;
        }
        if (!rayInterval(origin, direction, tEnter, tExit)) {
            return false;
        }

        tEnter = std::max(tEnter, tMin);
        tExit = std::min(tExit, tMax);
        return tExit >= tEnter;
    }

    /// True when the ray hits the box within `[tMin, tMax]` (inclusive segment guard).
    bool rayHits(const Vec3& origin, const Vec3& direction, f32 tMin = 0.f,
                 f32 tMax = std::numeric_limits<f32>::max()) const {
        f32 tEnter = 0.f;
        f32 tExit = 0.f;
        return rayIntervalClamped(origin, direction, tMin, tMax, tEnter, tExit);
    }
};

/// Transforms an AABB through an affine matrix using the absolute linear-part envelope.
/// Requires an orthogonal 3x3 upper block (rotation ± uniform scale); non-uniform scale
/// should use `transformAabbCorners` for an exact axis-aligned result.
inline AABB transformAabb(const Mat4& matrix, const AABB& box) {
    if (box.isEmpty()) {
        return box;
    }
    const Vec3 center = box.center();
    const Vec3 extents = box.extents();
    const Vec3 newCenter = transformPoint(matrix, center);

    const Mat3 linear = matrix.upper3x3();
    const Vec3 newExtents{
        std::abs(linear.at(0, 0)) * extents.x + std::abs(linear.at(0, 1)) * extents.y +
            std::abs(linear.at(0, 2)) * extents.z,
        std::abs(linear.at(1, 0)) * extents.x + std::abs(linear.at(1, 1)) * extents.y +
            std::abs(linear.at(1, 2)) * extents.z,
        std::abs(linear.at(2, 0)) * extents.x + std::abs(linear.at(2, 1)) * extents.y +
            std::abs(linear.at(2, 2)) * extents.z,
    };
    return AABB::fromCenterExtents(newCenter, newExtents);
}

/// Exact world-space AABB by transforming all eight corners (reference path for tests).
inline AABB transformAabbCorners(const Mat4& matrix, const AABB& box) {
    if (box.isEmpty()) {
        return box;
    }
    const std::array<Vec3, 8> corners = {{
        {box.min.x, box.min.y, box.min.z},
        {box.min.x, box.min.y, box.max.z},
        {box.min.x, box.max.y, box.min.z},
        {box.min.x, box.max.y, box.max.z},
        {box.max.x, box.min.y, box.min.z},
        {box.max.x, box.min.y, box.max.z},
        {box.max.x, box.max.y, box.min.z},
        {box.max.x, box.max.y, box.max.z},
    }};

    Vec3 outMin = transformPoint(matrix, corners[0]);
    Vec3 outMax = outMin;
    for (u32 i = 1; i < corners.size(); ++i) {
        const Vec3 point = transformPoint(matrix, corners[i]);
        outMin.x = std::min(outMin.x, point.x);
        outMin.y = std::min(outMin.y, point.y);
        outMin.z = std::min(outMin.z, point.z);
        outMax.x = std::max(outMax.x, point.x);
        outMax.y = std::max(outMax.y, point.y);
        outMax.z = std::max(outMax.z, point.z);
    }
    return {outMin, outMax};
}

inline AABB mergeAabb(const AABB& a, const AABB& b) {
    return a.merge(b);
}

/// Slab ray interval with empty-box early-out; returns false when `box` is empty.
inline bool tryRayInterval(const AABB& box, const Vec3& origin, const Vec3& direction, f32& tEnter,
                           f32& tExit) {
    if (box.isEmpty()) {
        return false;
    }
    return box.rayInterval(origin, direction, tEnter, tExit);
}

/// Clamped slab ray interval with empty-box early-out.
inline bool tryRayIntervalClamped(const AABB& box, const Vec3& origin, const Vec3& direction, f32 tMin,
                                  f32 tMax, f32& tEnter, f32& tExit) {
    if (box.isEmpty()) {
        return false;
    }
    return box.rayIntervalClamped(origin, direction, tMin, tMax, tEnter, tExit);
}

/// Segment ray hit test with empty-box early-out.
inline bool tryRayHits(const AABB& box, const Vec3& origin, const Vec3& direction, f32 tMin = 0.f,
                       f32 tMax = std::numeric_limits<f32>::max()) {
    if (box.isEmpty()) {
        return false;
    }
    return box.rayHits(origin, direction, tMin, tMax);
}

/// Writes parametric hit distance to `t` when the box is non-empty and the ray hits; returns false on early-out.
inline bool tryRayIntersect(const AABB& box, const Vec3& origin, const Vec3& direction, f32& t) {
    if (box.isEmpty()) {
        return false;
    }
    const f32 hit = box.rayIntersect(origin, direction);
    if (hit < 0.f) {
        return false;
    }
    t = hit;
    return true;
}

/// Writes transformed bounds when `box` is non-empty; returns false on empty early-out.
inline bool tryTransformAabb(const Mat4& matrix, const AABB& box, AABB& out) {
    if (box.isEmpty()) {
        return false;
    }
    out = transformAabb(matrix, box);
    return true;
}

/// Exact corner-transform path with empty-box early-out.
inline bool tryTransformAabbCorners(const Mat4& matrix, const AABB& box, AABB& out) {
    if (box.isEmpty()) {
        return false;
    }
    out = transformAabbCorners(matrix, box);
    return true;
}

/// Merges two boxes when at least one operand is non-empty; returns false when both are empty.
inline bool tryMergeAabb(const AABB& a, const AABB& b, AABB& out) {
    if (a.isEmpty() && b.isEmpty()) {
        return false;
    }
    out = a.merge(b);
    return true;
}

} // namespace fuse::math
