#include <fuse/physics/shapes/tri_mesh.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace fuse::physics {

namespace {

vec3 vmin(vec3 a, vec3 b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

vec3 vmax(vec3 a, vec3 b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

f32 axisOf(vec3 v, u32 axis) {
    return axis == 0u ? v.x : (axis == 1u ? v.y : v.z);
}

f32 surfaceArea(vec3 lo, vec3 hi) {
    const vec3 d = hi - lo;
    if (d.x < 0.f || d.y < 0.f || d.z < 0.f) {
        return 0.f;
    }
    return 2.f * (d.x * d.y + d.y * d.z + d.z * d.x);
}

struct BuildTri {
    vec3 lo{};
    vec3 hi{};
    vec3 centre{};
    u32 source = 0;
};

class BvhBuilder {
public:
    BvhBuilder(std::vector<BuildTri>& tris, std::vector<TriMeshBvhNode>& nodes) : m_tris(tris), m_nodes(nodes) {}

    u32 run() {
        m_nodes.clear();
        m_nodes.reserve(m_tris.size() * 2u);
        m_nodes.push_back({});
        return split(0u, 0u, static_cast<u32>(m_tris.size()), 1u);
    }

private:
    u32 split(u32 nodeIndex, u32 first, u32 count, u32 depth) {
        vec3 lo = m_tris[first].lo;
        vec3 hi = m_tris[first].hi;
        vec3 clo = m_tris[first].centre;
        vec3 chi = clo;
        for (u32 i = first; i < first + count; ++i) {
            lo = vmin(lo, m_tris[i].lo);
            hi = vmax(hi, m_tris[i].hi);
            clo = vmin(clo, m_tris[i].centre);
            chi = vmax(chi, m_tris[i].centre);
        }
        m_nodes[nodeIndex].boundsMin = lo;
        m_nodes[nodeIndex].boundsMax = hi;
        if (count <= TriMesh::kMaxLeafTriangles || depth >= TriMesh::kMaxDepth) {
            m_nodes[nodeIndex].first = first;
            m_nodes[nodeIndex].count = count;
            return depth;
        }

        // Binned SAH over centroids (12 bins per axis); falls back to a median split.
        constexpr u32 kBins = 12u;
        f32 bestCost = std::numeric_limits<f32>::max();
        u32 bestAxis = 0u;
        u32 bestBin = 0u;
        for (u32 axis = 0; axis < 3u; ++axis) {
            const f32 cmin = axisOf(clo, axis);
            const f32 cmax = axisOf(chi, axis);
            if (cmax - cmin <= 1e-12f) {
                continue;
            }
            const f32 scale = static_cast<f32>(kBins) / (cmax - cmin);
            vec3 binLo[kBins];
            vec3 binHi[kBins];
            u32 binCount[kBins]{};
            for (u32 b = 0; b < kBins; ++b) {
                binLo[b] = {std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(),
                            std::numeric_limits<f32>::max()};
                binHi[b] = binLo[b] * -1.f;
            }
            for (u32 i = first; i < first + count; ++i) {
                const u32 b = std::min(kBins - 1u, static_cast<u32>((axisOf(m_tris[i].centre, axis) - cmin) * scale));
                binLo[b] = vmin(binLo[b], m_tris[i].lo);
                binHi[b] = vmax(binHi[b], m_tris[i].hi);
                ++binCount[b];
            }
            // Sweep: cost of splitting after bin b.
            f32 leftArea[kBins]{};
            u32 leftCount[kBins]{};
            vec3 accLo = binLo[0];
            vec3 accHi = binHi[0];
            u32 acc = 0;
            for (u32 b = 0; b < kBins; ++b) {
                if (binCount[b] > 0u) {
                    accLo = acc == 0u ? binLo[b] : vmin(accLo, binLo[b]);
                    accHi = acc == 0u ? binHi[b] : vmax(accHi, binHi[b]);
                }
                acc += binCount[b];
                leftCount[b] = acc;
                leftArea[b] = acc > 0u ? surfaceArea(accLo, accHi) : 0.f;
            }
            acc = 0;
            for (u32 b = kBins - 1u; b > 0u; --b) {
                if (binCount[b] > 0u) {
                    accLo = acc == 0u ? binLo[b] : vmin(accLo, binLo[b]);
                    accHi = acc == 0u ? binHi[b] : vmax(accHi, binHi[b]);
                }
                acc += binCount[b];
                const u32 left = leftCount[b - 1u];
                if (left == 0u || acc == 0u) {
                    continue;
                }
                const f32 cost = leftArea[b - 1u] * static_cast<f32>(left) + surfaceArea(accLo, accHi) * static_cast<f32>(acc);
                if (cost < bestCost) {
                    bestCost = cost;
                    bestAxis = axis;
                    bestBin = b;
                }
            }
        }

        u32 mid = first;
        if (bestCost < std::numeric_limits<f32>::max()) {
            const f32 cmin = axisOf(clo, bestAxis);
            const f32 scale = static_cast<f32>(kBins) / (axisOf(chi, bestAxis) - cmin);
            const auto begin = m_tris.begin() + first;
            const auto end = begin + count;
            const auto pivot = std::stable_partition(begin, end, [&](const BuildTri& t) {
                return std::min(kBins - 1u, static_cast<u32>((axisOf(t.centre, bestAxis) - cmin) * scale)) < bestBin;
            });
            mid = static_cast<u32>(pivot - m_tris.begin());
        }
        if (mid == first || mid == first + count) {
            // All centroids in one bin: split at the median of the widest centroid axis.
            const vec3 extent = chi - clo;
            const u32 axis = extent.x >= extent.y && extent.x >= extent.z ? 0u : (extent.y >= extent.z ? 1u : 2u);
            const auto begin = m_tris.begin() + first;
            std::stable_sort(begin, begin + count, [&](const BuildTri& a, const BuildTri& b) {
                return axisOf(a.centre, axis) < axisOf(b.centre, axis);
            });
            mid = first + count / 2u;
        }

        const u32 left = static_cast<u32>(m_nodes.size());
        m_nodes.push_back({});
        m_nodes.push_back({});
        m_nodes[nodeIndex].first = left;
        m_nodes[nodeIndex].count = 0u;
        const u32 depthLeft = split(left, first, mid - first, depth + 1u);
        const u32 depthRight = split(left + 1u, mid, first + count - mid, depth + 1u);
        return std::max(depthLeft, depthRight);
    }

    std::vector<BuildTri>& m_tris;
    std::vector<TriMeshBvhNode>& m_nodes;
};

u64 edgeKey(u32 a, u32 b) {
    return (static_cast<u64>(std::min(a, b)) << 32u) | std::max(a, b);
}

bool rayAabb(vec3 origin, vec3 invDir, vec3 lo, vec3 hi, f32 maxT) {
    f32 tMin = 0.f;
    f32 tMax = maxT;
    const f32 o[3] = {origin.x, origin.y, origin.z};
    const f32 inv[3] = {invDir.x, invDir.y, invDir.z};
    const f32 mn[3] = {lo.x, lo.y, lo.z};
    const f32 mx[3] = {hi.x, hi.y, hi.z};
    for (u32 a = 0; a < 3u; ++a) {
        f32 t0 = (mn[a] - o[a]) * inv[a];
        f32 t1 = (mx[a] - o[a]) * inv[a];
        if (std::isnan(t0) || std::isnan(t1)) {
            // Ray parallel to the slab and on its boundary plane: inside iff within the slab.
            if (o[a] < mn[a] || o[a] > mx[a]) {
                return false;
            }
            continue;
        }
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) {
            return false;
        }
    }
    return true;
}

} // namespace

bool rayTriangle(vec3 origin, vec3 direction, vec3 a, vec3 b, vec3 c, f32 maxT, f32& t) {
    const vec3 e1 = b - a;
    const vec3 e2 = c - a;
    const vec3 p = direction.cross(e2);
    const f32 det = e1.dot(p);
    if (std::fabs(det) < 1e-12f) {
        return false;
    }
    const f32 invDet = 1.f / det;
    const vec3 s = origin - a;
    const f32 u = s.dot(p) * invDet;
    if (u < 0.f || u > 1.f) {
        return false;
    }
    const vec3 q = s.cross(e1);
    const f32 v = direction.dot(q) * invDet;
    if (v < 0.f || u + v > 1.f) {
        return false;
    }
    const f32 hit = e2.dot(q) * invDet;
    if (hit < 0.f || hit > maxT) {
        return false;
    }
    t = hit;
    return true;
}

bool TriMesh::build(const vec3* vertices, u32 vertexCount, const u32* indices, u32 indexCount, std::string* error) {
    const auto fail = [&](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        *this = TriMesh{};
        return false;
    };
    *this = TriMesh{};
    if (vertices == nullptr || indices == nullptr || indexCount < 3u) {
        return fail("a triangle mesh needs at least one triangle");
    }
    m_vertices.assign(vertices, vertices + vertexCount);
    std::vector<BuildTri> tris;
    tris.reserve(indexCount / 3u);
    for (u32 t = 0; t + 2u < indexCount; t += 3u) {
        const u32 i0 = indices[t];
        const u32 i1 = indices[t + 1u];
        const u32 i2 = indices[t + 2u];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
            return fail("triangle index out of range");
        }
        const vec3 a = vertices[i0];
        const vec3 b = vertices[i1];
        const vec3 c = vertices[i2];
        if ((b - a).cross(c - a).length() <= 1e-12f) {
            continue; // zero area: never a collision feature
        }
        BuildTri tri{};
        tri.lo = vmin(a, vmin(b, c));
        tri.hi = vmax(a, vmax(b, c));
        tri.centre = (a + b + c) * (1.f / 3.f);
        tri.source = t / 3u;
        tris.push_back(tri);
    }
    if (tris.empty()) {
        return fail("the mesh has no triangle with area");
    }
    BvhBuilder builder(tris, m_nodes);
    m_depth = builder.run();
    m_indices.resize(tris.size() * 3u);
    for (u32 i = 0; i < tris.size(); ++i) {
        const u32 source = tris[i].source * 3u;
        m_indices[i * 3u] = indices[source];
        m_indices[i * 3u + 1u] = indices[source + 1u];
        m_indices[i * 3u + 2u] = indices[source + 2u];
    }
    computeNormalsAndEdges();
    computeBounds();
    return true;
}

