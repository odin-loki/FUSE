#include <fuse/physics/shapes/convex_hull.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>
#include <utility>

namespace fuse::physics {

namespace {

// ---------------------------------------------------------------------------------------------------
// Double-precision helpers for the builder.

struct D3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double length(D3 a) { return std::sqrt(dot(a, a)); }
D3 toD(vec3 v) { return {v.x, v.y, v.z}; }

struct QhFace {
    s32 v[3]{};
    s32 adj[3]{-1, -1, -1}; ///< face across edge v[i] -> v[(i + 1) % 3]
    D3 normal{};
    double offset = 0.0;
    std::vector<s32> outside;
    bool alive = true;
    u32 visit = 0;
};

class QuickHullBuilder {
public:
    QuickHullBuilder(const vec3* points, u32 count, double tolerance) {
        m_points.resize(count);
        for (u32 i = 0; i < count; ++i) {
            m_points[i] = toD(points[i]);
        }
        m_tol = tolerance;
    }

    bool run(std::string& error) {
        if (!initialSimplex(error)) {
            return false;
        }
        const usize guard = m_points.size() * 8u + 64u;
        usize iterations = 0;
        for (;;) {
            s32 faceIndex = -1;
            for (s32 f = 0; f < static_cast<s32>(m_faces.size()); ++f) {
                if (m_faces[f].alive && !m_faces[f].outside.empty()) {
                    faceIndex = f;
                    break;
                }
            }
            if (faceIndex < 0) {
                return true;
            }
            if (++iterations > guard) {
                error = "quickhull did not converge";
                return false;
            }
            if (!addPoint(faceIndex, error)) {
                return false;
            }
        }
    }

    const std::vector<D3>& points() const { return m_points; }
    const std::vector<QhFace>& faces() const { return m_faces; }
    double tolerance() const { return m_tol; }

private:
    double distance(const QhFace& face, s32 point) const { return dot(face.normal, m_points[point]) - face.offset; }

    bool setPlane(QhFace& face) {
        const D3 a = m_points[face.v[0]];
        const D3 b = m_points[face.v[1]];
        const D3 c = m_points[face.v[2]];
        const D3 n = cross(b - a, c - a);
        const double len = length(n);
        if (len <= 0.0) {
            face.normal = {0.0, 1.0, 0.0};
            face.offset = dot(face.normal, a);
            return false;
        }
        face.normal = n * (1.0 / len);
        face.offset = dot(face.normal, (a + b + c) * (1.0 / 3.0));
        return true;
    }

