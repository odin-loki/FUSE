#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/spatial/aabb.hpp>
#include <fuse/spatial/frustum.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <vector>

namespace fuse::spatial {

enum class BVHLeafType : u8 { Mesh, SDF, VoxelChunk, Light };

struct BVHLeaf {
    BVHLeafType type = BVHLeafType::Mesh;
    ecs::EntityID entity = ecs::EntityID::null();
    u32 primitive_index = 0;
    AABB aabb{};
};

struct BVHNode {
    AABB aabb{};
    u32 left_child = 0;
    u32 right_child = 0;
    u16 leaf_count = 0;
    u8 split_axis = 0;
    u8 pad = 0;
};

struct BVHBuildDesc {
    u32 max_leaf_primitives = 4;
    bool use_sah = true;
    bool parallel = true;
};

class BVH {
public:
    /// Binned-SAH (16 bins) or median build. Leaves are stored reordered; `update_leaf_aabb`
    /// addresses them by their index in `leaves`.
    void build(const std::vector<BVHLeaf>& leaves, const BVHBuildDesc& desc = {});
    /// Replace the bounds of the leaf that was `leaves[source_index]` at build time. Call `refit()`
    /// after a batch of updates. Returns false for an out-of-range index.
    bool update_leaf_aabb(u32 source_index, const AABB& aabb);
    /// Recompute every node's bounds bottom-up from the current leaf bounds (topology unchanged).
    void refit();

    bool ray_cast(const ecs::vec3& origin, const ecs::vec3& direction, f32 max_t, BVHLeaf& hit,
                  f32& t) const;
    /// Exact nearest-hit query. `intersect(leaf, max_t, t)` refines a candidate leaf (whose AABB
    /// the ray enters before the current best hit) to the true surface distance and returns false
    /// on a miss. Nodes and leaves whose AABB entry lies beyond the best refined hit are skipped,
    /// so overlapping bounds cannot hide a nearer surface behind a nearer box. Children are visited
    /// near-first. Returns the leaf with the smallest refined `t` in [0, max_t].
    using LeafIntersector = std::function<bool(const BVHLeaf& leaf, f32 max_t, f32& t)>;
    bool ray_cast_exact(const ecs::vec3& origin, const ecs::vec3& direction, f32 max_t,
                        const LeafIntersector& intersect, BVHLeaf& hit, f32& t) const;

    void query_aabb(const AABB& box, std::vector<BVHLeaf>& results) const;
    void query_sphere(const ecs::vec3& center, f32 radius, std::vector<BVHLeaf>& results) const;
    void query_frustum(const Frustum& frustum, std::vector<BVHLeaf>& results) const;

    const BVHNode* nodes() const { return m_nodes.data(); }
    usize node_count() const { return m_nodes.size(); }
    usize leaf_count() const { return m_leaves.size(); }

private:
    u32 build_recursive(const std::vector<BVHLeaf>& leaves, std::vector<u32>& order, u32 begin, u32 end,
                        u32 depth);
    u32 choose_split(const std::vector<BVHLeaf>& leaves, std::vector<u32>& order, u32 begin, u32 end,
                     u32& axis_out) const;
    bool ray_cast_exact_node(u32 node_index, const ecs::vec3& origin, const ecs::vec3& direction,
                             const LeafIntersector& intersect, BVHLeaf& hit, f32& closest_t) const;
    void query_node(u32 node_index, const AABB& box, std::vector<BVHLeaf>& results) const;
    void query_node_sphere(u32 node_index, const ecs::vec3& center, f32 radius_sq,
                           std::vector<BVHLeaf>& results) const;
    void query_node_frustum(u32 node_index, const Frustum& frustum, std::vector<BVHLeaf>& results) const;
    bool ray_cast_node(u32 node_index, const ecs::vec3& origin, const ecs::vec3& direction, f32 max_t,
                       BVHLeaf& hit, f32& closest_t) const;

    std::vector<BVHNode> m_nodes;
    std::vector<BVHLeaf> m_leaves;
    std::vector<u32> m_slot_of_source; ///< build-input index -> index into m_leaves
    std::vector<ecs::vec3> m_centroids; ///< build scratch, indexed by build-input index
    BVHBuildDesc m_desc{};
};

} // namespace fuse::spatial