bool TriMesh::assign(std::vector<vec3> vertices, std::vector<u32> indices, std::vector<TriMeshBvhNode> nodes,
                     std::vector<u8> edgeFlags, std::string* error) {
    const auto fail = [&](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        *this = TriMesh{};
        return false;
    };
    *this = TriMesh{};
    const u32 triangles = static_cast<u32>(indices.size() / 3u);
    if (indices.empty() || indices.size() % 3u != 0u || nodes.empty() || edgeFlags.size() != triangles) {
        return fail("inconsistent mesh arrays");
    }
    for (const u32 index : indices) {
        if (index >= vertices.size()) {
            return fail("triangle index out of range");
        }
    }
    // Every triangle in exactly one leaf; interior children in range; depth bounded.
    std::vector<u8> covered(triangles, 0u);
    std::vector<std::pair<u32, u32>> stack{{0u, 1u}};
    u32 depth = 0;
    while (!stack.empty()) {
        const auto [index, level] = stack.back();
        stack.pop_back();
        if (index >= nodes.size() || level > kMaxDepth) {
            return fail("BVH node out of range or too deep");
        }
        depth = std::max(depth, level);
        const TriMeshBvhNode& node = nodes[index];
        if (node.count > 0u) {
            if (node.first + node.count > triangles) {
                return fail("BVH leaf out of range");
            }
            for (u32 t = node.first; t < node.first + node.count; ++t) {
                if (covered[t] != 0u) {
                    return fail("triangle referenced by two leaves");
                }
                covered[t] = 1u;
            }
        } else {
            if (node.first == 0u || node.first + 1u >= nodes.size()) {
                return fail("BVH child out of range");
            }
            stack.push_back({node.first + 1u, level + 1u});
            stack.push_back({node.first, level + 1u});
        }
    }
    if (std::find(covered.begin(), covered.end(), u8{0}) != covered.end()) {
        return fail("triangle not referenced by the BVH");
    }
    m_vertices = std::move(vertices);
    m_indices = std::move(indices);
    m_nodes = std::move(nodes);
    m_depth = depth;
    computeNormalsAndEdges();
    m_edgeFlags = std::move(edgeFlags);
    computeBounds();
    return true;
}