    bool initialSimplex(std::string& error) {
        const s32 n = static_cast<s32>(m_points.size());
        // Extreme points along the axes; the farthest pair among them seeds the simplex.
        s32 extremes[6] = {0, 0, 0, 0, 0, 0};
        for (s32 i = 1; i < n; ++i) {
            const D3 p = m_points[i];
            if (p.x < m_points[extremes[0]].x) extremes[0] = i;
            if (p.x > m_points[extremes[1]].x) extremes[1] = i;
            if (p.y < m_points[extremes[2]].y) extremes[2] = i;
            if (p.y > m_points[extremes[3]].y) extremes[3] = i;
            if (p.z < m_points[extremes[4]].z) extremes[4] = i;
            if (p.z > m_points[extremes[5]].z) extremes[5] = i;
        }
        s32 i0 = extremes[0];
        s32 i1 = extremes[1];
        double best = -1.0;
        for (u32 a = 0; a < 6u; ++a) {
            for (u32 b = a + 1u; b < 6u; ++b) {
                const double d = length(m_points[extremes[a]] - m_points[extremes[b]]);
                if (d > best) {
                    best = d;
                    i0 = extremes[a];
                    i1 = extremes[b];
                }
            }
        }
        if (best <= m_tol) {
            error = "points are coincident";
            return false;
        }
        const D3 lineDir = (m_points[i1] - m_points[i0]) * (1.0 / best);
        s32 i2 = -1;
        best = m_tol;
        for (s32 i = 0; i < n; ++i) {
            const D3 rel = m_points[i] - m_points[i0];
            const double d = length(rel - lineDir * dot(rel, lineDir));
            if (d > best) {
                best = d;
                i2 = i;
            }
        }
        if (i2 < 0) {
            error = "points are collinear";
            return false;
        }
        D3 planeNormal = cross(m_points[i1] - m_points[i0], m_points[i2] - m_points[i0]);
        planeNormal = planeNormal * (1.0 / length(planeNormal));
        s32 i3 = -1;
        best = m_tol;
        for (s32 i = 0; i < n; ++i) {
            const double d = std::fabs(dot(m_points[i] - m_points[i0], planeNormal));
            if (d > best) {
                best = d;
                i3 = i;
            }
        }
        if (i3 < 0) {
            error = "points are coplanar";
            return false;
        }
        // Orient so that i3 lies below the base triangle's plane.
        if (dot(m_points[i3] - m_points[i0], planeNormal) > 0.0) {
            std::swap(i1, i2);
        }
        const s32 tetra[4][3] = {{i0, i1, i2}, {i0, i3, i1}, {i1, i3, i2}, {i2, i3, i0}};
        for (const auto& tri : tetra) {
            QhFace face{};
            face.v[0] = tri[0];
            face.v[1] = tri[1];
            face.v[2] = tri[2];
            setPlane(face);
            m_faces.push_back(face);
        }
        linkAll();
        // Outside sets: each remaining point goes to the first face it is in front of.
        for (s32 i = 0; i < n; ++i) {
            if (i == i0 || i == i1 || i == i2 || i == i3) {
                continue;
            }
            assign(i, 0, static_cast<s32>(m_faces.size()));
        }
        return true;
    }

    /// Adjacency by matching reversed directed edges (only used for the initial tetrahedron).
    void linkAll() {
        for (s32 f = 0; f < static_cast<s32>(m_faces.size()); ++f) {
            for (s32 e = 0; e < 3; ++e) {
                const s32 a = m_faces[f].v[e];
                const s32 b = m_faces[f].v[(e + 1) % 3];
                for (s32 g = 0; g < static_cast<s32>(m_faces.size()); ++g) {
                    if (g == f) {
                        continue;
                    }
                    for (s32 k = 0; k < 3; ++k) {
                        if (m_faces[g].v[k] == b && m_faces[g].v[(k + 1) % 3] == a) {
                            m_faces[f].adj[e] = g;
                        }
                    }
                }
            }
        }
    }

    void assign(s32 point, s32 firstFace, s32 endFace) {
        s32 bestFace = -1;
        double bestDist = m_tol;
        for (s32 f = firstFace; f < endFace; ++f) {
            if (!m_faces[f].alive) {
                continue;
            }
            const double d = distance(m_faces[f], point);
            if (d > bestDist) {
                bestDist = d;
                bestFace = f;
            }
        }
        if (bestFace >= 0) {
            m_faces[bestFace].outside.push_back(point);
        }
    }

