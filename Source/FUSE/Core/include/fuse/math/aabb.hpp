#pragma once

#include <fuse/math/vec.hpp>

#include <algorithm>
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

    bool contains(const Vec3& point) const {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }

    bool overlaps(const AABB& other) const {
        return min.x <= other.max.x && max.x >= other.min.x && min.y <= other.max.y &&
               max.y >= other.min.y && min.z <= other.max.z && max.z >= other.min.z;
    }

    AABB merge(const AABB& other) const {
        return {{std::min(min.x, other.min.x), std::min(min.y, other.min.y), std::min(min.z, other.min.z)},
                {std::max(max.x, other.max.x), std::max(max.y, other.max.y), std::max(max.z, other.max.z)}};
    }

    /// Slab ray intersection. Returns -1 when there is no hit.
    f32 rayIntersect(const Vec3& origin, const Vec3& direction) const {
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
                    return -1.f;
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
                return -1.f;
            }
        }

        if (tmax < 0.f) {
            return -1.f;
        }
        return tmin >= 0.f ? tmin : tmax;
    }
};

} // namespace fuse::math
