#include <fuse/spatial/bvh.hpp>

#include <fuse/spatial/frustum.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::spatial {

namespace {

f32 axis_component(const ecs::vec3& value, u32 axis) {
    return axis == 0 ? value.x : (axis == 1 ? value.y : value.z);
}

constexpr u32 kSahBins = 16;
constexpr u32 kMaxLeafCountField = 0xFFFFu; // BVHNode::leaf_count is u16

AABB empty_aabb() {
    const f32 inf = std::numeric_limits<f32>::max();
    return {{inf, inf, inf, 0.f}, {-inf, -inf, -inf, 0.f}};
}

AABB grow(const AABB& box, const ecs::vec3& p) {
    return {{std::min(box.min.x, p.x), std::min(box.min.y, p.y), std::min(box.min.z, p.z), 0.f},
            {std::max(box.max.x, p.x), std::max(box.max.y, p.y), std::max(box.max.z, p.z), 0.f}};
}

f32 half_area(const AABB& box) {
    const f32 dx = std::max(0.f, box.max.x - box.min.x);
    const f32 dy = std::max(0.f, box.max.y - box.min.y);
    const f32 dz = std::max(0.f, box.max.z - box.min.z);
    return dx * dy + dy * dz + dz * dx;
}

} // namespace

u32 BVH::choose_split(const std::vector<BVHLeaf>& leaves, std::vector<u32>& order, u32 begin, u32 end,
                      u32& axis_out) const {
    AABB centroid_bounds = empty_aabb();
    for (u32 i = begin; i < end; ++i) {
        centroid_bounds = grow(centroid_bounds, m_centroids[order[i]]);
    }

    u32 axis = 0;
    const f32 ex = centroid_bounds.max.x - centroid_bounds.min.x;
    const f32 ey = centroid_bounds.max.y - centroid_bounds.min.y;
    const f32 ez = centroid_bounds.max.z - centroid_bounds.min.z;
    if (ey > ex && ey >= ez) {
        axis = 1;
    } else if (ez > ex && ez > ey) {
        axis = 2;
    }
    const f32 lo = axis_component(centroid_bounds.min, axis);
    const f32 extent = axis_component(centroid_bounds.max, axis) - lo;
    const u32 mid = begin + (end - begin) / 2;

    auto median_split = [&]() {
        std::nth_element(order.begin() + begin, order.begin() + mid, order.begin() + end, [&](u32 a, u32 b) {
            return axis_component(m_centroids[a], axis) < axis_component(m_centroids[b], axis);
        });
        axis_out = axis;
        return mid;
    };

    if (!m_desc.use_sah || extent <= 0.f) {
        return median_split(); // coincident centroids: any balanced split is as good as another
    }

    // Binned SAH along the widest centroid axis (Wald 2007): O(n) per node.
    AABB bin_bounds[kSahBins];
    u32 bin_counts[kSahBins] = {};
    for (AABB& b : bin_bounds) {
        b = empty_aabb();
    }
    const f32 scale = static_cast<f32>(kSahBins) / extent;
    auto bin_of = [&](u32 source) {
        const f32 c = axis_component(m_centroids[source], axis);
        return std::min(kSahBins - 1u, static_cast<u32>((c - lo) * scale));
    };
    for (u32 i = begin; i < end; ++i) {
        const u32 b = bin_of(order[i]);
        ++bin_counts[b];
        bin_bounds[b] = bin_counts[b] == 1u ? leaves[order[i]].aabb : bin_bounds[b].merge(leaves[order[i]].aabb);
    }

    f32 right_cost[kSahBins] = {};
    AABB acc = empty_aabb();
    u32 acc_count = 0;
    for (u32 b = kSahBins - 1u; b > 0u; --b) {
        if (bin_counts[b] > 0u) {
            acc = acc_count == 0u ? bin_bounds[b] : acc.merge(bin_bounds[b]);
            acc_count += bin_counts[b];
        }
        right_cost[b] = acc_count == 0u ? 0.f : half_area(acc) * static_cast<f32>(acc_count);
    }

    f32 best_cost = std::numeric_limits<f32>::max();
    u32 best_bin = 0;
    acc = empty_aabb();
    acc_count = 0;
    for (u32 b = 0; b + 1u < kSahBins; ++b) {
        if (bin_counts[b] > 0u) {
            acc = acc_count == 0u ? bin_bounds[b] : acc.merge(bin_bounds[b]);
            acc_count += bin_counts[b];
        }
        const u32 right_count = (end - begin) - acc_count;
        if (acc_count == 0u || right_count == 0u) {
            continue;
        }
        const f32 cost = half_area(acc) * static_cast<f32>(acc_count) + right_cost[b + 1u];
        if (cost < best_cost) {
            best_cost = cost;
            best_bin = b;
        }
    }
    if (best_cost == std::numeric_limits<f32>::max()) {
        return median_split();
    }

    const auto split_it = std::partition(order.begin() + begin, order.begin() + end,
                                         [&](u32 source) { return bin_of(source) <= best_bin; });
    const u32 split = static_cast<u32>(split_it - order.begin());
    if (split == begin || split == end) {
        return median_split();
    }
    axis_out = axis;
    return split;
}