    bool addPoint(s32 faceIndex, std::string& error) {
        // Eye point: the farthest point of the face's outside set.
        const std::vector<s32>& outside = m_faces[faceIndex].outside;
        s32 eye = outside.front();
        double eyeDist = distance(m_faces[faceIndex], eye);
        for (const s32 p : outside) {
            const double d = distance(m_faces[faceIndex], p);
            if (d > eyeDist) {
                eyeDist = d;
                eye = p;
            }
        }

        // Visible faces (flood fill) and the horizon: directed edges of visible faces whose
        // neighbour is not visible.
        ++m_visit;
        m_visible.clear();
        m_horizon.clear();
        m_stack.clear();
        m_stack.push_back(faceIndex);
        m_faces[faceIndex].visit = m_visit;
        while (!m_stack.empty()) {
            const s32 f = m_stack.back();
            m_stack.pop_back();
            m_visible.push_back(f);
            for (s32 e = 0; e < 3; ++e) {
                const s32 g = m_faces[f].adj[e];
                if (g < 0) {
                    error = "hull adjacency broken";
                    return false;
                }
                if (m_faces[g].visit == m_visit) {
                    continue;
                }
                if (distance(m_faces[g], eye) > m_tol) {
                    m_faces[g].visit = m_visit;
                    m_stack.push_back(g);
                }
            }
        }
        for (const s32 f : m_visible) {
            for (s32 e = 0; e < 3; ++e) {
                const s32 g = m_faces[f].adj[e];
                if (m_faces[g].visit != m_visit) {
                    m_horizon.push_back({m_faces[f].v[e], m_faces[f].v[(e + 1) % 3], g});
                }
            }
        }
        if (m_horizon.size() < 3u) {
            error = "degenerate horizon";
            return false;
        }

        // New cone faces (a, b, eye) for each horizon edge a -> b.
        const s32 firstNew = static_cast<s32>(m_faces.size());
        m_startToFace.clear();
        for (const HorizonEdge& edge : m_horizon) {
            QhFace face{};
            face.v[0] = edge.a;
            face.v[1] = edge.b;
            face.v[2] = eye;
            setPlane(face);
            face.adj[0] = edge.across;
            const s32 index = static_cast<s32>(m_faces.size());
            m_faces.push_back(face);
            // Re-point the surviving neighbour's edge b -> a at the new face.
            QhFace& across = m_faces[edge.across];
            for (s32 k = 0; k < 3; ++k) {
                if (across.v[k] == edge.b && across.v[(k + 1) % 3] == edge.a) {
                    across.adj[k] = index;
                }
            }
            if (!m_startToFace.emplace(edge.a, index).second) {
                error = "horizon is not a simple loop";
                return false;
            }
        }
        // Cone adjacency: edge b -> eye of face (a, b) borders the face starting at b; edge
        // eye -> a borders the face ending at a.
        for (s32 f = firstNew; f < static_cast<s32>(m_faces.size()); ++f) {
            const auto next = m_startToFace.find(m_faces[f].v[1]);
            if (next == m_startToFace.end()) {
                error = "horizon is not closed";
                return false;
            }
            m_faces[f].adj[1] = next->second;
            m_faces[next->second].adj[2] = f;
        }

        // Retire the visible faces and hand their outside points to the cone.
        m_orphans.clear();
        for (const s32 f : m_visible) {
            QhFace& face = m_faces[f];
            face.alive = false;
            for (const s32 p : face.outside) {
                if (p != eye) {
                    m_orphans.push_back(p);
                }
            }
            face.outside.clear();
            face.outside.shrink_to_fit();
        }
        for (const s32 p : m_orphans) {
            assign(p, firstNew, static_cast<s32>(m_faces.size()));
        }
        return true;
    }

    struct HorizonEdge {
        s32 a = 0;
        s32 b = 0;
        s32 across = 0;
    };

    std::vector<D3> m_points;
    std::vector<QhFace> m_faces;
    std::vector<s32> m_visible;
    std::vector<s32> m_stack;
    std::vector<HorizonEdge> m_horizon;
    std::vector<s32> m_orphans;
    std::unordered_map<s32, s32> m_startToFace;
    double m_tol = 0.0;
    u32 m_visit = 0;
};

u64 edgeKey(u32 a, u32 b) {
    return (static_cast<u64>(a) << 32u) | b;
}

vec3 toF(D3 v) {
    return {static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z)};
}

/// Merges coplanar triangles into polygons and drops vertices that sit in the middle of an edge.
/// Writes face loops (indices into the builder's point array).
bool mergeFaces(const QuickHullBuilder& builder, std::vector<std::vector<s32>>& loops, std::string& error) {
    const std::vector<QhFace>& faces = builder.faces();
    const std::vector<D3>& points = builder.points();
    std::vector<s32> alive;
    for (s32 f = 0; f < static_cast<s32>(faces.size()); ++f) {
        if (faces[f].alive) {
            alive.push_back(f);
        }
    }
    // Largest triangles seed the groups (their planes are the most accurate).
    const auto area = [&](s32 f) {
        const QhFace& face = faces[f];
        return length(cross(points[face.v[1]] - points[face.v[0]], points[face.v[2]] - points[face.v[0]]));
    };
    std::vector<std::pair<double, s32>> order;
    order.reserve(alive.size());
    for (const s32 f : alive) {
        order.push_back({area(f), f});
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });

