#include <fuse/physics/narrowphase/hull_contacts.hpp>

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/primitive_contacts.hpp>
#include <fuse/physics/rotation.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace fuse::physics::narrowphase {

namespace {

constexpr f32 kMaxF = std::numeric_limits<f32>::max();

/// World-space copy of a hull's vertices and face planes (thread-local scratch: no steady-state
/// allocation once the buffers have grown to the largest hull seen).
struct WorldHull {
    std::vector<vec3> vertices;
    std::vector<vec3> normals;
    std::vector<f32> offsets;
    HullView topology{};
    vec3 centroid{};
};

struct HullScratch {
    WorldHull a;
    WorldHull b;
    std::vector<vec3> polygon;
    std::vector<vec3> clipped;
    std::vector<hd::DepthPoint> points;
};

HullScratch& scratch() {
    thread_local HullScratch s;
    return s;
}

void toWorld(const HullPose& pose, WorldHull& out) {
    const HullView& h = pose.hull;
    out.topology = h;
    out.vertices.resize(h.vertexCount);
    out.normals.resize(h.faceCount);
    out.offsets.resize(h.faceCount);
    for (u32 i = 0; i < h.vertexCount; ++i) {
        out.vertices[i] = pose.position + rotate(pose.orientation, h.vertices[i]);
    }
    for (u32 f = 0; f < h.faceCount; ++f) {
        const vec3 n = rotate(pose.orientation, h.faces[f].normal);
        out.normals[f] = n;
        out.offsets[f] = h.faces[f].offset + n.dot(pose.position);
    }
    out.centroid = pose.position + rotate(pose.orientation, h.centroid);
}

f32 minProjection(const WorldHull& hull, vec3 axis) {
    f32 best = kMaxF;
    for (const vec3& v : hull.vertices) {
        best = std::min(best, v.dot(axis));
    }
    return best;
}

struct FaceQuery {
    f32 separation = -kMaxF;
    u32 face = 0;
};

FaceQuery queryFaces(const WorldHull& a, const WorldHull& b) {
    FaceQuery q{};
    for (u32 f = 0; f < a.normals.size(); ++f) {
        const f32 sep = minProjection(b, a.normals[f]) - a.offsets[f];
        if (sep > q.separation) {
            q.separation = sep;
            q.face = f;
        }
    }
    return q;
}

bool isMinkowskiFace(vec3 a, vec3 b, vec3 bxa, vec3 c, vec3 d, vec3 dxc) {
    const f32 cba = c.dot(bxa);
    const f32 dba = d.dot(bxa);
    const f32 adc = a.dot(dxc);
    const f32 bdc = b.dot(dxc);
    return cba * dba < 0.f && adc * bdc < 0.f && cba * bdc > 0.f;
}

struct EdgeQuery {
    f32 separation = -kMaxF;
    u32 edgeA = 0;
    u32 edgeB = 0;
    vec3 axis{};
};

EdgeQuery queryEdges(const WorldHull& a, const WorldHull& b) {
    EdgeQuery q{};
    const HullView& ta = a.topology;
    const HullView& tb = b.topology;
    for (u32 i = 0; i < ta.edgeCount; ++i) {
        const HullEdge& ea = ta.edges[i];
        const vec3 pa = a.vertices[ea.v0];
        const vec3 dirA = a.vertices[ea.v1] - pa;
        const vec3 na0 = a.normals[ea.face0];
        const vec3 na1 = a.normals[ea.face1];
        const vec3 bxa = na1.cross(na0);
        for (u32 j = 0; j < tb.edgeCount; ++j) {
            const HullEdge& eb = tb.edges[j];
            const vec3 nc = b.normals[eb.face0] * -1.f;
            const vec3 nd = b.normals[eb.face1] * -1.f;
            if (!isMinkowskiFace(na0, na1, bxa, nc, nd, nd.cross(nc))) {
                continue;
            }
            const vec3 pb = b.vertices[eb.v0];
            const vec3 dirB = b.vertices[eb.v1] - pb;
            const vec3 cross = dirA.cross(dirB);
            const f32 len = cross.length();
            if (len < 0.005f * std::sqrt(dirA.dot(dirA) * dirB.dot(dirB))) {
                continue; // parallel edges: the face axes cover them
            }
            vec3 axis = cross * (1.f / len);
            if (axis.dot(pa - a.centroid) < 0.f) {
                axis = axis * -1.f;
            }
            const f32 sep = axis.dot(pb - pa);
            if (sep > q.separation) {
                q.separation = sep;
                q.edgeA = i;
                q.edgeB = j;
                q.axis = axis;
            }
        }
    }
    return q;
}

vec3 anyPerpendicular(vec3 n) {
    const vec3 hint = std::fabs(n.x) < 0.9f ? vec3{1.f, 0.f, 0.f} : vec3{0.f, 1.f, 0.f};
    return n.cross(hint).normalized();
}

/// Keeps the part of `in` with `normal . p <= offset`.
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

/// Face contact: reference face `refFace` of `ref`, incident face of `inc` clipped against it.
ContactManifold faceContact(const WorldHull& ref, u32 refFace, const WorldHull& inc, bool refIsA, u32 idxA,
                            u32 idxB, f32 margin) {
    HullScratch& s = scratch();
    const vec3 refNormal = ref.normals[refFace];
    const f32 refOffset = ref.offsets[refFace];

    u32 incFace = 0;
    f32 most = kMaxF;
    for (u32 f = 0; f < inc.normals.size(); ++f) {
        const f32 align = inc.normals[f].dot(refNormal);
        if (align < most) {
            most = align;
            incFace = f;
        }
    }
    const HullFace& incident = inc.topology.faces[incFace];
    s.polygon.clear();
    for (u32 k = 0; k < incident.indexCount; ++k) {
        s.polygon.push_back(inc.vertices[inc.topology.faceIndices[incident.firstIndex + k]]);
    }
    const HullFace& reference = ref.topology.faces[refFace];
    for (u32 k = 0; k < reference.indexCount && !s.polygon.empty(); ++k) {
        const vec3 v0 = ref.vertices[ref.topology.faceIndices[reference.firstIndex + k]];
        const vec3 v1 = ref.vertices[ref.topology.faceIndices[reference.firstIndex + (k + 1u) % reference.indexCount]];
        const vec3 side = (v1 - v0).cross(refNormal);
        const f32 len = side.length();
        if (len < 1e-12f) {
            continue;
        }
        const vec3 sideNormal = side * (1.f / len);
        clipPolygon(s.polygon, sideNormal, sideNormal.dot(v0), s.clipped);
        std::swap(s.polygon, s.clipped);
    }

    ContactManifold manifold{};
    manifold.bodyA = idxA;
    manifold.bodyB = idxB;
    manifold.contactNormal = refIsA ? refNormal * -1.f : refNormal;
    manifold.valid = true;

    s.points.clear();
    for (const vec3& p : s.polygon) {
        const f32 depth = refOffset - refNormal.dot(p);
        if (depth >= -margin) {
            s.points.push_back({p + refNormal * (0.5f * depth), depth});
        }
    }
    if (s.points.empty()) {
        // Numerical corner case (the clipped polygon slipped past the face): deepest incident vertex.
        f32 bestDepth = -kMaxF;
        vec3 bestPoint{};
        for (const vec3& v : inc.vertices) {
            const f32 depth = refOffset - refNormal.dot(v);
            if (depth > bestDepth) {
                bestDepth = depth;
                bestPoint = v;
            }
        }
        if (bestDepth < -margin) {
            return invalidContactManifold();
        }
        manifold.addPoint(bestPoint + refNormal * (0.5f * bestDepth), bestDepth);
        return manifold;
    }
    const vec3 v0 = ref.vertices[ref.topology.faceIndices[reference.firstIndex]];
    const vec3 v1 = ref.vertices[ref.topology.faceIndices[reference.firstIndex + 1u]];
    vec3 s1 = (v1 - v0);
    s1 = s1.length() > 1e-12f ? s1.normalized() : anyPerpendicular(refNormal);
    const vec3 s2 = refNormal.cross(s1);
    const u32 count = hd::reduceToFour(s.points.data(), static_cast<u32>(s.points.size()), refNormal,
                                       s1 * 0.8f + s2 * 0.6f, 1e-3f);
    for (u32 i = 0; i < count; ++i) {
        manifold.addPoint(s.points[i].point, s.points[i].depth);
    }
    return manifold;
}

/// Clips the segment [p0, p1] to a face's side planes (hull body frame); false when nothing is left.
bool clipSegmentToFace(const HullView& hull, u32 face, vec3& p0, vec3& p1) {
    const HullFace& f = hull.faces[face];
    f32 t0 = 0.f;
    f32 t1 = 1.f;
    const vec3 d = p1 - p0;
    for (u32 k = 0; k < f.indexCount; ++k) {
        const vec3 a = hull.vertices[hull.faceIndices[f.firstIndex + k]];
        const vec3 b = hull.vertices[hull.faceIndices[f.firstIndex + (k + 1u) % f.indexCount]];
        const vec3 side = (b - a).cross(f.normal);
        const f32 start = side.dot(p0 - a);
        const f32 rate = side.dot(d);
        if (std::fabs(rate) < 1e-12f) {
            if (start > 0.f) {
                return false;
            }
            continue;
        }
        const f32 t = -start / rate;
        if (rate > 0.f) {
            t1 = std::min(t1, t);
        } else {
            t0 = std::max(t0, t);
        }
        if (t0 > t1) {
            return false;
        }
    }
    const vec3 a = p0 + d * t0;
    const vec3 b = p0 + d * t1;
    p0 = a;
    p1 = b;
    return true;
}

} // namespace

