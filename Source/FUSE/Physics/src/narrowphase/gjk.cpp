#include <fuse/physics/narrowphase/gjk.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

namespace fuse::physics::narrowphase {

namespace {

constexpr u32 kGjkMaxIterations = 64u;
constexpr f32 kGjkTouchSq = 1e-12f;
constexpr f32 kGjkRelTolerance = 1e-6f;
constexpr u32 kEpaMaxIterations = 128u;
constexpr f32 kEpaTolerance = 1e-5f;

vec3 neg(vec3 v) {
    return {-v.x, -v.y, -v.z};
}

CsoPoint csoSupport(const vec3* hullA, u32 countA, const vec3* hullB, u32 countB, vec3 direction) {
    CsoPoint p{};
    p.a = support(hullA, countA, direction);
    p.b = support(hullB, countB, neg(direction));
    p.v = p.a - p.b;
    return p;
}

vec3 centroid(const vec3* hull, u32 count) {
    vec3 sum{};
    for (u32 i = 0; i < count; ++i) {
        sum += hull[i];
    }
    return sum * (1.f / static_cast<f32>(count));
}

bool nearZero(vec3 v) {
    return v.dot(v) < 1e-20f;
}

/// Closest point to the origin on the simplex; reduces the simplex to the vertices of the
/// supporting feature (Ericson, RTCD 5.1). Returns false when the origin is inside a tetrahedron.
bool closestOnSimplex(GjkSimplex& s, vec3& closest) {
    const auto keep = [&s](std::initializer_list<u32> indices) {
        CsoPoint kept[4];
        u32 n = 0;
        for (const u32 i : indices) {
            kept[n++] = s.points[i];
        }
        for (u32 i = 0; i < n; ++i) {
            s.points[i] = kept[i];
        }
        s.count = n;
    };
    switch (s.count) {
    case 1:
        closest = s.points[0].v;
        return true;
    case 2: {
        const vec3 a = s.points[0].v;
        const vec3 ab = s.points[1].v - a;
        const f32 denom = ab.dot(ab);
        const f32 t = denom > 1e-20f ? std::clamp(-a.dot(ab) / denom, 0.f, 1.f) : 0.f;
        if (t <= 0.f) {
            keep({0});
        } else if (t >= 1.f) {
            keep({1});
        }
        closest = a + ab * t;
        return true;
    }
    case 3: {
        const vec3 a = s.points[0].v;
        const vec3 b = s.points[1].v;
        const vec3 c = s.points[2].v;
        const vec3 ab = b - a;
        const vec3 ac = c - a;
        const vec3 ap = neg(a);
        const f32 d1 = ab.dot(ap);
        const f32 d2 = ac.dot(ap);
        if (d1 <= 0.f && d2 <= 0.f) {
            keep({0});
            closest = a;
            return true;
        }
        const vec3 bp = neg(b);
        const f32 d3 = ab.dot(bp);
        const f32 d4 = ac.dot(bp);
        if (d3 >= 0.f && d4 <= d3) {
            keep({1});
            closest = b;
            return true;
        }
        const f32 vc = d1 * d4 - d3 * d2;
        if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) {
            const f32 v = d1 / (d1 - d3);
            keep({0, 1});
            closest = a + ab * v;
            return true;
        }
        const vec3 cp = neg(c);
        const f32 d5 = ab.dot(cp);
        const f32 d6 = ac.dot(cp);
        if (d6 >= 0.f && d5 <= d6) {
            keep({2});
            closest = c;
            return true;
        }
        const f32 vb = d5 * d2 - d1 * d6;
        if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) {
            const f32 w = d2 / (d2 - d6);
            keep({0, 2});
            closest = a + ac * w;
            return true;
        }
        const f32 va = d3 * d6 - d5 * d4;
        if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) {
            const f32 w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            keep({1, 2});
            closest = b + (c - b) * w;
            return true;
        }
        const f32 denom = 1.f / (va + vb + vc);
        closest = a + ab * (vb * denom) + ac * (vc * denom);
        return true;
    }
    case 4: {
        // Origin outside a face (w.r.t. the opposite vertex) => closest point is on such a face.
        const u32 faces[4][4] = {{0, 1, 2, 3}, {0, 2, 3, 1}, {0, 3, 1, 2}, {1, 3, 2, 0}};
        f32 bestDistSq = 1e30f;
        GjkSimplex best{};
        vec3 bestPoint{};
        bool outsideAny = false;
        for (const auto& f : faces) {
            const vec3 a = s.points[f[0]].v;
            const vec3 n = (s.points[f[1]].v - a).cross(s.points[f[2]].v - a);
            const f32 originSide = n.dot(neg(a));
            const f32 opposite = n.dot(s.points[f[3]].v - a);
            if (originSide * opposite >= 0.f && std::fabs(opposite) > 1e-20f) {
                continue; // origin on the same side as the opposite vertex
            }
            outsideAny = true;
            GjkSimplex tri{};
            tri.points[0] = s.points[f[0]];
            tri.points[1] = s.points[f[1]];
            tri.points[2] = s.points[f[2]];
            tri.count = 3;
            vec3 q{};
            closestOnSimplex(tri, q);
            const f32 distSq = q.dot(q);
            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                best = tri;
                bestPoint = q;
            }
        }
        if (!outsideAny) {
            return false; // origin enclosed
        }
        s = best;
        closest = bestPoint;
        return true;
    }
    default:
        return true;
    }
}