    const double planeTol = builder.tolerance() * 2.0;
    std::vector<s32> group(faces.size(), -1);
    std::vector<std::vector<s32>> groups;
    std::vector<s32> stack;
    for (const auto& entry : order) {
        const s32 seed = entry.second;
        if (group[seed] >= 0) {
            continue;
        }
        const s32 id = static_cast<s32>(groups.size());
        groups.emplace_back();
        const D3 n = faces[seed].normal;
        const double offset = faces[seed].offset;
        stack.clear();
        stack.push_back(seed);
        group[seed] = id;
        while (!stack.empty()) {
            const s32 f = stack.back();
            stack.pop_back();
            groups[id].push_back(f);
            for (s32 e = 0; e < 3; ++e) {
                const s32 g = faces[f].adj[e];
                if (g < 0 || !faces[g].alive || group[g] >= 0) {
                    continue;
                }
                if (dot(faces[g].normal, n) < 0.999) {
                    continue;
                }
                bool coplanar = true;
                for (s32 k = 0; k < 3 && coplanar; ++k) {
                    coplanar = std::fabs(dot(points[faces[g].v[k]], n) - offset) <= planeTol;
                }
                if (coplanar) {
                    group[g] = id;
                    stack.push_back(g);
                }
            }
        }
    }

    // Boundary loop of each group: its directed edges whose neighbour lies in another group.
    loops.clear();
    for (const std::vector<s32>& members : groups) {
        std::map<s32, s32> next;
        bool simple = true;
        for (const s32 f : members) {
            for (s32 e = 0; e < 3; ++e) {
                const s32 g = faces[f].adj[e];
                if (g >= 0 && group[g] == group[f]) {
                    continue;
                }
                if (!next.emplace(faces[f].v[e], faces[f].v[(e + 1) % 3]).second) {
                    simple = false;
                }
            }
        }
        std::vector<s32> loop;
        if (simple && !next.empty()) {
            const s32 start = next.begin()->first;
            s32 v = start;
            do {
                loop.push_back(v);
                const auto it = next.find(v);
                if (it == next.end() || loop.size() > next.size()) {
                    simple = false;
                    break;
                }
                v = it->second;
            } while (v != start);
            simple = simple && loop.size() == next.size();
        }
        if (!simple) {
            // Not a single simple polygon (numerical corner case): keep the triangles.
            for (const s32 f : members) {
                loops.push_back({faces[f].v[0], faces[f].v[1], faces[f].v[2]});
            }
            continue;
        }
        loops.push_back(std::move(loop));
    }

    // A vertex that belongs to fewer than three faces lies inside an edge (or a face): drop it.
    for (;;) {
        std::unordered_map<s32, u32> incident;
        for (const std::vector<s32>& loop : loops) {
            for (const s32 v : loop) {
                ++incident[v];
            }
        }
        bool removed = false;
        for (std::vector<s32>& loop : loops) {
            for (usize i = 0; i < loop.size();) {
                if (incident[loop[i]] < 3u && loop.size() > 3u) {
                    loop.erase(loop.begin() + static_cast<std::ptrdiff_t>(i));
                    removed = true;
                } else {
                    ++i;
                }
            }
        }
        if (!removed) {
            break;
        }
    }
    for (const std::vector<s32>& loop : loops) {
        if (loop.size() < 3u) {
            error = "degenerate face after merging";
            return false;
        }
    }
    return true;
}