f32 HullSatResult::separation() const {
    return std::max(std::max(faceSeparationA, faceSeparationB), edgeSeparation);
}

HullSatResult hullSatQuery(const HullPose& a, const HullPose& b) {
    HullScratch& s = scratch();
    toWorld(a, s.a);
    toWorld(b, s.b);
    const FaceQuery fa = queryFaces(s.a, s.b);
    const FaceQuery fb = queryFaces(s.b, s.a);
    const EdgeQuery e = queryEdges(s.a, s.b);
    HullSatResult r{};
    r.faceSeparationA = fa.separation;
    r.faceSeparationB = fb.separation;
    r.faceA = fa.face;
    r.faceB = fb.face;
    r.edgeSeparation = e.separation;
    r.edgeA = e.edgeA;
    r.edgeB = e.edgeB;
    r.edgeAxis = e.axis;
    return r;
}

f32 hullSatBruteForce(const HullPose& a, const HullPose& b, vec3* axisOut) {
    HullScratch& s = scratch();
    toWorld(a, s.a);
    toWorld(b, s.b);
    f32 best = -kMaxF;
    vec3 bestAxis{};
    const auto test = [&](vec3 axis) {
        const f32 len = axis.length();
        if (len < 1e-6f) {
            return;
        }
        axis = axis * (1.f / len);
        f32 maxA = -kMaxF;
        f32 minA = kMaxF;
        for (const vec3& v : s.a.vertices) {
            maxA = std::max(maxA, v.dot(axis));
            minA = std::min(minA, v.dot(axis));
        }
        f32 maxB = -kMaxF;
        f32 minB = kMaxF;
        for (const vec3& v : s.b.vertices) {
            maxB = std::max(maxB, v.dot(axis));
            minB = std::min(minB, v.dot(axis));
        }
        // Separation along the better of the two directions.
        const f32 forward = minB - maxA; // A below B along axis
        const f32 backward = minA - maxB;
        if (forward >= backward && forward > best) {
            best = forward;
            bestAxis = axis;
        } else if (backward > forward && backward > best) {
            best = backward;
            bestAxis = axis * -1.f;
        }
    };
    for (const vec3& n : s.a.normals) {
        test(n);
    }
    for (const vec3& n : s.b.normals) {
        test(n);
    }
    for (u32 i = 0; i < s.a.topology.edgeCount; ++i) {
        const HullEdge& ea = s.a.topology.edges[i];
        const vec3 dirA = s.a.vertices[ea.v1] - s.a.vertices[ea.v0];
        for (u32 j = 0; j < s.b.topology.edgeCount; ++j) {
            const HullEdge& eb = s.b.topology.edges[j];
            const vec3 dirB = s.b.vertices[eb.v1] - s.b.vertices[eb.v0];
            const vec3 cross = dirA.cross(dirB);
            if (cross.length() < 0.005f * std::sqrt(dirA.dot(dirA) * dirB.dot(dirB))) {
                continue;
            }
            test(cross);
        }
    }
    if (axisOut != nullptr) {
        *axisOut = bestAxis;
    }
    return best;
}

