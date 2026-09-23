#pragma once

#include <fuse/scene/math.hpp>
#include <fuse/types.hpp>

#include <array>
#include <vector>

namespace fuse::scene {

/// Interior octree node (B3.5). Each covers a cubic region; a child slot holds the index of the
/// next interior node or, one level above the brick depth, a brick index (`SVO::kNoNode` = empty).
struct SVONode {
    u32 children[8] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                       0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
};

/// Leaf brick record: an 8x8x8 voxel block (smaller when `maxDepth` < 3). The payload lives in a
/// pooled word arena and takes one of four forms:
///  - Sparse:  `count` sorted (localIndex, material) pairs; sdf is implicit (+/- half a voxel).
///  - Masked:  one material shared by every written voxel plus a "written" bitmask (fill edges).
///  - Dense:   one material per voxel, a "written" bitmask and an optional per-voxel sdf plane.
///  - Uniform: every voxel written with one material (`payload` holds it) and the implicit sdf.
/// Sparse grows into Masked or Dense; any write Masked/Uniform cannot express promotes to Dense.
struct SVOBrick {
    u32 payload = 0;
    u16 count = 0; ///< sparse entry count
    u8 mode = 0;
    u8 sizeClass = 0;
};

struct SVODesc {
    vec3 origin{};
    f32 rootSize = 1024.f;
    u32 maxDepth = 10;
    bool storeSdf = true;
};

/// Sparse Voxel Octree (B3.5). Interior nodes are walked by octant down to brick leaves
/// (O(depth) get/set). Every written voxel has a material and a signed distance at its centre;
/// `sdfQuery` trilinearly interpolates those samples (continuous across voxel and brick
/// boundaries) and `carve` applies CSG sphere subtraction. `rayCast` is an exact 3D-DDA over the
/// voxel grid that jumps over empty octree cells and empty bricks.
class SVO {
public:
    static constexpr u32 kNoNode = 0xFFFFFFFFu;

    void init(const SVODesc& desc);
    void destroy();

    void set(ivec3 voxelCoord, u32 material);
    [[nodiscard]] u32 get(ivec3 voxelCoord) const;
    void fill(ivec3 minCoord, ivec3 maxCoord, u32 material);
    void carve(vec3 center, f32 radius);

    bool rayCast(vec3 rayOrigin, vec3 rayDirection, f32 maxDistance, ivec3& hitVoxel, vec3& hitNormal,
                 f32& hitDistance) const;

    f32 sdfQuery(vec3 worldPos) const;

    /// Interior nodes plus brick leaves.
    [[nodiscard]] usize nodeCount() const { return m_nodes.size() + m_bricks.size(); }
    [[nodiscard]] usize brickCount() const { return m_bricks.size(); }
    [[nodiscard]] usize voxelCount() const { return m_voxelCount; }
    /// Heap bytes held by the node, brick and payload pools (allocated capacity).
    [[nodiscard]] usize memoryBytes() const;
    [[nodiscard]] const SVODesc& desc() const { return m_desc; }
    [[nodiscard]] bool isInitialized() const { return m_initialized; }

private:
    struct VoxelState {
        bool written = false;
        u32 material = 0;
        f32 sdf = 0.f;
    };

    [[nodiscard]] bool inBounds(ivec3 coord) const;
    [[nodiscard]] f32 leafSize() const;
    [[nodiscard]] vec3 voxelCenterWorld(ivec3 coord) const;
    [[nodiscard]] f32 defaultSdf(u32 material) const;
    [[nodiscard]] f32 sdfAtVoxel(ivec3 coord) const;
    [[nodiscard]] u32 localIndex(ivec3 coord) const;

    [[nodiscard]] u32 findBrick(ivec3 coord) const; ///< kNoNode when no voxel of the brick was written
    u32 ensureBrick(ivec3 coord);
    /// Walks towards `coord`; returns the brick index or kNoNode plus the voxel-space size of the
    /// empty cell that stopped the walk (for ray skipping).
    [[nodiscard]] u32 locate(ivec3 coord, u32& emptyCellSize) const;

    [[nodiscard]] VoxelState readVoxel(u32 brick, u32 local) const;
    void writeVoxel(u32 brick, u32 local, u32 material, f32 sdf);
    void makeUniform(u32 brick, u32 material);
    void toDense(u32 brick);
    void ensureSdfPlane(u32 brick);
    /// Fills `mask` (one bit per voxel) with the brick's solid voxels; returns true if any.
    bool solidMask(u32 brick, u64* mask) const;

    u32 allocWords(u32 sizeClass);
    void freeWords(u32 offset, u32 sizeClass);
    [[nodiscard]] u32 classWords(u32 sizeClass) const;
    [[nodiscard]] u32 denseSdfSlot(u32 brick) const { return m_bricks[brick].payload + m_brickVoxels + m_maskWords; }

    static constexpr u32 kSparseClasses = 7; ///< 1..64 entries (powers of two)
    static constexpr u32 kSparseMax = 64;
    static constexpr u32 kDenseClass = kSparseClasses;
    static constexpr u32 kSdfClass = kSparseClasses + 1;
    static constexpr u32 kMaskedClass = kSparseClasses + 2;
    static constexpr u32 kClassCount = kSparseClasses + 3;

    SVODesc m_desc{};
    u32 m_brickLog = 0;    ///< log2 of the brick edge in voxels
    u32 m_brickDepth = 0;  ///< octree depth at which bricks sit
    u32 m_brickVoxels = 1; ///< voxels per brick
    u32 m_maskWords = 1;   ///< u32 words in a dense brick's written bitmask
    std::vector<SVONode> m_nodes;
    std::vector<SVOBrick> m_bricks;
    std::vector<u32> m_pool;
    std::array<std::vector<u32>, kClassCount> m_freeLists{};
    usize m_voxelCount = 0;
    bool m_initialized = false;
};

} // namespace fuse::scene