// Unit box topology (vertex bit 0/1/2 = +x/+y/+z), faces counter-clockwise from outside.
constexpr u32 kBoxFaceLoops[6][4] = {
    {1, 3, 7, 5}, // +X
    {0, 4, 6, 2}, // -X
    {2, 6, 7, 3}, // +Y
    {0, 1, 5, 4}, // -Y
    {4, 5, 7, 6}, // +Z
    {0, 2, 3, 1}, // -Z
};

const ConvexHull& unitBoxHull() {
    static const ConvexHull hull = [] {
        ConvexHull h{};
        makeBoxHull({1.f, 1.f, 1.f}, h);
        return h;
    }();
    return hull;
}

} // namespace

HullView ConvexHull::view() const {
    HullView v{};
    v.vertices = vertices.data();
    v.vertexCount = static_cast<u32>(vertices.size());
    v.faces = faces.data();
    v.faceCount = static_cast<u32>(faces.size());
    v.faceIndices = faceIndices.data();
    v.edges = edges.data();
    v.edgeCount = static_cast<u32>(edges.size());
    v.centroid = centroid;
    return v;
}

bool finalizeConvexHull(ConvexHull& hull, std::string* error) {
    const auto fail = [&](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (hull.vertices.size() < 4u || hull.faces.size() < 4u) {
        return fail("a hull needs at least four vertices and four faces");
    }
    const u32 vertexCount = static_cast<u32>(hull.vertices.size());
    for (HullFace& face : hull.faces) {
        if (face.indexCount < 3u || face.firstIndex + face.indexCount > hull.faceIndices.size()) {
            return fail("face index range out of bounds");
        }
        // Newell normal (robust for slightly non-planar polygons).
        D3 n{};
        for (u32 i = 0; i < face.indexCount; ++i) {
            const u32 a = hull.faceIndices[face.firstIndex + i];
            const u32 b = hull.faceIndices[face.firstIndex + (i + 1u) % face.indexCount];
            if (a >= vertexCount || b >= vertexCount) {
                return fail("face vertex index out of bounds");
            }
            const D3 p = toD(hull.vertices[a]);
            const D3 q = toD(hull.vertices[b]);
            n.x += (p.y - q.y) * (p.z + q.z);
            n.y += (p.z - q.z) * (p.x + q.x);
            n.z += (p.x - q.x) * (p.y + q.y);
        }
        const double len = length(n);
        if (len <= 0.0) {
            return fail("face has zero area");
        }
        n = n * (1.0 / len);
        double offset = -std::numeric_limits<double>::max();
        for (u32 i = 0; i < face.indexCount; ++i) {
            offset = std::max(offset, dot(n, toD(hull.vertices[hull.faceIndices[face.firstIndex + i]])));
        }
        face.normal = toF(n);
        face.offset = static_cast<f32>(offset);
    }

    // Edges: each directed edge must meet its reverse exactly once.
    std::unordered_map<u64, u32> directed;
    directed.reserve(hull.faceIndices.size() * 2u);
    for (u32 f = 0; f < hull.faces.size(); ++f) {
        const HullFace& face = hull.faces[f];
        for (u32 i = 0; i < face.indexCount; ++i) {
            const u32 a = hull.faceIndices[face.firstIndex + i];
            const u32 b = hull.faceIndices[face.firstIndex + (i + 1u) % face.indexCount];
            if (!directed.emplace(edgeKey(a, b), f).second) {
                return fail("directed edge used twice (faces not consistently oriented)");
            }
        }
    }
    hull.edges.clear();
    for (u32 f = 0; f < hull.faces.size(); ++f) {
        const HullFace& face = hull.faces[f];
        for (u32 i = 0; i < face.indexCount; ++i) {
            const u32 a = hull.faceIndices[face.firstIndex + i];
            const u32 b = hull.faceIndices[face.firstIndex + (i + 1u) % face.indexCount];
            const auto twin = directed.find(edgeKey(b, a));
            if (twin == directed.end()) {
                return fail("hull is not closed");
            }
            if (a < b) {
                hull.edges.push_back({a, b, f, twin->second});
            }
        }
    }

    // Bounds.
    hull.boundsMin = hull.vertices[0];
    hull.boundsMax = hull.vertices[0];
    hull.radius = 0.f;
    for (const vec3& v : hull.vertices) {
        hull.boundsMin = {std::min(hull.boundsMin.x, v.x), std::min(hull.boundsMin.y, v.y),
                          std::min(hull.boundsMin.z, v.z)};
        hull.boundsMax = {std::max(hull.boundsMax.x, v.x), std::max(hull.boundsMax.y, v.y),
                          std::max(hull.boundsMax.z, v.z)};
        hull.radius = std::max(hull.radius, v.length());
    }
    hull.halfExtents = {std::max(std::fabs(hull.boundsMin.x), std::fabs(hull.boundsMax.x)),
                        std::max(std::fabs(hull.boundsMin.y), std::fabs(hull.boundsMax.y)),
                        std::max(std::fabs(hull.boundsMin.z), std::fabs(hull.boundsMax.z))};

    // Volume and centroid: tetrahedra from a face-fan to an interior reference point.
    D3 reference{};
    for (const vec3& v : hull.vertices) {
        reference = reference + toD(v);
    }
    reference = reference * (1.0 / static_cast<double>(vertexCount));
    double volume = 0.0;
    D3 moment{};
    for (const HullFace& face : hull.faces) {
        const D3 p0 = toD(hull.vertices[hull.faceIndices[face.firstIndex]]);
        for (u32 i = 1; i + 1u < face.indexCount; ++i) {
            const D3 p1 = toD(hull.vertices[hull.faceIndices[face.firstIndex + i]]);
            const D3 p2 = toD(hull.vertices[hull.faceIndices[face.firstIndex + i + 1u]]);
            const double v = dot(p0 - reference, cross(p1 - reference, p2 - reference)) / 6.0;
            volume += v;
            moment = moment + (reference + p0 + p1 + p2) * (v * 0.25);
        }
    }
    if (volume <= 0.0) {
        return fail("hull has no volume (faces wound inwards?)");
    }
    hull.volume = static_cast<f32>(volume);
    hull.centroid = toF(moment * (1.0 / volume));
    return true;
}

bool buildConvexHull(const vec3* points, u32 count, ConvexHull& out, const QuickHullOptions& options,
                     std::string* error) {
    out = ConvexHull{};
    std::string message;
    const auto fail = [&](const std::string& why) {
        if (error != nullptr) {
            *error = why;
        }
        out = ConvexHull{};
        return false;
    };
    if (points == nullptr || count < 4u) {
        return fail("a hull needs at least four points");
    }
    double extent = 0.0;
    D3 lo = toD(points[0]);
    D3 hi = lo;
    for (u32 i = 0; i < count; ++i) {
        const D3 p = toD(points[i]);
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            return fail("non-finite point");
        }
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    const double tolerance = options.distanceTolerance > 0.f ? static_cast<double>(options.distanceTolerance)
                                                              : std::max(extent * 1e-5, 1e-9);

    QuickHullBuilder builder(points, count, tolerance);
    if (!builder.run(message)) {
        return fail(message);
    }
    std::vector<std::vector<s32>> loops;
    if (!mergeFaces(builder, loops, message)) {
        return fail(message);
    }

    // Compact the vertex set in first-use order (deterministic).
    std::unordered_map<s32, u32> remap;
    for (const std::vector<s32>& loop : loops) {
        HullFace face{};
        face.firstIndex = static_cast<u32>(out.faceIndices.size());
        face.indexCount = static_cast<u32>(loop.size());
        for (const s32 v : loop) {
            const auto inserted = remap.emplace(v, static_cast<u32>(out.vertices.size()));
            if (inserted.second) {
                out.vertices.push_back(toF(builder.points()[v]));
            }
            out.faceIndices.push_back(inserted.first->second);
        }
        out.faces.push_back(face);
    }
    if (!finalizeConvexHull(out, &message)) {
        return fail(message);
    }
    return true;
}

void makeBoxHull(vec3 halfExtents, ConvexHull& out) {
    out = ConvexHull{};
    out.vertices.resize(8);
    for (u32 c = 0; c < 8u; ++c) {
        out.vertices[c] = {(c & 1u) != 0u ? halfExtents.x : -halfExtents.x,
                           (c & 2u) != 0u ? halfExtents.y : -halfExtents.y,
                           (c & 4u) != 0u ? halfExtents.z : -halfExtents.z};
    }
    for (u32 f = 0; f < 6u; ++f) {
        HullFace face{};
        face.firstIndex = f * 4u;
        face.indexCount = 4u;
        out.faces.push_back(face);
        for (u32 k = 0; k < 4u; ++k) {
            out.faceIndices.push_back(kBoxFaceLoops[f][k]);
        }
    }
    finalizeConvexHull(out);
    // Exact axis planes (Newell is exact here too, but keep the offsets bit-identical to the extents).
    const vec3 normals[6] = {{1.f, 0.f, 0.f}, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f},
                             {0.f, -1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f, -1.f}};
    const f32 offsets[6] = {halfExtents.x, halfExtents.x, halfExtents.y, halfExtents.y, halfExtents.z, halfExtents.z};
    for (u32 f = 0; f < 6u; ++f) {
        out.faces[f].normal = normals[f];
        out.faces[f].offset = offsets[f];
    }
    out.centroid = {};
}