ContactManifold collideHullHull(const HullPose& a, const HullPose& b, u32 idxA, u32 idxB, f32 margin) {
    if (a.hull.empty() || b.hull.empty()) {
        return invalidContactManifold();
    }
    HullScratch& s = scratch();
    toWorld(a, s.a);
    toWorld(b, s.b);
    const FaceQuery fa = queryFaces(s.a, s.b);
    if (fa.separation >= margin) {
        return invalidContactManifold();
    }
    const FaceQuery fb = queryFaces(s.b, s.a);
    if (fb.separation >= margin) {
        return invalidContactManifold();
    }
    const EdgeQuery e = queryEdges(s.a, s.b);
    if (e.separation >= margin) {
        return invalidContactManifold();
    }

    // Prefer faces (stable resting manifolds), and A's face over B's: the same biases as the oriented
    // box-box pair, written on separations (= -penetration).
    const bool referenceIsB = fb.separation > fa.separation + (0.02f * std::fabs(fa.separation) + 1e-4f);
    const f32 faceSeparation = referenceIsB ? fb.separation : fa.separation;
    if (e.separation > -kMaxF && e.separation > faceSeparation + (0.05f * std::fabs(faceSeparation) + 5e-4f)) {
        const HullEdge& ea = s.a.topology.edges[e.edgeA];
        const HullEdge& eb = s.b.topology.edges[e.edgeB];
        vec3 onA{};
        vec3 onB{};
        closestPointsSegmentSegment(s.a.vertices[ea.v0], s.a.vertices[ea.v1], s.b.vertices[eb.v0],
                                    s.b.vertices[eb.v1], onA, onB);
        ContactManifold manifold{};
        manifold.bodyA = idxA;
        manifold.bodyB = idxB;
        manifold.valid = true;
        manifold.contactNormal = e.axis * -1.f; // axis points out of A towards B
        manifold.addPoint((onA + onB) * 0.5f, -e.separation);
        return manifold;
    }
    return referenceIsB ? faceContact(s.b, fb.face, s.a, false, idxA, idxB, margin)
                        : faceContact(s.a, fa.face, s.b, true, idxA, idxB, margin);
}

