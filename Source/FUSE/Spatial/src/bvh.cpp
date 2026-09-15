#include <fuse/spatial/bvh.hpp>

#include <fuse/jobs/parallel_for.hpp>
#include <fuse/spatial/frustum.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::spatial {

namespace {

f32 axis_component(const ecs::vec3& value, u32 axis) {
    return axis == 0 ? value.x : (axis == 1 ? value.y : value.z);
}

AABB bounds_for_leaves(std::span<BVHLeaf> leaves) {
    AABB bounds = leaves[0].aabb;
    for (usize i = 1; i < leaves.size(); ++i) {
        bounds = bounds.merge(leaves[i].aabb);
    }
    return bounds;
}

} // namespace

f32 BVH::sah_cost(const AABB& parent, const AABB& left, const AABB& right, u32 left_count,
                  u32 right_count) const {
    const f32 parent_area = parent.surface_area();
    if (parent_area <= 0.f) {
        return 0.f;
    }
    return 1.f + (left.surface_area() * left_count + right.surface_area() * right_count) / parent_area;
}

u32 BVH::build_recursive(std::span<BVHLeaf> leaves, u32 depth) {
    const u32 node_index = static_cast<u32>(m_nodes.size());
    m_nodes.push_back({});
    const AABB bounds = bounds_for_leaves(leaves);

    if (leaves.size() <= m_desc.max_leaf_primitives || depth > 32) {
        const u32 leaf_begin = static_cast<u32>(m_leaves.size());
        for (const BVHLeaf& leaf : leaves) {
            m_leaves.push_back(leaf);
        }
        m_nodes[node_index].aabb = bounds;
        m_nodes[node_index].leaf_count = static_cast<u16>(leaves.size());
        m_nodes[node_index].left_child = leaf_begin;
        return node_index;
    }

    u32 best_axis = 0;
    u32 best_split = static_cast<u32>(leaves.size() / 2);
    f32 best_cost = std::numeric_limits<f32>::max();

    if (m_desc.use_sah) {
        for (u32 axis = 0; axis < 3; ++axis) {
            std::sort(leaves.begin(), leaves.end(), [axis](const BVHLeaf& a, const BVHLeaf& b) {
                return axis_component(a.aabb.center(), axis) < axis_component(b.aabb.center(), axis);
            });

            for (u32 split = 1; split < leaves.size(); ++split) {
                AABB left = leaves[0].aabb;
                for (u32 i = 1; i < split; ++i) {
                    left = left.merge(leaves[i].aabb);
                }
                AABB right = leaves[split].aabb;
                for (u32 i = split + 1; i < leaves.size(); ++i) {
                    right = right.merge(leaves[i].aabb);
                }
                const f32 cost =
                    sah_cost(bounds, left, right, split, static_cast<u32>(leaves.size()) - split);
                if (cost < best_cost) {
                    best_cost = cost;
                    best_axis = axis;
                    best_split = split;
                }
            }
        }
    }

    std::sort(leaves.begin(), leaves.end(), [best_axis](const BVHLeaf& a, const BVHLeaf& b) {
        return axis_component(a.aabb.center(), best_axis) < axis_component(b.aabb.center(), best_axis);
    });

    const u32 left_child = build_recursive(leaves.subspan(0, best_split), depth + 1);
    const u32 right_child = build_recursive(leaves.subspan(best_split), depth + 1);

    m_nodes[node_index].aabb = bounds;
    m_nodes[node_index].split_axis = static_cast<u8>(best_axis);
    m_nodes[node_index].leaf_count = 0;
    m_nodes[node_index].left_child = left_child;
    m_nodes[node_index].right_child = right_child;
    return node_index;
}

void BVH::build(std::span<BVHLeaf> leaves, const BVHBuildDesc& desc) {
    m_desc = desc;
    m_nodes.clear();
    m_leaves.clear();
    m_leaf_ranges.clear();

    if (leaves.empty()) {
        return;
    }

    std::vector<BVHLeaf> working(leaves.begin(), leaves.end());
    if (m_desc.parallel && working.size() > 1024) {
        fuse::jobs::parallel_for(static_cast<u32>(working.size()), [&](u32) {});
    }
    build_recursive(working, 0);
}

void BVH::refit() {
    if (m_nodes.empty()) {
        return;
    }

    for (usize node_index = m_nodes.size(); node_index-- > 0;) {
        BVHNode& node = m_nodes[node_index];
        if (node.leaf_count > 0) {
            const u32 begin = node.left_child;
            node.aabb = m_leaves[begin].aabb;
            for (u16 i = 1; i < node.leaf_count; ++i) {
                node.aabb = node.aabb.merge(m_leaves[begin + i].aabb);
            }
            continue;
        }

        node.aabb = m_nodes[node.left_child].aabb.merge(m_nodes[node.right_child].aabb);
    }
}

