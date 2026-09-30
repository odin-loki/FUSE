#include <fuse/physics/narrowphase/mesh_contacts.hpp>

#include <fuse/physics/narrowphase/primitive_contacts.hpp>
#include <fuse/physics/rotation.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace fuse::physics::narrowphase {

namespace {

constexpr f32 kMaxF = std::numeric_limits<f32>::max();

/// A polytope (box or hull) transformed into the mesh frame.
struct LocalHull {
    std::vector<vec3> vertices;
    std::vector<vec3> normals;
    std::vector<f32> offsets;
    HullView topology{};
    vec3 lo{};
    vec3 hi{};
};

struct MeshScratch {
    LocalHull hull;
    ContactClusterer clusterer;
    std::vector<vec3> polygon;
    std::vector<vec3> clipped;
};

MeshScratch& scratch() {
    thread_local MeshScratch s;
    return s;
}

void toLocal(const HullView& view, vec3 position, const quat& orientation, LocalHull& out) {
    out.topology = view;
    out.vertices.resize(view.vertexCount);
    out.normals.resize(view.faceCount);
    out.offsets.resize(view.faceCount);
    out.lo = {kMaxF, kMaxF, kMaxF};
    out.hi = {-kMaxF, -kMaxF, -kMaxF};
    for (u32 i = 0; i < view.vertexCount; ++i) {
        const vec3 v = position + rotate(orientation, view.vertices[i]);
        out.vertices[i] = v;
        out.lo = {std::min(out.lo.x, v.x), std::min(out.lo.y, v.y), std::min(out.lo.z, v.z)};
        out.hi = {std::max(out.hi.x, v.x), std::max(out.hi.y, v.y), std::max(out.hi.z, v.z)};
    }
    for (u32 f = 0; f < view.faceCount; ++f) {
        const vec3 n = rotate(orientation, view.faces[f].normal);
        out.normals[f] = n;
        out.offsets[f] = view.faces[f].offset + n.dot(position);
    }
}

bool edgeReal(u8 flags, u32 edge) {
    return (flags & (1u << edge)) != 0u;
}

/// Region (closestPointOnTriangle) lies on an internal feature: an internal edge, or a vertex whose
/// two triangle edges are both internal.
bool regionInternal(u32 region, u8 flags) {
    switch (region) {
    case 0: // vertex a: edges ca (2) and ab (0)
        return !edgeReal(flags, 2u) && !edgeReal(flags, 0u);
    case 1:
        return !edgeReal(flags, 0u) && !edgeReal(flags, 1u);
    case 2:
        return !edgeReal(flags, 1u) && !edgeReal(flags, 2u);
    case 3:
        return !edgeReal(flags, 0u);
    case 4:
        return !edgeReal(flags, 1u);
    case 5:
        return !edgeReal(flags, 2u);
    default:
        return false;
    }
}

/// A normal that is not the triangle's own may only lean out over one of its real edges.
bool normalAllowed(vec3 n, vec3 triNormal, const vec3 tri[3], u8 flags) {
    if (std::fabs(n.dot(triNormal)) >= 0.99f) {
        return true;
    }
    for (u32 e = 0; e < 3u; ++e) {
        if (!edgeReal(flags, e)) {
            continue;
        }
        const vec3 side = (tri[(e + 1u) % 3u] - tri[e]).cross(triNormal);
        if (side.dot(n) > 1e-4f) {
            return true;
        }
    }
    return false;
}

/// Sphere at `centre` (radius r) vs triangle: one candidate.
void sphereTriangle(vec3 centre, f32 radius, const vec3 tri[3], vec3 triNormal, u8 flags, f32 margin,
                    ContactClusterer& out) {
    u32 region = 6;
    const vec3 q = closestPointOnTriangle(centre, tri[0], tri[1], tri[2], &region);
    const vec3 delta = centre - q;
    const f32 distSq = delta.dot(delta);
    if (distSq > (radius + margin) * (radius + margin)) {
        return;
    }
    const f32 planeDist = triNormal.dot(centre - tri[0]);
    vec3 normal{};
    f32 dist = 0.f;
    if (region == 6u || regionInternal(region, flags) || distSq <= 1e-12f) {
        normal = planeDist >= 0.f ? triNormal : triNormal * -1.f;
        dist = std::fabs(planeDist);
    } else {
        dist = std::sqrt(distSq);
        normal = delta * (1.f / dist);
    }
    if (dist > radius + margin) {
        return;
    }
    out.add(centre - normal * radius, normal, radius - dist);
}

void capsuleTriangle(vec3 p0, vec3 p1, f32 radius, const vec3 tri[3], vec3 triNormal, u8 flags, f32 margin,
                     ContactClusterer& out) {
    vec3 onSeg{};
    vec3 onTri{};
    const f32 distSq = closestPointsSegmentTriangle(p0, p1, tri[0], tri[1], tri[2], onSeg, onTri);
    if (distSq > (radius + margin) * (radius + margin)) {
        return;
    }
    if (distSq <= 1e-12f) {
        // The axis pierces the triangle: push out along the face normal on the centre's side.
        const vec3 mid = (p0 + p1) * 0.5f;
        const vec3 n = triNormal.dot(mid - tri[0]) >= 0.f ? triNormal : triNormal * -1.f;
        const f32 d0 = n.dot(p0 - tri[0]);
        const f32 d1 = n.dot(p1 - tri[0]);
        const vec3 deepest = d0 <= d1 ? p0 : p1;
        out.add(deepest - n * radius, n, radius - std::min(d0, d1));
        return;
    }
    sphereTriangle(onSeg, radius, tri, triNormal, flags, margin, out);
    // Cap centres add support when the capsule lies along the triangle.
    const vec3 ends[2] = {p0, p1};
    for (const vec3& e : ends) {
        if ((e - onSeg).dot(e - onSeg) > 1e-8f) {
            sphereTriangle(e, radius, tri, triNormal, flags, margin, out);
        }
    }
}

void clipPolygon(const std::vector<vec3>& in, vec3 normal, f32 offset, std::vector<vec3>& out) {
    out.clear();
    const usize n = in.size();
    for (usize i = 0; i < n; ++i) {
        const vec3 a = in[i];
        const vec3 b = in[(i + 1u) % n];
        const f32 da = normal.dot(a) - offset;
        const f32 db = normal.dot(b) - offset;
        if (da <= 0.f) {
            out.push_back(a);
        }
        if ((da < 0.f && db > 0.f) || (da > 0.f && db < 0.f)) {
            out.push_back(a + (b - a) * (da / (da - db)));
        }
    }
}

void interval(const std::vector<vec3>& points, vec3 axis, f32& lo, f32& hi) {
    lo = kMaxF;
    hi = -kMaxF;
    for (const vec3& p : points) {
        const f32 d = p.dot(axis);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
}

/// Box / hull (in the mesh frame) vs one triangle: separating axes with face clipping.
void polytopeTriangle(const LocalHull& h, const vec3 tri[3], vec3 triNormal, u8 flags, f32 margin,
                      MeshScratch& s) {
    // Triangle face axis (two-sided: the hull's side).
    f32 hlo = 0.f;
    f32 hhi = 0.f;
    interval(h.vertices, triNormal, hlo, hhi);
    const f32 planeD = triNormal.dot(tri[0]);
    const f32 sepUp = hlo - planeD;
    const f32 sepDown = planeD - hhi;
    const vec3 nTri = sepUp >= sepDown ? triNormal : triNormal * -1.f;
    const f32 sepTri = std::max(sepUp, sepDown);
    if (sepTri >= margin) {
        return;
    }

    // Hull face axes.
    f32 sepFace = -kMaxF;
    u32 bestFace = 0;
    for (u32 f = 0; f < h.normals.size(); ++f) {
        const vec3 n = h.normals[f];
        const f32 triMin = std::min(n.dot(tri[0]), std::min(n.dot(tri[1]), n.dot(tri[2])));
        const f32 sep = triMin - h.offsets[f];
        if (sep >= margin) {
            return;
        }
        if (sep > sepFace) {
            sepFace = sep;
            bestFace = f;
        }
    }

    // Edge axes: every hull edge against every triangle edge.
    f32 sepEdge = -kMaxF;
    u32 bestHullEdge = 0;
    u32 bestTriEdge = 0;
    vec3 edgeNormal{};
    for (u32 i = 0; i < h.topology.edgeCount; ++i) {
        const vec3 h0 = h.vertices[h.topology.edges[i].v0];
        const vec3 hdir = h.vertices[h.topology.edges[i].v1] - h0;
        for (u32 e = 0; e < 3u; ++e) {
            const vec3 td = tri[(e + 1u) % 3u] - tri[e];
            vec3 axis = hdir.cross(td);
            const f32 len = axis.length();
            if (len < 0.005f * std::sqrt(hdir.dot(hdir) * td.dot(td)) || len < 1e-12f) {
                continue;
            }
            axis = axis * (1.f / len);
            f32 lo = 0.f;
            f32 hi = 0.f;
            interval(h.vertices, axis, lo, hi);
            const f32 t0 = axis.dot(tri[0]);
            const f32 t1 = axis.dot(tri[1]);
            const f32 t2 = axis.dot(tri[2]);
            const f32 tlo = std::min(t0, std::min(t1, t2));
            const f32 thi = std::max(t0, std::max(t1, t2));
            const f32 up = lo - thi;   // hull on the +axis side
            const f32 down = tlo - hi; // hull on the -axis side
            const f32 sep = std::max(up, down);
            if (sep >= margin) {
                return;
            }
            if (sep > sepEdge) {
                sepEdge = sep;
                bestHullEdge = i;
                bestTriEdge = e;
                edgeNormal = up >= down ? axis : axis * -1.f;
            }
        }
    }

    // Feature choice: prefer the triangle face, then the hull face, then an edge pair, and never a
    // normal that points across an internal edge.
    enum class Feature { TriFace, HullFace, Edge };
    Feature feature = Feature::TriFace;
    f32 refSep = sepTri;
    if (sepFace > sepTri + (0.02f * std::fabs(sepTri) + 1e-4f) &&
        normalAllowed(h.normals[bestFace] * -1.f, triNormal, tri, flags)) {
        feature = Feature::HullFace;
        refSep = sepFace;
    }
    if (sepEdge > refSep + (0.05f * std::fabs(refSep) + 5e-4f) && edgeReal(flags, bestTriEdge) &&
        normalAllowed(edgeNormal, triNormal, tri, flags)) {
        feature = Feature::Edge;
    }

    if (feature == Feature::Edge) {
        vec3 onHull{};
        vec3 onTri{};
        closestPointsSegmentSegment(h.vertices[h.topology.edges[bestHullEdge].v0],
                                    h.vertices[h.topology.edges[bestHullEdge].v1], tri[bestTriEdge],
                                    tri[(bestTriEdge + 1u) % 3u], onHull, onTri);
        s.clusterer.add((onHull + onTri) * 0.5f, edgeNormal, -sepEdge);
        return;
    }

    if (feature == Feature::TriFace) {
        // Incident hull face (most anti-parallel to the oriented triangle normal), clipped to the triangle.
        u32 incFace = 0;
        f32 most = kMaxF;
        for (u32 f = 0; f < h.normals.size(); ++f) {
            const f32 align = h.normals[f].dot(nTri);
            if (align < most) {
                most = align;
                incFace = f;
            }
        }
        const HullFace& face = h.topology.faces[incFace];
        s.polygon.clear();
        for (u32 k = 0; k < face.indexCount; ++k) {
            s.polygon.push_back(h.vertices[h.topology.faceIndices[face.firstIndex + k]]);
        }
        for (u32 e = 0; e < 3u && !s.polygon.empty(); ++e) {
            const vec3 side = (tri[(e + 1u) % 3u] - tri[e]).cross(triNormal).normalized();
            clipPolygon(s.polygon, side, side.dot(tri[e]), s.clipped);
            std::swap(s.polygon, s.clipped);
        }
        const f32 planeOffset = nTri.dot(tri[0]);
        bool any = false;
        for (const vec3& p : s.polygon) {
            const f32 depth = planeOffset - nTri.dot(p);
            if (depth >= -margin) {
                s.clusterer.add(p + nTri * (0.5f * depth), nTri, depth);
                any = true;
            }
        }
        if (!any) {
            // Nothing of the incident face lies over the triangle (e.g. a hull corner poking at the
            // triangle interior): the deepest hull vertex inside the triangle's prism.
            f32 bestDepth = -kMaxF;
            vec3 bestPoint{};
            for (const vec3& v : h.vertices) {
                u32 region = 6;
                closestPointOnTriangle(v, tri[0], tri[1], tri[2], &region);
                if (region != 6u) {
                    continue;
                }
                const f32 depth = planeOffset - nTri.dot(v);
                if (depth > bestDepth) {
                    bestDepth = depth;
                    bestPoint = v;
                }
            }
            if (bestDepth >= -margin) {
                s.clusterer.add(bestPoint + nTri * (0.5f * bestDepth), nTri, bestDepth);
            } else if (sepEdge > -kMaxF && -sepEdge >= -margin) {
                // Edge-crossing configuration on an internal edge: use the triangle normal with the
                // SAT depth so the body is still pushed out of the surface.
                vec3 onHull{};
                vec3 onTri{};
                closestPointsSegmentSegment(h.vertices[h.topology.edges[bestHullEdge].v0],
                                            h.vertices[h.topology.edges[bestHullEdge].v1], tri[bestTriEdge],
                                            tri[(bestTriEdge + 1u) % 3u], onHull, onTri);
                s.clusterer.add((onHull + onTri) * 0.5f, nTri, -sepTri);
            }
        }
        return;
    }

    // Hull face reference: the triangle is the incident polygon.
    const vec3 nf = h.normals[bestFace];
    const f32 offset = h.offsets[bestFace];
    const HullFace& face = h.topology.faces[bestFace];
    s.polygon.assign(tri, tri + 3);
    for (u32 k = 0; k < face.indexCount && !s.polygon.empty(); ++k) {
        const vec3 v0 = h.vertices[h.topology.faceIndices[face.firstIndex + k]];
        const vec3 v1 = h.vertices[h.topology.faceIndices[face.firstIndex + (k + 1u) % face.indexCount]];
        const vec3 side = (v1 - v0).cross(nf);
        const f32 len = side.length();
        if (len < 1e-12f) {
            continue;
        }
        const vec3 sideNormal = side * (1.f / len);
        clipPolygon(s.polygon, sideNormal, sideNormal.dot(v0), s.clipped);
        std::swap(s.polygon, s.clipped);
    }
    for (const vec3& p : s.polygon) {
        const f32 depth = offset - nf.dot(p);
        if (depth >= -margin) {
            s.clusterer.add(p + nf * (0.5f * depth), nf * -1.f, depth);
        }
    }
}

u32 finish(MeshScratch& s, const MeshPose& mesh, u32 idxA, u32 idxB, ContactManifold* out, u32 maxOut) {
    const u32 count = s.clusterer.build(idxA, idxB, out, maxOut);
    for (u32 m = 0; m < count; ++m) {
        ContactManifold& manifold = out[m];
        manifold.contactNormal = rotate(mesh.orientation, manifold.contactNormal);
        for (u32 i = 0; i < manifold.pointCount; ++i) {
            manifold.points[i].point = mesh.position + rotate(mesh.orientation, manifold.points[i].point);
        }
        manifold.syncLegacyFields();
    }
    return count;
}

template <typename PerTriangle>
void forTriangles(const TriMesh& mesh, vec3 lo, vec3 hi, PerTriangle&& perTriangle) {
    mesh.queryAabb(lo, hi, [&](u32 t) {
        vec3 tri[3];
        mesh.triangle(t, tri[0], tri[1], tri[2]);
        perTriangle(tri, mesh.triangleNormal(t), mesh.edgeFlags(t));
    });
}

} // namespace

vec3 closestPointOnTriangle(vec3 p, vec3 a, vec3 b, vec3 c, u32* region) {
    const vec3 ab = b - a;
    const vec3 ac = c - a;
    const vec3 ap = p - a;
    const f32 d1 = ab.dot(ap);
    const f32 d2 = ac.dot(ap);
    const auto setRegion = [&](u32 r) {
        if (region != nullptr) {
            *region = r;
        }
    };
    if (d1 <= 0.f && d2 <= 0.f) {
        setRegion(0u);
        return a;
    }
    const vec3 bp = p - b;
    const f32 d3 = ab.dot(bp);
    const f32 d4 = ac.dot(bp);
    if (d3 >= 0.f && d4 <= d3) {
        setRegion(1u);
        return b;
    }
    const f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) {
        setRegion(3u);
        return a + ab * (d1 / (d1 - d3));
    }
    const vec3 cp = p - c;
    const f32 d5 = ab.dot(cp);
    const f32 d6 = ac.dot(cp);
    if (d6 >= 0.f && d5 <= d6) {
        setRegion(2u);
        return c;
    }
    const f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) {
        setRegion(5u);
        return a + ac * (d2 / (d2 - d6));
    }
    const f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) {
        setRegion(4u);
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const f32 denom = 1.f / (va + vb + vc);
    setRegion(6u);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

f32 closestPointsSegmentTriangle(vec3 p0, vec3 p1, vec3 a, vec3 b, vec3 c, vec3& onSegment, vec3& onTriangle) {
    // Segment through the triangle: distance 0 at the crossing.
    const vec3 n = (b - a).cross(c - a);
    const f32 s0 = n.dot(p0 - a);
    const f32 s1 = n.dot(p1 - a);
    if ((s0 <= 0.f && s1 >= 0.f) || (s0 >= 0.f && s1 <= 0.f)) {
        const f32 denom = s0 - s1;
        const vec3 x = std::fabs(denom) > 1e-20f ? p0 + (p1 - p0) * (s0 / denom) : p0;
        u32 region = 6;
        const vec3 q = closestPointOnTriangle(x, a, b, c, &region);
        if (region == 6u && std::fabs(denom) > 1e-20f) {
            onSegment = x;
            onTriangle = q;
            return 0.f;
        }
    }
    f32 best = kMaxF;
    const vec3 ends[2] = {p0, p1};
    for (const vec3& e : ends) {
        const vec3 q = closestPointOnTriangle(e, a, b, c);
        const f32 d = (e - q).dot(e - q);
        if (d < best) {
            best = d;
            onSegment = e;
            onTriangle = q;
        }
    }
    const vec3 edges[3][2] = {{a, b}, {b, c}, {c, a}};
    for (const auto& edge : edges) {
        vec3 c1{};
        vec3 c2{};
        const f32 d = closestPointsSegmentSegment(p0, p1, edge[0], edge[1], c1, c2);
        if (d < best) {
            best = d;
            onSegment = c1;
            onTriangle = c2;
        }
    }
    return best;
}

u32 collideConvexMesh(const ShapeInstance& convex, const MeshPose& mesh, u32 idxA, u32 idxB, f32 margin,
                      ContactManifold* out, u32 maxOut) {
    if (mesh.mesh == nullptr || mesh.mesh->empty()) {
        return 0u;
    }
    MeshScratch& s = scratch();
    s.clusterer.reset();
    const vec3 position = inverseRotate(mesh.orientation, convex.position - mesh.position);
    const quat orientation = quatMul(quatConjugate(mesh.orientation), convex.orientation);
    const vec3 pad{margin, margin, margin};
    switch (convex.type) {
    case CollisionShapeType::Sphere: {
        const f32 r = convex.params.x;
        const vec3 reach{r, r, r};
        forTriangles(*mesh.mesh, position - reach - pad, position + reach + pad,
                     [&](const vec3* tri, vec3 n, u8 flags) { sphereTriangle(position, r, tri, n, flags, margin, s.clusterer); });
        break;
    }
    case CollisionShapeType::Capsule: {
        const f32 r = convex.params.x;
        const vec3 half = capsuleHalfAxis(orientation, std::max(convex.params.y, 0.f));
        const vec3 p0 = position - half;
        const vec3 p1 = position + half;
        const vec3 reach{r, r, r};
        const vec3 lo{std::min(p0.x, p1.x), std::min(p0.y, p1.y), std::min(p0.z, p1.z)};
        const vec3 hi{std::max(p0.x, p1.x), std::max(p0.y, p1.y), std::max(p0.z, p1.z)};
        forTriangles(*mesh.mesh, lo - reach - pad, hi + reach + pad, [&](const vec3* tri, vec3 n, u8 flags) {
            capsuleTriangle(p0, p1, r, tri, n, flags, margin, s.clusterer);
        });
        break;
    }
    case CollisionShapeType::Box: {
        BoxHull box;
        initBoxHull(box, convex.params);
        toLocal(box.view(), position, orientation, s.hull);
        forTriangles(*mesh.mesh, s.hull.lo - pad, s.hull.hi + pad,
                     [&](const vec3* tri, vec3 n, u8 flags) { polytopeTriangle(s.hull, tri, n, flags, margin, s); });
        break;
    }
    default:
        return 0u;
    }
    return finish(s, mesh, idxA, idxB, out, maxOut);
}

u32 collideHullMesh(const HullPose& hull, const MeshPose& mesh, u32 idxA, u32 idxB, f32 margin, ContactManifold* out,
                    u32 maxOut) {
    if (mesh.mesh == nullptr || mesh.mesh->empty() || hull.hull.empty()) {
        return 0u;
    }
    MeshScratch& s = scratch();
    s.clusterer.reset();
    const vec3 position = inverseRotate(mesh.orientation, hull.position - mesh.position);
    const quat orientation = quatMul(quatConjugate(mesh.orientation), hull.orientation);
    toLocal(hull.hull, position, orientation, s.hull);
    const vec3 pad{margin, margin, margin};
    forTriangles(*mesh.mesh, s.hull.lo - pad, s.hull.hi + pad,
                 [&](const vec3* tri, vec3 n, u8 flags) { polytopeTriangle(s.hull, tri, n, flags, margin, s); });
    return finish(s, mesh, idxA, idxB, out, maxOut);
}

} // namespace fuse::physics::narrowphase