ContactManifold collideHullPlane(const HullPose& hull, vec3 planeNormal, f32 planeDistance, u32 idxHull, u32 idxPlane,
                                 f32 margin) {
    if (hull.hull.empty()) {
        return invalidContactManifold();
    }
    HullScratch& s = scratch();
    s.points.clear();
    f32 reach = 0.f;
    const vec3 localNormal = inverseRotate(hull.orientation, planeNormal);
    for (u32 i = 0; i < hull.hull.vertexCount; ++i) {
        const vec3 local = hull.hull.vertices[i];
        reach = std::max(reach, -local.dot(localNormal));
        const vec3 corner = hull.position + rotate(hull.orientation, local);
        const f32 distance = corner.dot(planeNormal) - planeDistance;
        if (distance <= margin) {
            s.points.push_back({corner - planeNormal * (0.5f * distance), -distance});
        }
    }
    if (s.points.empty()) {
        return invalidContactManifold();
    }
    u32 count = static_cast<u32>(s.points.size());
    if (count > kMaxContactPointsPerManifold) {
        count = hd::reduceToFour(s.points.data(), count, planeNormal, rotate(hull.orientation, {0.8f, 0.f, 0.6f}), 1e-3f);
    }
    ContactManifold manifold{};
    manifold.contactNormal = planeNormal;
    manifold.minSeparation = reach;
    manifold.bodyA = idxHull;
    manifold.bodyB = idxPlane;
    manifold.valid = true;
    for (u32 i = 0; i < count; ++i) {
        manifold.addPoint(s.points[i].point, s.points[i].depth);
    }
    return manifold;
}