void TriMesh::computeNormalsAndEdges() {
    const u32 triangles = triangleCount();
    m_normals.resize(triangles);
    for (u32 t = 0; t < triangles; ++t) {
        vec3 a;
        vec3 b;
        vec3 c;
        triangle(t, a, b, c);
        m_normals[t] = (b - a).cross(c - a).normalized();
    }
    // Real edges: open (one triangle), non-manifold (3+), or convex (the neighbour's far vertex lies
    // below this triangle's plane). Coplanar and concave shared edges are internal.
    std::unordered_map<u64, std::vector<u32>> byEdge;
    byEdge.reserve(triangles * 3u);
    for (u32 t = 0; t < triangles; ++t) {
        for (u32 e = 0; e < 3u; ++e) {
            byEdge[edgeKey(m_indices[t * 3u + e], m_indices[t * 3u + (e + 1u) % 3u])].push_back(t * 3u + e);
        }
    }
    m_edgeFlags.assign(triangles, 0u);
    for (u32 t = 0; t < triangles; ++t) {
        vec3 a;
        vec3 b;
        vec3 c;
        triangle(t, a, b, c);
        const f32 scale = std::max({(b - a).length(), (c - b).length(), (a - c).length()});
        for (u32 e = 0; e < 3u; ++e) {
            const u32 i0 = m_indices[t * 3u + e];
            const u32 i1 = m_indices[t * 3u + (e + 1u) % 3u];
            const std::vector<u32>& users = byEdge[edgeKey(i0, i1)];
            bool real = users.size() != 2u;
            if (!real) {
                const u32 other = users[0] / 3u == t ? users[1] : users[0];
                const u32 otherTri = other / 3u;
                const u32 otherEdge = other % 3u;
                const vec3 opposite = m_vertices[m_indices[otherTri * 3u + (otherEdge + 2u) % 3u]];
                const f32 height = m_normals[t].dot(opposite - m_vertices[i0]);
                real = height < -1e-4f * scale; // convex fold
            }
            if (real) {
                m_edgeFlags[t] = static_cast<u8>(m_edgeFlags[t] | (1u << e));
            }
        }
    }
}

