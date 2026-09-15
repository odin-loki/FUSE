#pragma once

#include <fuse/scene/math.hpp>
#include <fuse/types.hpp>

namespace fuse::scene {

/// Axis-aligned bounding box used by the B3.4 BVH placeholder.
struct AABB {
    vec3 min{};
    vec3 max{};

    vec3 center() const { return (min + max) * 0.5f; }
    bool contains(vec3 point) const {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }
};

/// B3.4 upstream placeholder — Scene composes this until SAH BVH lands.
class BVH {
public:
    void init(f32 worldSize);
    void destroy();

    void refit();

    bool rayCast(vec3 origin, vec3 direction, f32 maxDistance, f32& hitDistance) const;

    [[nodiscard]] bool isInitialized() const { return m_initialized; }
    [[nodiscard]] AABB bounds() const { return m_bounds; }

private:
    AABB m_bounds{};
    bool m_initialized = false;
};

} // namespace fuse::scene