ContactManifold collideSphereHull(vec3 spherePos, f32 sphereRadius, const HullPose& hull, u32 idxSphere, u32 idxHull,
                                  f32 margin) {
    if (hull.hull.empty()) {
        return invalidContactManifold();
    }
    const vec3 local = inverseRotate(hull.orientation, spherePos - hull.position);
    u32 face = 0;
    const f32 maxDistance = hullMaxFaceDistance(hull.hull, local, &face);
    if (maxDistance > sphereRadius + margin) {
        return invalidContactManifold();
    }
    vec3 normal{};
    f32 penetration = 0.f;
    if (maxDistance <= 0.f) {
        normal = hull.hull.faces[face].normal;
        penetration = sphereRadius - maxDistance;
    } else {
        const vec3 closest = hullClosestPoint(hull.hull, local);
        const vec3 delta = local - closest;
        const f32 distSq = delta.dot(delta);
        if (distSq > (sphereRadius + margin) * (sphereRadius + margin)) {
            return invalidContactManifold();
        }
        if (distSq > 1e-12f) {
            const f32 dist = std::sqrt(distSq);
            normal = delta * (1.f / dist);
            penetration = sphereRadius - dist;
        } else {
            normal = hull.hull.faces[face].normal;
            penetration = sphereRadius - maxDistance;
        }
    }
    const vec3 worldNormal = rotate(hull.orientation, normal);
    ContactManifold manifold{};
    manifold.contactNormal = worldNormal;
    manifold.minSeparation = sphereRadius;
    manifold.bodyA = idxSphere;
    manifold.bodyB = idxHull;
    manifold.valid = true;
    manifold.addPoint(spherePos - worldNormal * sphereRadius, penetration);
    return manifold;
}

f32 segmentHullDistance(const HullView& hull, vec3 p0, vec3 p1, vec3& onSegment, vec3& onHull) {
    // Does the segment enter the hull? (Cyrus-Beck against every face plane.)
    const vec3 d = p1 - p0;
    f32 tEnter = 0.f;
    f32 tExit = 1.f;
    bool outside = false;
    for (u32 f = 0; f < hull.faceCount && !outside; ++f) {
        const f32 start = hull.faces[f].normal.dot(p0) - hull.faces[f].offset;
        const f32 rate = hull.faces[f].normal.dot(d);
        if (std::fabs(rate) < 1e-12f) {
            outside = start > 0.f;
            continue;
        }
        const f32 t = -start / rate;
        if (rate > 0.f) {
            tExit = std::min(tExit, t);
        } else {
            tEnter = std::max(tEnter, t);
        }
        outside = tEnter > tExit;
    }
    if (!outside) {
        onSegment = p0 + d * tEnter;
        onHull = onSegment;
        return 0.f;
    }
    // Disjoint: the closest pair involves a segment endpoint or a hull edge.
    f32 bestSq = kMaxF;
    const vec3 ends[2] = {p0, p1};
    for (const vec3& e : ends) {
        const vec3 q = hullClosestPoint(hull, e);
        const f32 dsq = (e - q).dot(e - q);
        if (dsq < bestSq) {
            bestSq = dsq;
            onSegment = e;
            onHull = q;
        }
    }
    for (u32 i = 0; i < hull.edgeCount; ++i) {
        vec3 c1{};
        vec3 c2{};
        const f32 dsq = closestPointsSegmentSegment(p0, p1, hull.vertices[hull.edges[i].v0], hull.vertices[hull.edges[i].v1],
                                                    c1, c2);
        if (dsq < bestSq) {
            bestSq = dsq;
            onSegment = c1;
            onHull = c2;
        }
    }
    return std::sqrt(bestSq);
}

