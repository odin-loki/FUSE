#pragma once

// Static triangle-mesh collision shape (GAP-PHYS-HULL-MESH part 2): the triangles of a level or prop in
// the body frame, reordered into the leaves of a per-mesh BVH (binned SAH, <= 4 triangles per leaf), with
// per-triangle normals and "real edge" flags for the internal-edge normal fix (an edge shared with a
// coplanar or concave neighbour is not a collision feature: contacts on it take the face normal, so a
// body sliding across the seam between two floor triangles never catches on it).
//
// Built at cook / load time (it allocates); queries (ray cast, AABB overlap) never allocate.

#include <fuse/physics/math.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::physics {

/// 32-byte BVH node. Leaf: `count` > 0 triangles starting at `first`. Interior: `count` == 0, the
/// children are nodes `first` and `first + 1`.
struct TriMeshBvhNode {
    vec3 boundsMin{};
    u32 first = 0;
    vec3 boundsMax{};
    u32 count = 0;
};

/// Per-triangle edge flag bits: edge i (vertex i -> i + 1) is a real (convex or open) edge.
enum TriEdgeFlags : u8 {
    kTriEdge0Real = 1u << 0,
    kTriEdge1Real = 1u << 1,
    kTriEdge2Real = 1u << 2,
};

struct TriMeshRayHit {
    f32 t = 0.f;
    u32 triangle = 0;
    vec3 normal{}; ///< geometric normal facing the ray origin
};

class TriMesh {
public:
    static constexpr u32 kMaxLeafTriangles = 4u;
    static constexpr u32 kMaxDepth = 64u;

    /// Builds the BVH over `indexCount / 3` triangles. Degenerate (zero-area) triangles are dropped.
    /// False (with `error`) for no usable triangle or indices out of range.
    bool build(const vec3* vertices, u32 vertexCount, const u32* indices, u32 indexCount,
               std::string* error = nullptr);
    bool build(const std::vector<vec3>& vertices, const std::vector<u32>& indices, std::string* error = nullptr) {
        return build(vertices.data(), static_cast<u32>(vertices.size()), indices.data(),
                     static_cast<u32>(indices.size()), error);
    }
    /// Adopts cooked data (vertices, BVH-ordered indices, nodes, edge flags) after validating it.
    bool assign(std::vector<vec3> vertices, std::vector<u32> indices, std::vector<TriMeshBvhNode> nodes,
                std::vector<u8> edgeFlags, std::string* error = nullptr);

    [[nodiscard]] u32 triangleCount() const { return static_cast<u32>(m_indices.size() / 3u); }
    [[nodiscard]] bool empty() const { return m_indices.empty(); }
    void triangle(u32 t, vec3& a, vec3& b, vec3& c) const {
        a = m_vertices[m_indices[t * 3u]];
        b = m_vertices[m_indices[t * 3u + 1u]];
        c = m_vertices[m_indices[t * 3u + 2u]];
    }
    [[nodiscard]] vec3 triangleNormal(u32 t) const { return m_normals[t]; }
    [[nodiscard]] u8 edgeFlags(u32 t) const { return m_edgeFlags[t]; }

    [[nodiscard]] const std::vector<vec3>& vertices() const { return m_vertices; }
    /// Triangle indices in BVH leaf order.
    [[nodiscard]] const std::vector<u32>& indices() const { return m_indices; }
    [[nodiscard]] const std::vector<TriMeshBvhNode>& nodes() const { return m_nodes; }
    [[nodiscard]] const std::vector<u8>& allEdgeFlags() const { return m_edgeFlags; }

    [[nodiscard]] vec3 boundsMin() const { return m_boundsMin; }
    [[nodiscard]] vec3 boundsMax() const { return m_boundsMax; }
    /// Largest |coordinate| per axis (the value CollisionShapeSoA::params holds for a mesh).
    [[nodiscard]] vec3 halfExtents() const { return m_halfExtents; }
    [[nodiscard]] u32 maxDepth() const { return m_depth; }

    /// Nearest triangle hit along the ray (both faces), t in [0, maxT]; ties go to the lower triangle.
    bool rayCast(vec3 origin, vec3 direction, f32 maxT, TriMeshRayHit& hit) const;
    /// Same answer by testing every triangle (reference for the BVH gates).
    bool rayCastBruteForce(vec3 origin, vec3 direction, f32 maxT, TriMeshRayHit& hit) const;

    /// Calls `visit(triangle)` for every triangle whose bounds overlap [lo, hi] (body frame).
    template <typename Visit>
    void queryAabb(vec3 lo, vec3 hi, Visit&& visit) const;

private:
    void computeNormalsAndEdges();
    void computeBounds();

    std::vector<vec3> m_vertices;
    std::vector<u32> m_indices;
    std::vector<vec3> m_normals;
    std::vector<u8> m_edgeFlags;
    std::vector<TriMeshBvhNode> m_nodes;
    vec3 m_boundsMin{};
    vec3 m_boundsMax{};
    vec3 m_halfExtents{};
    u32 m_depth = 0;
};

/// Möller-Trumbore, both faces. True with `t` in [0, maxT].
bool rayTriangle(vec3 origin, vec3 direction, vec3 a, vec3 b, vec3 c, f32 maxT, f32& t);

template <typename Visit>
void TriMesh::queryAabb(vec3 lo, vec3 hi, Visit&& visit) const {
    if (m_nodes.empty()) {
        return;
    }
    u32 stack[kMaxDepth + 2u];
    u32 top = 0;
    stack[top++] = 0u;
    while (top > 0u) {
        const TriMeshBvhNode& node = m_nodes[stack[--top]];
        if (node.boundsMin.x > hi.x || node.boundsMax.x < lo.x || node.boundsMin.y > hi.y ||
            node.boundsMax.y < lo.y || node.boundsMin.z > hi.z || node.boundsMax.z < lo.z) {
            continue;
        }
        if (node.count > 0u) {
            for (u32 t = node.first; t < node.first + node.count; ++t) {
                const vec3 a = m_vertices[m_indices[t * 3u]];
                const vec3 b = m_vertices[m_indices[t * 3u + 1u]];
                const vec3 c = m_vertices[m_indices[t * 3u + 2u]];
                const f32 minX = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
                const f32 maxX = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
                const f32 minY = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
                const f32 maxY = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
                const f32 minZ = a.z < b.z ? (a.z < c.z ? a.z : c.z) : (b.z < c.z ? b.z : c.z);
                const f32 maxZ = a.z > b.z ? (a.z > c.z ? a.z : c.z) : (b.z > c.z ? b.z : c.z);
                if (minX > hi.x || maxX < lo.x || minY > hi.y || maxY < lo.y || minZ > hi.z || maxZ < lo.z) {
                    continue;
                }
                visit(t);
            }
            continue;
        }
        if (top + 2u <= kMaxDepth + 2u) {
            stack[top++] = node.first + 1u;
            stack[top++] = node.first;
        }
    }
}

} // namespace fuse::physics
