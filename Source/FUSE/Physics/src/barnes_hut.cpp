#include <fuse/physics/nbody/barnes_hut.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::physics {
namespace {

constexpr u32 kOctantCount = 8;

struct BuildNode {
    vec3 min_bounds{};
    vec3 max_bounds{};
    std::vector<u32> body_indices;
    u32 child_start = 0;
    u32 child_count = 0;
};

vec3 nodeSize(const BuildNode& node) {
    return node.max_bounds - node.min_bounds;
}

vec3 nodeCenter(const BuildNode& node) {
    return (node.min_bounds + node.max_bounds) * 0.5f;
}

int octantIndex(const vec3& point, const vec3& center) {
    int index = 0;
    if (point.x >= center.x) {
        index |= 1;
    }
    if (point.y >= center.y) {
        index |= 2;
    }
    if (point.z >= center.z) {
        index |= 4;
    }
    return index;
}

vec3 computeCenterOfMass(const std::vector<vec3>& positions,
                         const std::vector<f32>& masses,
                         const std::vector<u32>& indices) {
    vec3 weighted{};
    f32 total_mass = 0.f;
    for (u32 index : indices) {
        weighted += positions[index] * masses[index];
        total_mass += masses[index];
    }
    if (total_mass < 1e-12f) {
        return {};
    }
    return weighted * (1.f / total_mass);
}

u32 subdivide(BuildNode& node,
              std::vector<BuildNode>& build_nodes,
              const std::vector<vec3>& positions,
              u32 depth,
              const BHParams& params) {
    if (node.body_indices.size() <= 1 || depth >= params.max_depth) {
        return 0;
    }

    const vec3 center = nodeCenter(node);
    const vec3 half = nodeSize(node) * 0.5f;
    if (half.x < 1e-6f && half.y < 1e-6f && half.z < 1e-6f) {
        return 0;
    }

    std::vector<u32> buckets[kOctantCount];
    for (u32 index : node.body_indices) {
        buckets[octantIndex(positions[index], center)].push_back(index);
    }

    u32 occupied = 0;
    for (u32 octant = 0; octant < kOctantCount; ++octant) {
        if (!buckets[octant].empty()) {
            ++occupied;
        }
    }
    if (occupied <= 1) {
        return 0;
    }

    node.child_start = static_cast<u32>(build_nodes.size());
    node.child_count = 0;

    for (u32 octant = 0; octant < kOctantCount; ++octant) {
        if (buckets[octant].empty()) {
            continue;
        }

        BuildNode child{};
        child.min_bounds = node.min_bounds;
        child.max_bounds = node.max_bounds;
        if (octant & 1) {
            child.min_bounds.x = center.x;
        } else {
            child.max_bounds.x = center.x;
        }
        if (octant & 2) {
            child.min_bounds.y = center.y;
        } else {
            child.max_bounds.y = center.y;
        }
        if (octant & 4) {
            child.min_bounds.z = center.z;
        } else {
            child.max_bounds.z = center.z;
        }
        child.body_indices = std::move(buckets[octant]);
        build_nodes.push_back(child);
        ++node.child_count;
    }

    node.body_indices.clear();
    return node.child_count;
}

void buildTree(const std::vector<vec3>& positions,
               const std::vector<f32>& masses,
               std::vector<BuildNode>& build_nodes,
               std::vector<BHNode>& nodes,
               const BHParams& params) {
    build_nodes.clear();
    nodes.clear();

    if (positions.empty()) {
        return;
    }

    BuildNode root{};
    root.min_bounds = {std::numeric_limits<f32>::max(),
                       std::numeric_limits<f32>::max(),
                       std::numeric_limits<f32>::max()};
    root.max_bounds = {std::numeric_limits<f32>::lowest(),
                       std::numeric_limits<f32>::lowest(),
                       std::numeric_limits<f32>::lowest()};

    for (u32 i = 0; i < positions.size(); ++i) {
        root.body_indices.push_back(i);
        root.min_bounds.x = std::min(root.min_bounds.x, positions[i].x);
        root.min_bounds.y = std::min(root.min_bounds.y, positions[i].y);
        root.min_bounds.z = std::min(root.min_bounds.z, positions[i].z);
        root.max_bounds.x = std::max(root.max_bounds.x, positions[i].x);
        root.max_bounds.y = std::max(root.max_bounds.y, positions[i].y);
        root.max_bounds.z = std::max(root.max_bounds.z, positions[i].z);
    }

    const vec3 padding{0.01f, 0.01f, 0.01f};
    root.min_bounds -= padding;
    root.max_bounds += padding;
    build_nodes.push_back(root);

    bool changed = true;
    u32 depth = 0;
    while (changed && depth < params.max_depth) {
        changed = false;
        for (u32 i = 0; i < build_nodes.size(); ++i) {
            if (build_nodes[i].child_count == 0 &&
                subdivide(build_nodes[i], build_nodes, positions, depth, params) > 0) {
                changed = true;
            }
        }
        ++depth;
    }

    nodes.resize(build_nodes.size());
    for (u32 i = 0; i < build_nodes.size(); ++i) {
        const BuildNode& src = build_nodes[i];
        BHNode& dst = nodes[i];
        dst.min_bounds = src.min_bounds;
        dst.max_bounds = src.max_bounds;
        dst.first_child = src.child_start;
        dst.body_count = static_cast<u32>(src.body_indices.size());
        dst.total_mass = 0.f;
        for (u32 body_index : src.body_indices) {
            dst.total_mass += masses[body_index];
        }
        dst.center_of_mass =
            src.body_indices.empty()
                ? nodeCenter(src)
                : computeCenterOfMass(positions, masses, src.body_indices);
    }

    for (int i = static_cast<int>(nodes.size()) - 1; i >= 0; --i) {
        if (build_nodes[static_cast<u32>(i)].child_count == 0) {
            continue;
        }

        vec3 weighted{};
        f32 total_mass = 0.f;
        const BuildNode& node = build_nodes[static_cast<u32>(i)];
        for (u32 child = 0; child < node.child_count; ++child) {
            const BHNode& child_node = nodes[node.child_start + child];
            weighted += child_node.center_of_mass * child_node.total_mass;
            total_mass += child_node.total_mass;
        }
        if (total_mass > 1e-12f) {
            nodes[static_cast<u32>(i)].center_of_mass = weighted * (1.f / total_mass);
            nodes[static_cast<u32>(i)].total_mass = total_mass;
        }
    }
}

bool containsPoint(const vec3& point, const vec3& min_bounds, const vec3& max_bounds) {
    return point.x >= min_bounds.x && point.x <= max_bounds.x && point.y >= min_bounds.y &&
           point.y <= max_bounds.y && point.z >= min_bounds.z && point.z <= max_bounds.z;
}

void addPairForce(u32 target,
                  u32 source,
                  const std::vector<vec3>& positions,
                  const std::vector<f32>& masses,
                  vec3& out_accel,
                  const BHParams& params) {
    const vec3 diff = positions[source] - positions[target];
    const f32 dist_sq = diff.dot(diff) + params.softening * params.softening;
    const f32 inv_dist = 1.f / std::sqrt(dist_sq);
    const f32 inv_dist_cubed = inv_dist * inv_dist * inv_dist;
    out_accel += diff * (params.gravitational_G * masses[source] * inv_dist_cubed);
}

void computeForceAt(u32 body_index,
                    const std::vector<vec3>& positions,
                    const std::vector<f32>& masses,
                    vec3& out_accel,
                    u32 node_index,
                    const std::vector<BuildNode>& build_nodes,
                    const std::vector<BHNode>& nodes,
                    const BHParams& params) {
    const BHNode& node = nodes[node_index];
    const BuildNode& build_node = build_nodes[node_index];

    if (build_node.child_count == 0) {
        for (u32 other : build_node.body_indices) {
            if (other == body_index) {
                continue;
            }
            addPairForce(body_index, other, positions, masses, out_accel, params);
        }
        return;
    }

    const vec3 diff = node.center_of_mass - positions[body_index];
    const f32 dist_sq = diff.dot(diff) + params.softening * params.softening;
    const vec3 size = node.max_bounds - node.min_bounds;
    const f32 size_max = std::max(size.x, std::max(size.y, size.z));
    const f32 dist = std::sqrt(dist_sq);
    const bool contains_target =
        containsPoint(positions[body_index], node.min_bounds, node.max_bounds);

    if (!contains_target && size_max / dist < params.theta) {
        const f32 inv_dist_cubed = 1.f / (dist * dist_sq);
        out_accel += diff * (params.gravitational_G * node.total_mass * inv_dist_cubed);
        return;
    }

    for (u32 child = 0; child < build_node.child_count; ++child) {
        computeForceAt(body_index,
                       positions,
                       masses,
                       out_accel,
                       build_node.child_start + child,
                       build_nodes,
                       nodes,
                       params);
    }
}

} // namespace

