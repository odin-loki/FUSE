#pragma once

#include <fuse/physics/math.hpp>

#include <vector>

namespace fuse::physics {

/// Sparse Voxel Octree surface — stub until Track A B3.5 lands.
class Svo {
public:
    bool carve(const vec3& center, f32 radius);
    bool extractMeshRegion(const aabb& region, std::vector<vec3>& verts, std::vector<u32>& indices) const;

    u32 carvedVoxelCount() const { return m_carvedVoxels; }
    bool isCarved(const vec3& worldPos) const;

private:
    u32 m_carvedVoxels = 0;
    vec3 m_lastCarveCenter{};
    f32 m_lastCarveRadius = 0.f;
};

} // namespace fuse::physics