struct EpaFace {
    u32 a = 0;
    u32 b = 0;
    u32 c = 0;
    vec3 normal{};
    f32 distance = 0.f;
};

/// Outward face from `interior`; false for a degenerate (zero-area) face.
bool makeFace(const std::vector<CsoPoint>& verts, u32 a, u32 b, u32 c, vec3 interior, EpaFace& face) {
    vec3 n = (verts[b].v - verts[a].v).cross(verts[c].v - verts[a].v);
    const f32 len = n.length();
    if (len < 1e-12f) {
        return false;
    }
    n = n * (1.f / len);
    if (n.dot(verts[a].v - interior) < 0.f) {
        std::swap(b, c);
        n = neg(n);
    }
    face = {a, b, c, n, n.dot(verts[a].v)};
    return true;
}

/// Grows a GJK simplex that enclosed the origin into a non-degenerate tetrahedron.
bool completeTetrahedron(const vec3* hullA, u32 countA, const vec3* hullB, u32 countB, GjkSimplex& s) {
    static const vec3 kDirections[6] = {{1.f, 0.f, 0.f}, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f},
                                        {0.f, -1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f, -1.f}};
    auto addIfNew = [&](const CsoPoint& p) {
        for (u32 i = 0; i < s.count; ++i) {
            if (nearZero(s.points[i].v - p.v)) {
                return false;
            }
        }
        s.points[s.count++] = p;
        return true;
    };
    if (s.count == 0u) {
        s.points[s.count++] = csoSupport(hullA, countA, hullB, countB, {1.f, 0.f, 0.f});
    }
    if (s.count == 1u) {
        for (const vec3& dir : kDirections) {
            if (addIfNew(csoSupport(hullA, countA, hullB, countB, dir))) {
                break;
            }
        }
    }
    if (s.count == 2u) {
        const vec3 line = s.points[1].v - s.points[0].v;
        for (const vec3& axis : kDirections) {
            const vec3 dir = line.cross(axis);
            if (nearZero(dir)) {
                continue;
            }
            const CsoPoint p = csoSupport(hullA, countA, hullB, countB, dir);
            if (!nearZero(line.cross(p.v - s.points[0].v)) && addIfNew(p)) {
                break;
            }
            const CsoPoint q = csoSupport(hullA, countA, hullB, countB, neg(dir));
            if (!nearZero(line.cross(q.v - s.points[0].v)) && addIfNew(q)) {
                break;
            }
        }
    }
    if (s.count == 3u) {
        const vec3 n = (s.points[1].v - s.points[0].v).cross(s.points[2].v - s.points[0].v);
        if (nearZero(n)) {
            return false;
        }
        const CsoPoint p = csoSupport(hullA, countA, hullB, countB, n);
        const CsoPoint q = csoSupport(hullA, countA, hullB, countB, neg(n));
        const f32 dp = std::fabs(n.dot(p.v - s.points[0].v));
        const f32 dq = std::fabs(n.dot(q.v - s.points[0].v));
        const CsoPoint& best = dp >= dq ? p : q;
        if (std::max(dp, dq) < 1e-9f * std::max(1.f, n.length())) {
            return false; // flat Minkowski difference
        }
        s.points[s.count++] = best;
    }
    if (s.count != 4u) {
        return false;
    }
    const vec3 volume = (s.points[1].v - s.points[0].v).cross(s.points[2].v - s.points[0].v);
    return std::fabs(volume.dot(s.points[3].v - s.points[0].v)) > 1e-12f;
}

} // namespace

bool gjkIntersect(const vec3* hullA, u32 countA, const vec3* hullB, u32 countB, GjkSimplex* simplexOut,
                  u32* iterationsOut) {
    GjkSimplex local{};
    GjkSimplex& s = simplexOut != nullptr ? *simplexOut : local;
    s.count = 0;
    if (iterationsOut != nullptr) {
        *iterationsOut = 0;
    }
    if (countA == 0 || countB == 0) {
        return false;
    }

    // Distance GJK: v is the closest point of the current simplex to the origin.
    vec3 d = centroid(hullA, countA) - centroid(hullB, countB);
    if (nearZero(d)) {
        d = {1.f, 0.f, 0.f};
    }
    s.points[0] = csoSupport(hullA, countA, hullB, countB, d);
    s.count = 1;
    vec3 v = s.points[0].v;

    for (u32 iteration = 1; iteration <= kGjkMaxIterations; ++iteration) {
        if (iterationsOut != nullptr) {
            *iterationsOut = iteration;
        }
        const f32 vv = v.dot(v);
        if (vv < kGjkTouchSq) {
            return true; // origin on the simplex: touching or overlapping
        }
        const CsoPoint w = csoSupport(hullA, countA, hullB, countB, neg(v));
        if (w.v.dot(v) > 0.f) {
            return false; // -v is a separating axis
        }
        if (vv - v.dot(w.v) <= kGjkRelTolerance * vv) {
            return false; // converged on a positive distance
        }
        s.points[s.count++] = w;
        if (!closestOnSimplex(s, v)) {
            return true; // origin inside the tetrahedron
        }
    }
    return v.dot(v) < kGjkTouchSq;
}