void compute_nbody_forces_naive(const std::vector<vec3>& positions,
                                const std::vector<f32>& masses,
                                std::vector<vec3>& out_accelerations,
                                const BHParams& params) {
    const u32 count = static_cast<u32>(positions.size());
    out_accelerations.assign(count, {});

    for (u32 i = 0; i < count; ++i) {
        for (u32 j = 0; j < count; ++j) {
            if (i == j) {
                continue;
            }
            const vec3 diff = positions[j] - positions[i];
            const f32 dist_sq = diff.dot(diff) + params.softening * params.softening;
            const f32 inv_dist = 1.f / std::sqrt(dist_sq);
            const f32 inv_dist_cubed = inv_dist * inv_dist * inv_dist;
            out_accelerations[i] += diff * (params.gravitational_G * masses[j] * inv_dist_cubed);
        }
    }
}

void BarnesHut::computeForces(const std::vector<vec3>& positions,
                              const std::vector<f32>& masses,
                              std::vector<vec3>& out_accelerations,
                              const BHParams& params) {
    const u32 count = static_cast<u32>(positions.size());
    out_accelerations.assign(count, {});

    if (count == 0) {
        nodes_.clear();
        return;
    }

    std::vector<BuildNode> build_nodes;
    buildTree(positions, masses, build_nodes, nodes_, params);

    if (count <= 2 || nodes_.empty()) {
        compute_nbody_forces_naive(positions, masses, out_accelerations, params);
        return;
    }

    if (params.theta <= 0.f) {
        compute_nbody_forces_naive(positions, masses, out_accelerations, params);
        return;
    }

    for (u32 i = 0; i < count; ++i) {
        computeForceAt(i, positions, masses, out_accelerations[i], 0, build_nodes, nodes_, params);
    }
}

} // namespace fuse::physics
