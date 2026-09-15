#include <fuse/scene/bvh_stub.hpp>

#include <cmath>
#include <limits>

namespace fuse::scene {

void BVH::init(f32 worldSize) {
    destroy();
    const f32 half = worldSize * 0.5f;
    m_bounds = AABB{{-half, -half, -half}, {half, half, half}};
    m_initialized = true;
}

void BVH::destroy() {
    m_bounds = {};
    m_initialized = false;
}

void BVH::refit() {
    // Scaffold: bounds fixed at init until B3.4 refit is wired.
}

bool BVH::rayCast(vec3 origin, vec3 direction, f32 maxDistance, f32& hitDistance) const {
    if (!m_initialized) {
        return false;
    }

    const vec3 invDir{
        direction.x != 0.f ? 1.f / direction.x : std::numeric_limits<f32>::infinity(),
        direction.y != 0.f ? 1.f / direction.y : std::numeric_limits<f32>::infinity(),
        direction.z != 0.f ? 1.f / direction.z : std::numeric_limits<f32>::infinity(),
    };

    const f32 t1 = (m_bounds.min.x - origin.x) * invDir.x;
    const f32 t2 = (m_bounds.max.x - origin.x) * invDir.x;
    const f32 t3 = (m_bounds.min.y - origin.y) * invDir.y;
    const f32 t4 = (m_bounds.max.y - origin.y) * invDir.y;
    const f32 t5 = (m_bounds.min.z - origin.z) * invDir.z;
    const f32 t6 = (m_bounds.max.z - origin.z) * invDir.z;

    const f32 tMin = std::max(std::max(std::min(t1, t2), std::min(t3, t4)), std::min(t5, t6));
    const f32 tMax = std::min(std::min(std::max(t1, t2), std::max(t3, t4)), std::max(t5, t6));

    if (tMax < 0.f || tMin > tMax || tMin > maxDistance) {
        return false;
    }

    hitDistance = tMin >= 0.f ? tMin : tMax;
    return hitDistance <= maxDistance;
}

} // namespace fuse::scene
