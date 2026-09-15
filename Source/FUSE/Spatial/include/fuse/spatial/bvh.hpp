#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/spatial/aabb.hpp>
#include <fuse/spatial/frustum.hpp>
#include <fuse/types.hpp>

#include <span>
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
    void build(std::span<BVHLeaf> leaves, const BVHBuildDesc& desc = {});
    void refit();

    bool ray_cast(const ecs::vec3& origin, const ecs::vec3& direction, f32 max_t, BVHLeaf& hit,
                  f32& t) const;
    void query_aabb(const AABB& box, std::vector<BVHLeaf>& results) const;
    void query_sphere(const ecs::vec3& center, f32 radius, std::vector<BVHLeaf>& results) const;
    void query_frustum(const Frustum& frustum, std::vector<BVHLeaf>& results) const;

    const BVHNode* nodes() const { return m_nodes.data(); }
    usize node_count() const { return m_nodes.size(); }
    usize leaf_count() const { return m_leaves.size(); }

private:
    u32 build_recursive(std::span<BVHLeaf> leaves, u32 depth);
    f32 sah_cost(const AABB& parent, const AABB& left, const AABB& right, u32 left_count,
                 u32 right_count) const;
    void query_node(u32 node_index, const AABB& box, std::vector<BVHLeaf>& results) const;
    void query_node_sphere(u32 node_index, const ecs::vec3& center, f32 radius_sq,
                           std::vector<BVHLeaf>& results) const;
    void query_node_frustum(u32 node_index, const Frustum& frustum, std::vector<BVHLeaf>& results) const;
    bool ray_cast_node(u32 node_index, const ecs::vec3& origin, const ecs::vec3& direction, f32 max_t,
                       BVHLeaf& hit, f32& closest_t) const;

    std::vector<BVHNode> m_nodes;
    std::vector<BVHLeaf> m_leaves;
    BVHBuildDesc m_desc{};
};

} // namespace fuse::spatial
