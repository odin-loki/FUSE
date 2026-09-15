#pragma once

#include <fuse/ecs/math/vec.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::spatial {

using vec3 = fuse::ecs::vec3;

struct AABB {
    vec3 min{};
    vec3 max{};

    vec3 center() const {
        return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f, 0.f};
    }

    vec3 extents() const {
        return {(max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f, (max.z - min.z) * 0.5f, 0.f};
    }

    f32 surface_area() const {
        const vec3 e = extents();
        return 8.f * (e.x * e.y + e.y * e.z + e.x * e.z);
    }

    bool contains(const vec3& point) const {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }

    bool overlaps(const AABB& other) const {
        return min.x <= other.max.x && max.x >= other.min.x && min.y <= other.max.y &&
               max.y >= other.min.y && min.z <= other.max.z && max.z >= other.min.z;
    }

    AABB merge(const AABB& other) const {
        return {{std::min(min.x, other.min.x), std::min(min.y, other.min.y), std::min(min.z, other.min.z), 0.f},
                {std::max(max.x, other.max.x), std::max(max.y, other.max.y), std::max(max.z, other.max.z), 0.f}};
    }

    f32 ray_intersect(const vec3& origin, const vec3& direction) const {
        f32 tmin = 0.f;
        f32 tmax = std::numeric_limits<f32>::max();

        for (u32 axis = 0; axis < 3; ++axis) {
            const f32 dir = axis == 0 ? direction.x : (axis == 1 ? direction.y : direction.z);
            const f32 origin_axis = axis == 0 ? origin.x : (axis == 1 ? origin.y : origin.z);
            const f32 min_axis = axis == 0 ? min.x : (axis == 1 ? min.y : min.z);
            const f32 max_axis = axis == 0 ? max.x : (axis == 1 ? max.y : max.z);

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

} // namespace fuse::spatial