HullView BoxHull::view() const {
    HullView v{};
    v.vertices = vertices;
    v.vertexCount = 8u;
    v.faces = faces;
    v.faceCount = 6u;
    v.faceIndices = faceIndices;
    v.edges = edges;
    v.edgeCount = 12u;
    v.centroid = {};
    return v;
}

void initBoxHull(BoxHull& box, vec3 halfExtents) {
    const ConvexHull& unit = unitBoxHull();
    for (u32 c = 0; c < 8u; ++c) {
        box.vertices[c] = {(c & 1u) != 0u ? halfExtents.x : -halfExtents.x,
                           (c & 2u) != 0u ? halfExtents.y : -halfExtents.y,
                           (c & 4u) != 0u ? halfExtents.z : -halfExtents.z};
    }
    const f32 offsets[6] = {halfExtents.x, halfExtents.x, halfExtents.y, halfExtents.y, halfExtents.z, halfExtents.z};
    for (u32 f = 0; f < 6u; ++f) {
        box.faces[f] = unit.faces[f];
        box.faces[f].offset = offsets[f];
    }
    for (u32 i = 0; i < 24u; ++i) {
        box.faceIndices[i] = unit.faceIndices[i];
    }
    for (u32 e = 0; e < 12u; ++e) {
        box.edges[e] = unit.edges[e];
    }
}