bool BVH::ray_cast_node(u32 node_index, const vec3& origin, const vec3& direction, f32 max_t, BVHLeaf& hit,
                        f32& closest_t) const {
    const BVHNode& node = m_nodes[node_index];
    const f32 entry = node.aabb.ray_intersect(origin, direction);
    if (entry < 0.f || entry > closest_t) {
        return false;
    }

    bool found = false;
    if (node.leaf_count > 0) {
        const u32 begin = node.left_child;
        for (u16 i = 0; i < node.leaf_count; ++i) {
            const BVHLeaf& leaf = m_leaves[begin + i];
            const f32 leaf_t = leaf.aabb.ray_intersect(origin, direction);
            if (leaf_t >= 0.f && leaf_t < closest_t) {
                closest_t = leaf_t;
                hit = leaf;
                found = true;
            }
        }
        return found;
    }

    if (ray_cast_node(node.left_child, origin, direction, max_t, hit, closest_t)) {
        found = true;
    }
    if (ray_cast_node(node.right_child, origin, direction, max_t, hit, closest_t)) {
        found = true;
    }
    return found;
}

bool BVH::ray_cast(const vec3& origin, const vec3& direction, f32 max_t, BVHLeaf& hit, f32& t) const {
    if (m_nodes.empty()) {
        return false;
    }

    f32 closest_t = max_t;
    return ray_cast_node(0, origin, direction, max_t, hit, closest_t) ? (t = closest_t, true) : false;
}

void BVH::query_node(u32 node_index, const AABB& box, std::vector<BVHLeaf>& results) const {
    const BVHNode& node = m_nodes[node_index];
    if (!node.aabb.overlaps(box)) {
        return;
    }

    if (node.leaf_count > 0) {
        const u32 begin = node.left_child;
        for (u16 i = 0; i < node.leaf_count; ++i) {
            const BVHLeaf& leaf = m_leaves[begin + i];
            if (leaf.aabb.overlaps(box)) {
                results.push_back(leaf);
            }
        }
        return;
    }

    query_node(node.left_child, box, results);
    query_node(node.right_child, box, results);
}

void BVH::query_aabb(const AABB& box, std::vector<BVHLeaf>& results) const {
    if (!m_nodes.empty()) {
        query_node(0, box, results);
    }
}

void BVH::query_node_sphere(u32 node_index, const vec3& center, f32 radius_sq,
                            std::vector<BVHLeaf>& results) const {
    const BVHNode& node = m_nodes[node_index];
    const vec3 closest{
        std::max(node.aabb.min.x, std::min(center.x, node.aabb.max.x)),
        std::max(node.aabb.min.y, std::min(center.y, node.aabb.max.y)),
        std::max(node.aabb.min.z, std::min(center.z, node.aabb.max.z)),
        0.f,
    };
    const f32 dx = center.x - closest.x;
    const f32 dy = center.y - closest.y;
    const f32 dz = center.z - closest.z;
    if (dx * dx + dy * dy + dz * dz > radius_sq) {
        return;
    }

    if (node.leaf_count > 0) {
        const u32 begin = node.left_child;
        for (u16 i = 0; i < node.leaf_count; ++i) {
            results.push_back(m_leaves[begin + i]);
        }
        return;
    }

    query_node_sphere(node.left_child, center, radius_sq, results);
    query_node_sphere(node.right_child, center, radius_sq, results);
}

void BVH::query_sphere(const vec3& center, f32 radius, std::vector<BVHLeaf>& results) const {
    if (!m_nodes.empty()) {
        query_node_sphere(0, center, radius * radius, results);
    }
}

void BVH::query_node_frustum(u32 node_index, const Frustum& frustum, std::vector<BVHLeaf>& results) const {
    const BVHNode& node = m_nodes[node_index];
    if (!test_aabb_frustum(frustum, node.aabb.min, node.aabb.max)) {
        return;
    }

    if (node.leaf_count > 0) {
        const u32 begin = node.left_child;
        for (u16 i = 0; i < node.leaf_count; ++i) {
            const BVHLeaf& leaf = m_leaves[begin + i];
            if (test_aabb_frustum(frustum, leaf.aabb.min, leaf.aabb.max)) {
                results.push_back(leaf);
            }
        }
        return;
    }

    query_node_frustum(node.left_child, frustum, results);
    query_node_frustum(node.right_child, frustum, results);
}

void BVH::query_frustum(const Frustum& frustum, std::vector<BVHLeaf>& results) const {
    if (!m_nodes.empty()) {
        query_node_frustum(0, frustum, results);
    }
}

} // namespace fuse::spatial