void TriMesh::computeBounds() {
    m_boundsMin = m_nodes.empty() ? vec3{} : m_nodes[0].boundsMin;
    m_boundsMax = m_nodes.empty() ? vec3{} : m_nodes[0].boundsMax;
    m_halfExtents = {std::max(std::fabs(m_boundsMin.x), std::fabs(m_boundsMax.x)),
                     std::max(std::fabs(m_boundsMin.y), std::fabs(m_boundsMax.y)),
                     std::max(std::fabs(m_boundsMin.z), std::fabs(m_boundsMax.z))};
}

bool TriMesh::rayCast(vec3 origin, vec3 direction, f32 maxT, TriMeshRayHit& hit) const {
    if (m_nodes.empty()) {
        return false;
    }
    const vec3 invDir{1.f / direction.x, 1.f / direction.y, 1.f / direction.z};
    f32 best = maxT;
    u32 bestTri = ~0u;
    u32 stack[kMaxDepth + 2u];
    u32 top = 0;
    stack[top++] = 0u;
    while (top > 0u) {
        const TriMeshBvhNode& node = m_nodes[stack[--top]];
        if (!rayAabb(origin, invDir, node.boundsMin, node.boundsMax, best)) {
            continue;
        }
        if (node.count > 0u) {
            for (u32 t = node.first; t < node.first + node.count; ++t) {
                vec3 a;
                vec3 b;
                vec3 c;
                triangle(t, a, b, c);
                f32 tHit = 0.f;
                if (rayTriangle(origin, direction, a, b, c, best, tHit) &&
                    (tHit < best || (tHit == best && t < bestTri))) {
                    best = tHit;
                    bestTri = t;
                }
            }
            continue;
        }
        stack[top++] = node.first + 1u;
        stack[top++] = node.first;
    }
    if (bestTri == ~0u) {
        return false;
    }
    hit.t = best;
    hit.triangle = bestTri;
    const vec3 n = m_normals[bestTri];
    hit.normal = n.dot(direction) > 0.f ? n * -1.f : n;
    return true;
}

bool TriMesh::rayCastBruteForce(vec3 origin, vec3 direction, f32 maxT, TriMeshRayHit& hit) const {
    f32 best = maxT;
    u32 bestTri = ~0u;
    for (u32 t = 0; t < triangleCount(); ++t) {
        vec3 a;
        vec3 b;
        vec3 c;
        triangle(t, a, b, c);
        f32 tHit = 0.f;
        if (rayTriangle(origin, direction, a, b, c, best, tHit) && (tHit < best || (tHit == best && t < bestTri))) {
            best = tHit;
            bestTri = t;
        }
    }
    if (bestTri == ~0u) {
        return false;
    }
    hit.t = best;
    hit.triangle = bestTri;
    const vec3 n = m_normals[bestTri];
    hit.normal = n.dot(direction) > 0.f ? n * -1.f : n;
    return true;
}

} // namespace fuse::physics