bool gjkIntersect(const vec3* hullA, u32 countA, const vec3* hullB, u32 countB) {
    return gjkIntersect(hullA, countA, hullB, countB, nullptr, nullptr);
}

EpaResult epaPenetration(const vec3* hullA, u32 countA, const vec3* hullB, u32 countB) {
    EpaResult result{};
    GjkSimplex simplex{};
    result.intersecting = gjkIntersect(hullA, countA, hullB, countB, &simplex, nullptr);
    if (!result.intersecting) {
        return result;
    }
    if (!completeTetrahedron(hullA, countA, hullB, countB, simplex)) {
        return result; // flat Minkowski difference: touching with zero depth
    }

    std::vector<CsoPoint> verts(simplex.points, simplex.points + 4);
    const vec3 interior = (verts[0].v + verts[1].v + verts[2].v + verts[3].v) * 0.25f;
    std::vector<EpaFace> faces;
    const u32 initial[4][3] = {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}};
    for (const auto& f : initial) {
        EpaFace face{};
        if (makeFace(verts, f[0], f[1], f[2], interior, face)) {
            faces.push_back(face);
        }
    }

    std::vector<std::pair<u32, u32>> horizon;
    for (u32 iteration = 0; iteration < kEpaMaxIterations && !faces.empty(); ++iteration) {
        result.iterations = iteration + 1u;
        u32 closest = 0;
        for (u32 i = 1; i < faces.size(); ++i) {
            if (faces[i].distance < faces[closest].distance) {
                closest = i;
            }
        }
        const EpaFace best = faces[closest];
        const CsoPoint p = csoSupport(hullA, countA, hullB, countB, best.normal);
        const f32 gain = p.v.dot(best.normal) - best.distance;
        if (gain < kEpaTolerance || iteration + 1u == kEpaMaxIterations) {
            // Witness points from the barycentric coordinates of the origin's projection.
            const vec3 q = best.normal * best.distance;
            const vec3 a = verts[best.a].v;
            const vec3 v0 = verts[best.b].v - a;
            const vec3 v1 = verts[best.c].v - a;
            const vec3 v2 = q - a;
            const f32 d00 = v0.dot(v0);
            const f32 d01 = v0.dot(v1);
            const f32 d11 = v1.dot(v1);
            const f32 d20 = v2.dot(v0);
            const f32 d21 = v2.dot(v1);
            const f32 denom = d00 * d11 - d01 * d01;
            f32 v = 0.f;
            f32 w = 0.f;
            if (std::fabs(denom) > 1e-20f) {
                v = (d11 * d20 - d01 * d21) / denom;
                w = (d00 * d21 - d01 * d20) / denom;
            }
            const f32 u = 1.f - v - w;
            result.pointA = verts[best.a].a * u + verts[best.b].a * v + verts[best.c].a * w;
            result.pointB = verts[best.a].b * u + verts[best.b].b * v + verts[best.c].b * w;
            result.normal = neg(best.normal); // A - B face normal points from A towards B
            result.depth = std::max(0.f, best.distance);
            result.converged = gain < kEpaTolerance;
            return result;
        }

        // Remove faces the new point sees; stitch the horizon to it.
        horizon.clear();
        for (u32 i = 0; i < faces.size();) {
            const EpaFace& f = faces[i];
            if (f.normal.dot(p.v - verts[f.a].v) > 0.f) {
                const u32 edges[3][2] = {{f.a, f.b}, {f.b, f.c}, {f.c, f.a}};
                for (const auto& e : edges) {
                    auto twin = std::find(horizon.begin(), horizon.end(), std::pair<u32, u32>{e[1], e[0]});
                    if (twin != horizon.end()) {
                        horizon.erase(twin);
                    } else {
                        horizon.emplace_back(e[0], e[1]);
                    }
                }
                faces[i] = faces.back();
                faces.pop_back();
            } else {
                ++i;
            }
        }
        const u32 newIndex = static_cast<u32>(verts.size());
        verts.push_back(p);
        for (const auto& e : horizon) {
            EpaFace face{};
            if (makeFace(verts, e.first, e.second, newIndex, interior, face)) {
                faces.push_back(face);
            }
        }
    }
    return result;
}

ContactManifold epa(
    const vec3* hullA,
    u32 countA,
    const vec3* hullB,
    u32 countB,
    u32 idxA,
    u32 idxB) {
    const EpaResult result = epaPenetration(hullA, countA, hullB, countB);
    if (!result.intersecting) {
        return invalidContactManifold();
    }
    ContactManifold manifold{};
    manifold.bodyA = idxA;
    manifold.bodyB = idxB;
    manifold.contactNormal = result.normal;
    manifold.valid = true;
    manifold.addPoint(result.pointB, result.depth);
    return manifold;
}

} // namespace fuse::physics::narrowphase