f32 hullMaxFaceDistance(const HullView& hull, vec3 p, u32* face) {
    f32 best = -std::numeric_limits<f32>::max();
    u32 bestFace = 0;
    for (u32 f = 0; f < hull.faceCount; ++f) {
        const f32 d = hull.faces[f].normal.dot(p) - hull.faces[f].offset;
        if (d > best) {
            best = d;
            bestFace = f;
        }
    }
    if (face != nullptr) {
        *face = bestFace;
    }
    return best;
}

namespace {

vec3 closestOnSegment(vec3 p, vec3 a, vec3 b) {
    const vec3 ab = b - a;
    const f32 denom = ab.dot(ab);
    if (denom <= 1e-20f) {
        return a;
    }
    const f32 t = std::clamp((p - a).dot(ab) / denom, 0.f, 1.f);
    return a + ab * t;
}

} // namespace

vec3 hullClosestPoint(const HullView& hull, vec3 p) {
    if (hullMaxFaceDistance(hull, p) <= 0.f) {
        return p;
    }
    f32 bestSq = std::numeric_limits<f32>::max();
    vec3 best = p;
    for (u32 f = 0; f < hull.faceCount; ++f) {
        const HullFace& face = hull.faces[f];
        const f32 d = face.normal.dot(p) - face.offset;
        if (d <= 0.f) {
            continue; // the closest point lies on a face the point is in front of
        }
        const vec3 projected = p - face.normal * d;
        bool inside = true;
        for (u32 i = 0; i < face.indexCount && inside; ++i) {
            const vec3 a = hull.vertices[hull.faceIndices[face.firstIndex + i]];
            const vec3 b = hull.vertices[hull.faceIndices[face.firstIndex + (i + 1u) % face.indexCount]];
            const vec3 side = (b - a).cross(face.normal);
            inside = side.dot(projected - a) <= 0.f;
        }
        if (inside) {
            if (d * d < bestSq) {
                bestSq = d * d;
                best = projected;
            }
            continue;
        }
        for (u32 i = 0; i < face.indexCount; ++i) {
            const vec3 a = hull.vertices[hull.faceIndices[face.firstIndex + i]];
            const vec3 b = hull.vertices[hull.faceIndices[face.firstIndex + (i + 1u) % face.indexCount]];
            const vec3 q = closestOnSegment(p, a, b);
            const f32 dsq = (p - q).dot(p - q);
            if (dsq < bestSq) {
                bestSq = dsq;
                best = q;
            }
        }
    }
    return best;
}