ContactManifold collideCapsuleHull(vec3 capsulePos, quat capsuleRotation, vec3 capsuleParams, const HullPose& hull,
                                   u32 idxCapsule, u32 idxHull, f32 margin) {
    if (hull.hull.empty()) {
        return invalidContactManifold();
    }
    const HullView& h = hull.hull;
    const f32 radius = capsuleParams.x;
    const vec3 half = capsuleHalfAxis(capsuleRotation, std::max(capsuleParams.y, 0.f));
    // Hull body frame.
    const vec3 a = inverseRotate(hull.orientation, capsulePos - half - hull.position);
    const vec3 b = inverseRotate(hull.orientation, capsulePos + half - hull.position);

    ContactManifold manifold{};
    manifold.bodyA = idxCapsule;
    manifold.bodyB = idxHull;
    manifold.minSeparation = radius;
    manifold.valid = true;
    const auto emit = [&](vec3 localNormal, vec3 localAxisPoint, f32 depth) {
        manifold.addPoint(hull.position + rotate(hull.orientation, localAxisPoint - localNormal * radius), depth);
    };

    vec3 onSeg{};
    vec3 onHull{};
    const f32 distance = segmentHullDistance(h, a, b, onSeg, onHull);
    vec3 normal{};
    if (distance > 0.f) {
        if (distance > radius + margin) {
            return invalidContactManifold();
        }
        if (distance > 1e-6f) {
            normal = (onSeg - onHull) * (1.f / distance);
        } else {
            u32 face = 0;
            hullMaxFaceDistance(h, onSeg, &face);
            normal = h.faces[face].normal;
        }
        manifold.contactNormal = rotate(hull.orientation, normal);
        emit(normal, onSeg, radius - distance);
    } else {
        // Axis inside the hull: least-penetration axis over the face normals and segment x edge axes.
        f32 bestSep = -kMaxF;
        u32 bestFace = 0;
        bool faceAxis = true;
        u32 bestEdge = 0;
        vec3 bestAxis{};
        for (u32 f = 0; f < h.faceCount; ++f) {
            const vec3 n = h.faces[f].normal;
            const f32 sep = std::min(n.dot(a), n.dot(b)) - h.faces[f].offset;
            if (sep > bestSep) {
                bestSep = sep;
                bestFace = f;
                bestAxis = n;
            }
        }
        const vec3 segDir = b - a;
        const vec3 mid = (a + b) * 0.5f;
        for (u32 i = 0; i < h.edgeCount; ++i) {
            const vec3 e0 = h.vertices[h.edges[i].v0];
            const vec3 edgeDir = h.vertices[h.edges[i].v1] - e0;
            vec3 axis = segDir.cross(edgeDir);
            const f32 len = axis.length();
            if (len < 0.005f * std::sqrt(segDir.dot(segDir) * edgeDir.dot(edgeDir)) || len < 1e-9f) {
                continue;
            }
            axis = axis * (1.f / len);
            if (axis.dot(mid - h.centroid) < 0.f) {
                axis = axis * -1.f;
            }
            f32 support = -kMaxF;
            for (u32 v = 0; v < h.vertexCount; ++v) {
                support = std::max(support, h.vertices[v].dot(axis));
            }
            const f32 sep = std::min(axis.dot(a), axis.dot(b)) - support;
            if (sep > bestSep + (0.05f * std::fabs(bestSep) + 5e-4f)) {
                bestSep = sep;
                bestAxis = axis;
                bestEdge = i;
                faceAxis = false;
            }
        }
        normal = bestAxis;
        manifold.contactNormal = rotate(hull.orientation, normal);
        if (!faceAxis) {
            vec3 c1{};
            vec3 c2{};
            closestPointsSegmentSegment(a, b, h.vertices[h.edges[bestEdge].v0], h.vertices[h.edges[bestEdge].v1], c1,
                                        c2);
            emit(normal, c1, radius - bestSep);
            return manifold;
        }
        // Face axis: both clipped ends of the axis against that face.
        vec3 p0 = a;
        vec3 p1 = b;
        if (clipSegmentToFace(h, bestFace, p0, p1)) {
            const HullFace& f = h.faces[bestFace];
            emit(normal, p0, radius - (f.normal.dot(p0) - f.offset));
            if ((p1 - p0).dot(p1 - p0) > 1e-8f) {
                emit(normal, p1, radius - (f.normal.dot(p1) - f.offset));
            }
        } else {
            const vec3 deepest = normal.dot(a) <= normal.dot(b) ? a : b;
            emit(normal, deepest, radius - bestSep);
        }
        return manifold;
    }

    // Capsule lying on a face: add the axis ends clipped to that face as support points.
    const f32 axisLen = (b - a).length();
    if (axisLen > 1e-6f) {
        u32 face = h.faceCount;
        f32 bestAlign = 0.99f;
        for (u32 f = 0; f < h.faceCount; ++f) {
            const f32 align = h.faces[f].normal.dot(normal);
            if (align > bestAlign) {
                bestAlign = align;
                face = f;
            }
        }
        if (face < h.faceCount) {
            vec3 p0 = a;
            vec3 p1 = b;
            if (clipSegmentToFace(h, face, p0, p1)) {
                const HullFace& f = h.faces[face];
                const vec3 ends[2] = {p0, p1};
                for (const vec3& end : ends) {
                    if ((end - onSeg).dot(end - onSeg) < 1e-8f) {
                        continue;
                    }
                    const f32 dist = f.normal.dot(end) - f.offset;
                    if (dist <= radius + margin && dist > 0.f) {
                        emit(f.normal, end, radius - dist);
                    }
                }
            }
        }
    }
    return manifold;
}

} // namespace fuse::physics::narrowphase