u32 BVH::build_recursive(const std::vector<BVHLeaf>& leaves, std::vector<u32>& order, u32 begin, u32 end,
                         u32 depth) {
    const u32 node_index = static_cast<u32>(m_nodes.size());
    m_nodes.push_back({});
    const u32 count = end - begin;

    AABB bounds = leaves[order[begin]].aabb;
    for (u32 i = begin + 1; i < end; ++i) {
        bounds = bounds.merge(leaves[order[i]].aabb);
    }

    const bool small = count <= m_desc.max_leaf_primitives;
    if (small || (depth > 48 && count <= kMaxLeafCountField)) {
        const u32 leaf_begin = static_cast<u32>(m_leaves.size());
        for (u32 i = begin; i < end; ++i) {
            m_slot_of_source[order[i]] = static_cast<u32>(m_leaves.size());
            m_leaves.push_back(leaves[order[i]]);
        }
        m_nodes[node_index].aabb = bounds;
        m_nodes[node_index].leaf_count = static_cast<u16>(count);
        m_nodes[node_index].left_child = leaf_begin;
        return node_index;
    }

    u32 axis = 0;
    const u32 split = choose_split(leaves, order, begin, end, axis);
    const u32 left_child = build_recursive(leaves, order, begin, split, depth + 1);
    const u32 right_child = build_recursive(leaves, order, split, end, depth + 1);

    m_nodes[node_index].aabb = bounds;
    m_nodes[node_index].split_axis = static_cast<u8>(axis);
    m_nodes[node_index].leaf_count = 0;
    m_nodes[node_index].left_child = left_child;
    m_nodes[node_index].right_child = right_child;
    return node_index;
}

void BVH::build(const std::vector<BVHLeaf>& leaves, const BVHBuildDesc& desc) {
    m_desc = desc;
    if (m_desc.max_leaf_primitives == 0u) {
        m_desc.max_leaf_primitives = 1u;
    }
    m_nodes.clear();
    m_leaves.clear();
    m_slot_of_source.assign(leaves.size(), 0u);

    if (leaves.empty()) {
        return;
    }

    m_centroids.resize(leaves.size());
    std::vector<u32> order(leaves.size());
    for (u32 i = 0; i < leaves.size(); ++i) {
        m_centroids[i] = leaves[i].aabb.center();
        order[i] = i;
    }
    m_nodes.reserve(2u * leaves.size());
    m_leaves.reserve(leaves.size());
    build_recursive(leaves, order, 0, static_cast<u32>(leaves.size()), 0);
}

bool BVH::update_leaf_aabb(u32 source_index, const AABB& aabb) {
    if (source_index >= m_slot_of_source.size()) {
        return false;
    }
    m_leaves[m_slot_of_source[source_index]].aabb = aabb;
    return true;
}

void BVH::refit() {
    // Nodes are allocated pre-order, so every child has a higher index than its parent.
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

bool BVH::ray_cast_exact_node(u32 node_index, const vec3& origin, const vec3& direction,
                              const LeafIntersector& intersect, BVHLeaf& hit, f32& closest_t) const {
    const BVHNode& node = m_nodes[node_index];
    if (node.leaf_count > 0) {
        bool found = false;
        const u32 begin = node.left_child;
        for (u16 i = 0; i < node.leaf_count; ++i) {
            const BVHLeaf& leaf = m_leaves[begin + i];
            const f32 entry = leaf.aabb.ray_intersect(origin, direction);
            if (entry < 0.f || entry > closest_t) {
                continue;
            }
            f32 leaf_t = 0.f;
            if (intersect(leaf, closest_t, leaf_t) && leaf_t >= 0.f && leaf_t <= closest_t) {
                closest_t = leaf_t;
                hit = leaf;
                found = true;
            }
        }
        return found;
    }

    const f32 left_entry = m_nodes[node.left_child].aabb.ray_intersect(origin, direction);
    const f32 right_entry = m_nodes[node.right_child].aabb.ray_intersect(origin, direction);
    const bool right_first = right_entry >= 0.f && (left_entry < 0.f || right_entry < left_entry);
    const u32 first = right_first ? node.right_child : node.left_child;
    const u32 second = right_first ? node.left_child : node.right_child;
    const f32 first_entry = right_first ? right_entry : left_entry;
    const f32 second_entry = right_first ? left_entry : right_entry;

    bool found = false;
    if (first_entry >= 0.f && first_entry <= closest_t &&
        ray_cast_exact_node(first, origin, direction, intersect, hit, closest_t)) {
        found = true;
    }
    // Re-test against the (possibly tightened) best hit before descending the far child.
    if (second_entry >= 0.f && second_entry <= closest_t &&
        ray_cast_exact_node(second, origin, direction, intersect, hit, closest_t)) {
        found = true;
    }
    return found;
}

bool BVH::ray_cast_exact(const vec3& origin, const vec3& direction, f32 max_t, const LeafIntersector& intersect,
                         BVHLeaf& hit, f32& t) const {
    if (m_nodes.empty() || !intersect) {
        return false;
    }
    const f32 root_entry = m_nodes[0].aabb.ray_intersect(origin, direction);
    if (root_entry < 0.f || root_entry > max_t) {
        return false;
    }
    f32 closest_t = max_t;
    if (!ray_cast_exact_node(0, origin, direction, intersect, hit, closest_t)) {
        return false;
    }
    t = closest_t;
    return true;
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