vec3 hullSupport(const HullView& hull, vec3 direction) {
    f32 best = -std::numeric_limits<f32>::max();
    vec3 out{};
    for (u32 i = 0; i < hull.vertexCount; ++i) {
        const f32 d = hull.vertices[i].dot(direction);
        if (d > best) {
            best = d;
            out = hull.vertices[i];
        }
    }
    return out;
}

vec3 hullInverseInertiaDiagonal(const ConvexHull& hull, f32 invMass) {
    if (invMass <= 0.f || hull.volume <= 0.f) {
        return {};
    }
    // Second moments of the solid (unit density) from signed tetrahedra (origin, face fan), then
    // shifted to the centroid and scaled to the body's mass.
    const D3 origin{};
    double xx = 0.0;
    double yy = 0.0;
    double zz = 0.0;
    double volume = 0.0;
    for (const HullFace& face : hull.faces) {
        const D3 p0 = toD(hull.vertices[hull.faceIndices[face.firstIndex]]);
        for (u32 i = 1; i + 1u < face.indexCount; ++i) {
            const D3 p1 = toD(hull.vertices[hull.faceIndices[face.firstIndex + i]]);
            const D3 p2 = toD(hull.vertices[hull.faceIndices[face.firstIndex + i + 1u]]);
            const double det = dot(p0 - origin, cross(p1 - origin, p2 - origin));
            volume += det / 6.0;
            // Integral of x^2 over a tetrahedron with a vertex at the origin: det/60 (sum of squares
            // plus cross terms of the three other vertices).
            const auto second = [&](double a, double b, double c) {
                return det / 60.0 * (a * a + b * b + c * c + a * b + b * c + c * a);
            };
            xx += second(p0.x, p1.x, p2.x);
            yy += second(p0.y, p1.y, p2.y);
            zz += second(p0.z, p1.z, p2.z);
        }
    }
    if (volume <= 0.0) {
        return {};
    }
    const D3 c = toD(hull.centroid);
    xx -= volume * c.x * c.x;
    yy -= volume * c.y * c.y;
    zz -= volume * c.z * c.z;
    const double mass = 1.0 / invMass;
    const double scale = mass / volume;
    const double ix = (yy + zz) * scale;
    const double iy = (xx + zz) * scale;
    const double iz = (xx + yy) * scale;
    const auto invert = [](double m) { return m > 1e-12 ? static_cast<f32>(1.0 / m) : 0.f; };
    return {invert(ix), invert(iy), invert(iz)};
}

} // namespace fuse::physics
