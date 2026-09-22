#pragma once

#include <fuse/scene/math.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::scene {

/// Each node covers a cubic region of space (B3.5 scaffold layout).
struct SVONode {
    u32 children[8] = {};
    u32 parent = 0;
    u8 depth = 0;
    bool isLeaf = false;
    u32 voxelData = 0;
    f32 sdfValue = 0.f;
};

struct SVODesc {
    vec3 origin{};
    f32 rootSize = 1024.f;
    u32 maxDepth = 10;
    bool storeSdf = true;
};

/// Sparse Voxel Octree (B3.5). Voxels live in depth-`maxDepth` leaves reached by octant walks
/// (O(depth) get/set). Each leaf stores its material and a signed distance at the voxel centre;
/// `sdfQuery` trilinearly interpolates those samples (continuous across leaf boundaries) and
/// `carve` applies CSG sphere subtraction. `rayCast` is an exact 3D-DDA over the voxel grid.
class SVO {
public:
    void init(const SVODesc& desc);
    void destroy();

    void set(ivec3 voxelCoord, u32 material);
    [[nodiscard]] u32 get(ivec3 voxelCoord) const;
    void fill(ivec3 minCoord, ivec3 maxCoord, u32 material);
    void carve(vec3 center, f32 radius);

    bool rayCast(vec3 rayOrigin, vec3 rayDirection, f32 maxDistance, ivec3& hitVoxel, vec3& hitNormal,
                 f32& hitDistance) const;

    f32 sdfQuery(vec3 worldPos) const;

    [[nodiscard]] usize nodeCount() const { return m_nodes.size(); }
    [[nodiscard]] usize voxelCount() const { return m_voxelCount; }
    [[nodiscard]] const SVODesc& desc() const { return m_desc; }
    [[nodiscard]] bool isInitialized() const { return m_initialized; }

private:
    [[nodiscard]] bool inBounds(ivec3 coord) const;
    [[nodiscard]] f32 leafSize() const;
    [[nodiscard]] vec3 voxelCenterWorld(ivec3 coord) const;
    [[nodiscard]] f32 sdfAtVoxel(ivec3 coord) const;
    [[nodiscard]] u32 findLeaf(ivec3 coord) const; ///< kNoNode when the voxel was never written
    u32 ensureLeafNode(ivec3 coord);

    static constexpr u32 kNoNode = 0xFFFFFFFFu;

    SVODesc m_desc{};
    std::vector<SVONode> m_nodes;
    usize m_voxelCount = 0;
    bool m_initialized = false;
};

} // namespace fuse::scene
