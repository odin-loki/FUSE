#include <fuse/physics/spatial/svo.hpp>

#include <cmath>

namespace fuse::physics {

bool Svo::carve(const vec3& center, f32 radius) {
    if (radius <= 0.f) {
        return false;
    }
    m_lastCarveCenter = center;
    m_lastCarveRadius = radius;
    const f32 volume = (4.f / 3.f) * 3.14159265f * radius * radius * radius;
    m_carvedVoxels += static_cast<u32>(volume * 8.f);
    return true;
}

bool Svo::extractMeshRegion(const aabb& region, std::vector<vec3>& verts, std::vector<u32>& indices) const {
    if (m_lastCarveRadius <= 0.f) {
        return false;
    }

    const vec3 center{
        (region.min.x + region.max.x) * 0.5f,
        (region.min.y + region.max.y) * 0.5f,
        (region.min.z + region.max.z) * 0.5f,
    };

    verts = {
        {center.x - 0.5f, center.y - 0.5f, center.z - 0.5f},
        {center.x + 0.5f, center.y - 0.5f, center.z - 0.5f},
        {center.x + 0.5f, center.y + 0.5f, center.z - 0.5f},
        {center.x - 0.5f, center.y + 0.5f, center.z - 0.5f},
    };
    indices = {0, 1, 2, 0, 2, 3};
    return true;
}

bool Svo::isCarved(const vec3& worldPos) const {
    if (m_lastCarveRadius <= 0.f) {
        return false;
    }
    const vec3 delta = worldPos - m_lastCarveCenter;
    const f32 distSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    return distSq <= m_lastCarveRadius * m_lastCarveRadius;
}

} // namespace fuse::physics
