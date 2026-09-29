#pragma once

// Primitive shape-pair contact generation shared by the CPU narrowphase and the CUDA resident
// narrowphase (fuse/physics/resident/resident_kernels.hpp). Every function is FUSE_HOST_DEVICE and
// allocation-free. The bodies were moved verbatim from box_box.cpp / oriented_shapes.cpp, whose
// public functions (collision_dispatch.hpp) now forward here, so the CPU path computes exactly what it
// computed before and the device path computes the same thing (the physics .cu files are built with
// --fmad=false so nvcc does not contract a * b + c into FMAs that the host never uses).
//
//   hd::collideBoxBox / collideOrientedBoxPlane / collideOrientedBoxSphere / collideOrientedBoxBox /
//   collideCapsulePlane / collideCapsuleBox   the out-of-line pairs of collision_dispatch.hpp
//   collidePrimitiveShapes                    the sphere / box / capsule / plane part of collideShapes

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/rotation.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics::narrowphase {

namespace hd {

struct Aabb {
    vec3 min{};
    vec3 max{};
};

FUSE_HOST_DEVICE inline Aabb makeAabb(vec3 center, vec3 halfExtents) {
    return {
        center - halfExtents,
        center + halfExtents,
    };
}

FUSE_HOST_DEVICE inline f32 overlapOnAxis(f32 minA, f32 maxA, f32 minB, f32 maxB) {
    return std::min(maxA, maxB) - std::max(minA, minB);
}

FUSE_HOST_DEVICE inline ContactManifold collideBoxBox(
    vec3 posA,
    vec3 halfExtentsA,
    vec3 posB,
    vec3 halfExtentsB,
    u32 idxA,
    u32 idxB,
    f32 margin) {
    const Aabb aabbA = makeAabb(posA, halfExtentsA);
    const Aabb aabbB = makeAabb(posB, halfExtentsB);

    const f32 overlapX = overlapOnAxis(aabbA.min.x, aabbA.max.x, aabbB.min.x, aabbB.max.x);
    const f32 overlapY = overlapOnAxis(aabbA.min.y, aabbA.max.y, aabbB.min.y, aabbB.max.y);
    const f32 overlapZ = overlapOnAxis(aabbA.min.z, aabbA.max.z, aabbB.min.z, aabbB.max.z);
    if (overlapX <= -margin || overlapY <= -margin || overlapZ <= -margin) {
        return invalidContactManifold();
    }
    // Speculative (separated within the margin) only across one face.
    const int separatedAxes = (overlapX <= 0.f ? 1 : 0) + (overlapY <= 0.f ? 1 : 0) + (overlapZ <= 0.f ? 1 : 0);
    if (separatedAxes > 1) {
        return invalidContactManifold();
    }

    f32 penetration = overlapX;
    vec3 normal{posA.x >= posB.x ? 1.f : -1.f, 0.f, 0.f};
    if (overlapY < penetration) {
        penetration = overlapY;
        normal = {0.f, posA.y >= posB.y ? 1.f : -1.f, 0.f};
    }
    if (overlapZ < penetration) {
        penetration = overlapZ;
        normal = {0.f, 0.f, posA.z >= posB.z ? 1.f : -1.f};
    }

    const f32 overlapMinX = std::max(aabbA.min.x, aabbB.min.x);
    const f32 overlapMaxX = std::min(aabbA.max.x, aabbB.max.x);
    const f32 overlapMinY = std::max(aabbA.min.y, aabbB.min.y);
    const f32 overlapMaxY = std::min(aabbA.max.y, aabbB.max.y);
    const f32 overlapMinZ = std::max(aabbA.min.z, aabbB.min.z);
    const f32 overlapMaxZ = std::min(aabbA.max.z, aabbB.max.z);

    const f32 faceCoord =
        normal.x != 0.f ? (normal.x > 0.f ? aabbA.min.x : aabbA.max.x)
        : normal.y != 0.f ? (normal.y > 0.f ? aabbA.min.y : aabbA.max.y)
                          : (normal.z > 0.f ? aabbA.min.z : aabbA.max.z);

    ContactManifold manifold{};
    manifold.contactNormal = normal;
    manifold.minSeparation = 0.f;
    manifold.bodyA = idxA;
    manifold.bodyB = idxB;
    manifold.valid = true;

    vec3 corners[4]{};
    if (normal.x != 0.f) {
        corners[0] = {faceCoord, overlapMinY, overlapMinZ};
        corners[1] = {faceCoord, overlapMaxY, overlapMinZ};
        corners[2] = {faceCoord, overlapMaxY, overlapMaxZ};
        corners[3] = {faceCoord, overlapMinY, overlapMaxZ};
    } else if (normal.y != 0.f) {
        corners[0] = {overlapMinX, faceCoord, overlapMinZ};
        corners[1] = {overlapMaxX, faceCoord, overlapMinZ};
        corners[2] = {overlapMaxX, faceCoord, overlapMaxZ};
        corners[3] = {overlapMinX, faceCoord, overlapMaxZ};
    } else {
        corners[0] = {overlapMinX, overlapMinY, faceCoord};
        corners[1] = {overlapMaxX, overlapMinY, faceCoord};
        corners[2] = {overlapMaxX, overlapMaxY, faceCoord};
        corners[3] = {overlapMinX, overlapMaxY, faceCoord};
    }

    for (const vec3& point : corners) {
        manifold.addPoint(point, penetration);
    }

    return manifold;
}

struct Obb {
    vec3 center{};
    vec3 axis[3]{};
    f32 extent[3]{};
};

FUSE_HOST_DEVICE inline Obb makeObb(vec3 center, const quat& rotation, vec3 halfExtents) {
    Obb box{};
    box.center = center;
    box.axis[0] = rotate(rotation, {1.f, 0.f, 0.f});
    box.axis[1] = rotate(rotation, {0.f, 1.f, 0.f});
    box.axis[2] = rotate(rotation, {0.f, 0.f, 1.f});
    box.extent[0] = halfExtents.x;
    box.extent[1] = halfExtents.y;
    box.extent[2] = halfExtents.z;
    return box;
}

FUSE_HOST_DEVICE inline f32 projectedRadius(const Obb& box, vec3 axis) {
    return box.extent[0] * std::fabs(box.axis[0].dot(axis)) + box.extent[1] * std::fabs(box.axis[1].dot(axis)) +
           box.extent[2] * std::fabs(box.axis[2].dot(axis));
}

inline constexpr u32 kMaxClipPoints = 16u;

struct ClipPolygon {
    vec3 points[kMaxClipPoints]{};
    u32 count = 0;
};

/// Keeps the part of `in` with `normal . p <= offset` (Sutherland-Hodgman, one plane).
FUSE_HOST_DEVICE inline void clipAgainstPlane(const ClipPolygon& in, vec3 normal, f32 offset, ClipPolygon& out) {
    out.count = 0;
    if (in.count == 0u) {
        return;
    }
    for (u32 i = 0; i < in.count; ++i) {
        const vec3 a = in.points[i];
        const vec3 b = in.points[(i + 1u) % in.count];
        const f32 da = normal.dot(a) - offset;
        const f32 db = normal.dot(b) - offset;
        if (da <= 0.f && out.count < kMaxClipPoints) {
            out.points[out.count++] = a;
        }
        if ((da < 0.f && db > 0.f) || (da > 0.f && db < 0.f)) {
            const f32 t = da / (da - db);
            if (out.count < kMaxClipPoints) {
                out.points[out.count++] = a + (b - a) * t;
            }
        }
    }
}

struct DepthPoint {
    vec3 point{};
    f32 depth = 0.f;
};

/// Picks at most four points spanning the largest area from `points`, starting from the deepest.
/// Points within `depthTolerance` of the deepest count as equally deep and the tie is broken by
/// position along `tieAxis`, so a resting face keeps the same four points from substep to substep
/// instead of flipping with depth noise.
FUSE_HOST_DEVICE inline u32 reduceToFour(DepthPoint* points, u32 count, vec3 normal, vec3 tieAxis, f32 depthTolerance) {
    if (count <= kMaxContactPointsPerManifold) {
        return count;
    }
    u32 pick[4]{};
    f32 deepest = points[0].depth;
    for (u32 i = 1; i < count; ++i) {
        deepest = std::max(deepest, points[i].depth);
    }
    f32 bestTie = -1e30f;
    for (u32 i = 0; i < count; ++i) {
        const f32 tie = points[i].point.dot(tieAxis);
        if (points[i].depth >= deepest - depthTolerance && tie > bestTie) {
            bestTie = tie;
            pick[0] = i;
        }
    }
    const vec3 p0 = points[pick[0]].point;
    f32 best = -1.f;
    for (u32 i = 0; i < count; ++i) {
        const f32 d = (points[i].point - p0).dot(points[i].point - p0);
        if (d > best) {
            best = d;
            pick[1] = i;
        }
    }
    const vec3 edge = points[pick[1]].point - p0;
    f32 most = -1e30f;
    f32 least = 1e30f;
    pick[2] = pick[0];
    pick[3] = pick[1];
    for (u32 i = 0; i < count; ++i) {
        const f32 area = edge.cross(points[i].point - p0).dot(normal);
        if (area > most) {
            most = area;
            pick[2] = i;
        }
        if (area < least) {
            least = area;
            pick[3] = i;
        }
    }
    DepthPoint chosen[4]{};
    u32 chosenCount = 0;
    for (u32 k = 0; k < 4u; ++k) {
        bool duplicate = false;
        for (u32 j = 0; j < k; ++j) {
            duplicate = duplicate || pick[j] == pick[k];
        }
        if (!duplicate) {
            chosen[chosenCount++] = points[pick[k]];
        }
    }
    for (u32 k = 0; k < chosenCount; ++k) {
        points[k] = chosen[k];
    }
    return chosenCount;
}

/// Signed distance from a point to an origin-centred axis-aligned box.
FUSE_HOST_DEVICE inline f32 boxSignedDistance(vec3 p, vec3 h) {
    const vec3 q{std::fabs(p.x) - h.x, std::fabs(p.y) - h.y, std::fabs(p.z) - h.z};
    const vec3 outside{std::max(q.x, 0.f), std::max(q.y, 0.f), std::max(q.z, 0.f)};
    return outside.length() + std::min(std::max(q.x, std::max(q.y, q.z)), 0.f);
}

FUSE_HOST_DEVICE inline ContactManifold collideOrientedBoxPlane(
    vec3 boxPos,
    quat boxRotation,
    vec3 boxHalfExtents,
    vec3 planeNormal,
    f32 planeDistance,
    u32 idxBox,
    u32 idxPlane,
    f32 margin) {
    const vec3 extent = orientedBoxHalfExtents(boxRotation, boxHalfExtents);
    const f32 reach = std::fabs(extent.x * planeNormal.x) + std::fabs(extent.y * planeNormal.y) +
                      std::fabs(extent.z * planeNormal.z);
    const f32 centreDistance = boxPos.dot(planeNormal) - planeDistance;
    if (centreDistance > reach + margin) {
        return invalidContactManifold();
    }

    const vec3 ax = rotate(boxRotation, {boxHalfExtents.x, 0.f, 0.f});
    const vec3 ay = rotate(boxRotation, {0.f, boxHalfExtents.y, 0.f});
    const vec3 az = rotate(boxRotation, {0.f, 0.f, boxHalfExtents.z});
    DepthPoint corners[8]{};
    u32 count = 0;
    for (u32 c = 0; c < 8u; ++c) {
        const vec3 corner = boxPos + ax * ((c & 1u) != 0u ? 1.f : -1.f) + ay * ((c & 2u) != 0u ? 1.f : -1.f) +
                            az * ((c & 4u) != 0u ? 1.f : -1.f);
        const f32 distance = corner.dot(planeNormal) - planeDistance;
        if (distance <= margin) {
            corners[count++] = {corner - planeNormal * (0.5f * distance), -distance};
        }
    }
    if (count == 0u) {
        return invalidContactManifold();
    }
    if (count > kMaxContactPointsPerManifold) {
        count = reduceToFour(corners, count, planeNormal, rotate(boxRotation, {0.8f, 0.f, 0.6f}), 1e-3f);
    }

    ContactManifold manifold{};
    manifold.contactNormal = planeNormal;
    manifold.minSeparation = reach;
    manifold.bodyA = idxBox;
    manifold.bodyB = idxPlane;
    manifold.valid = true;
    for (u32 i = 0; i < count; ++i) {
        manifold.addPoint(corners[i].point, corners[i].depth);
    }
    return manifold;
}

FUSE_HOST_DEVICE inline ContactManifold collideOrientedBoxSphere(
    vec3 spherePos,
    f32 sphereRadius,
    vec3 boxPos,
    quat boxRotation,
    vec3 boxHalfExtents,
    u32 idxSphere,
    u32 idxBox,
    f32 margin) {
    if (isIdentity(boxRotation)) {
        return collideBoxSphere(spherePos, sphereRadius, boxPos, boxHalfExtents, idxSphere, idxBox, margin);
    }
    const vec3 local = inverseRotate(boxRotation, spherePos - boxPos);
    ContactManifold manifold = collideBoxSphere(local, sphereRadius, {}, boxHalfExtents, idxSphere, idxBox, margin);
    if (!manifold.valid) {
        return manifold;
    }
    manifold.contactNormal = rotate(boxRotation, manifold.contactNormal);
    for (u32 i = 0; i < manifold.pointCount; ++i) {
        manifold.points[i].point = boxPos + rotate(boxRotation, manifold.points[i].point);
    }
    manifold.syncLegacyFields();
    return manifold;
}

FUSE_HOST_DEVICE inline ContactManifold collideOrientedBoxBox(
    vec3 posA,
    quat rotationA,
    vec3 halfExtentsA,
    vec3 posB,
    quat rotationB,
    vec3 halfExtentsB,
    u32 idxA,
    u32 idxB,
    f32 margin) {
    const Obb boxA = makeObb(posA, rotationA, halfExtentsA);
    const Obb boxB = makeObb(posB, rotationB, halfExtentsB);
    const vec3 d = posA - posB;

    // Penetration along an axis (negative = separated).
    const auto penetrationOn = [&](vec3 axis) {
        return projectedRadius(boxA, axis) + projectedRadius(boxB, axis) - std::fabs(d.dot(axis));
    };

    f32 facePenA = 1e30f;
    u32 faceAxisA = 0;
    for (u32 i = 0; i < 3u; ++i) {
        const f32 pen = penetrationOn(boxA.axis[i]);
        if (pen <= -margin) {
            return invalidContactManifold();
        }
        if (pen < facePenA) {
            facePenA = pen;
            faceAxisA = i;
        }
    }
    f32 facePenB = 1e30f;
    u32 faceAxisB = 0;
    for (u32 j = 0; j < 3u; ++j) {
        const f32 pen = penetrationOn(boxB.axis[j]);
        if (pen <= -margin) {
            return invalidContactManifold();
        }
        if (pen < facePenB) {
            facePenB = pen;
            faceAxisB = j;
        }
    }
    f32 edgePen = 1e30f;
    u32 edgeA = 0;
    u32 edgeB = 0;
    vec3 edgeAxis{};
    for (u32 i = 0; i < 3u; ++i) {
        for (u32 j = 0; j < 3u; ++j) {
            vec3 axis = boxA.axis[i].cross(boxB.axis[j]);
            const f32 length = axis.length();
            if (length < 1e-4f) {
                continue; // parallel edges: covered by the face axes
            }
            axis = axis * (1.f / length);
            const f32 pen = penetrationOn(axis);
            if (pen <= -margin) {
                return invalidContactManifold();
            }
            if (pen < edgePen) {
                edgePen = pen;
                edgeA = i;
                edgeB = j;
                edgeAxis = axis;
            }
        }
    }

    // Prefer faces (stable manifolds for resting contact); prefer A's face over B's. The biases
    // are written on the penetration difference so they also hold for speculative (negative)
    // penetrations.
    const bool referenceIsB = facePenB < facePenA - (0.02f * std::fabs(facePenA) + 1e-4f);
    const f32 facePen = referenceIsB ? facePenB : facePenA;

    ContactManifold manifold{};
    manifold.bodyA = idxA;
    manifold.bodyB = idxB;
    manifold.valid = true;

    if (edgePen < facePen - (0.05f * std::fabs(facePen) + 5e-4f)) {
        vec3 n = edgeAxis;
        if (n.dot(d) < 0.f) {
            n = n * -1.f;
        }
        // Supporting edges: A's edge closest to B (direction -n), B's edge closest to A (+n).
        vec3 centreA = boxA.center;
        vec3 centreB = boxB.center;
        for (u32 k = 0; k < 3u; ++k) {
            if (k != edgeA) {
                centreA += boxA.axis[k] * (boxA.axis[k].dot(n) > 0.f ? -boxA.extent[k] : boxA.extent[k]);
            }
            if (k != edgeB) {
                centreB += boxB.axis[k] * (boxB.axis[k].dot(n) > 0.f ? boxB.extent[k] : -boxB.extent[k]);
            }
        }
        const vec3 halfA = boxA.axis[edgeA] * boxA.extent[edgeA];
        const vec3 halfB = boxB.axis[edgeB] * boxB.extent[edgeB];
        vec3 onA{};
        vec3 onB{};
        closestPointsSegmentSegment(centreA - halfA, centreA + halfA, centreB - halfB, centreB + halfB, onA, onB);
        manifold.contactNormal = n;
        manifold.addPoint((onA + onB) * 0.5f, edgePen);
        return manifold;
    }

    // Face contact: clip the incident face against the reference face's side planes.
    const Obb& reference = referenceIsB ? boxB : boxA;
    const Obb& incident = referenceIsB ? boxA : boxB;
    const u32 refAxis = referenceIsB ? faceAxisB : faceAxisA;
    vec3 n = reference.axis[refAxis];
    if (n.dot(d) < 0.f) {
        n = n * -1.f;
    }
    manifold.contactNormal = n;
    // Outward normal of the reference face, towards the incident box.
    const vec3 refNormal = referenceIsB ? n : n * -1.f;
    const vec3 refCentre = reference.center + refNormal * reference.extent[refAxis];
    const u32 side1 = (refAxis + 1u) % 3u;
    const u32 side2 = (refAxis + 2u) % 3u;

    u32 incAxis = 0;
    f32 bestAlign = -1.f;
    for (u32 k = 0; k < 3u; ++k) {
        const f32 align = std::fabs(incident.axis[k].dot(refNormal));
        if (align > bestAlign) {
            bestAlign = align;
            incAxis = k;
        }
    }
    const vec3 incNormal = incident.axis[incAxis] * (incident.axis[incAxis].dot(refNormal) > 0.f ? -1.f : 1.f);
    const vec3 incCentre = incident.center + incNormal * incident.extent[incAxis];
    const vec3 u = incident.axis[(incAxis + 1u) % 3u] * incident.extent[(incAxis + 1u) % 3u];
    const vec3 v = incident.axis[(incAxis + 2u) % 3u] * incident.extent[(incAxis + 2u) % 3u];

    ClipPolygon polygon{};
    polygon.count = 4;
    polygon.points[0] = incCentre + u + v;
    polygon.points[1] = incCentre - u + v;
    polygon.points[2] = incCentre - u - v;
    polygon.points[3] = incCentre + u - v;
    ClipPolygon scratch{};
    const vec3 s1 = reference.axis[side1];
    const vec3 s2 = reference.axis[side2];
    clipAgainstPlane(polygon, s1, s1.dot(refCentre) + reference.extent[side1], scratch);
    clipAgainstPlane(scratch, s1 * -1.f, -s1.dot(refCentre) + reference.extent[side1], polygon);
    clipAgainstPlane(polygon, s2, s2.dot(refCentre) + reference.extent[side2], scratch);
    clipAgainstPlane(scratch, s2 * -1.f, -s2.dot(refCentre) + reference.extent[side2], polygon);

    DepthPoint points[kMaxClipPoints]{};
    u32 count = 0;
    for (u32 i = 0; i < polygon.count; ++i) {
        const f32 depth = refNormal.dot(refCentre - polygon.points[i]);
        if (depth >= -margin) {
            points[count++] = {polygon.points[i] + refNormal * (0.5f * depth), depth};
        }
    }
    if (count == 0u) {
        // Numerical corner case (clipped polygon slipped past the face): fall back to the centre line.
        manifold.addPoint((boxA.center + boxB.center) * 0.5f, std::min(facePen, edgePen));
        return manifold;
    }
    // Tie-break along a skewed in-face direction: the clipped polygon has whole edges on the
    // reference side planes, so ranking along a side axis ties two points and numerical noise
    // swaps the kept quad from substep to substep (tall yawed stacks then rock).
    count = reduceToFour(points, count, refNormal, s1 * 0.8f + s2 * 0.6f, 1e-3f);
    for (u32 i = 0; i < count; ++i) {
        manifold.addPoint(points[i].point, points[i].depth);
    }
    return manifold;
}

FUSE_HOST_DEVICE inline ContactManifold collideCapsulePlane(
    vec3 capsulePos,
    quat capsuleRotation,
    vec3 capsuleParams,
    vec3 planeNormal,
    f32 planeDistance,
    u32 idxCapsule,
    u32 idxPlane,
    f32 margin) {
    const f32 radius = capsuleParams.x;
    const vec3 half = capsuleHalfAxis(capsuleRotation, capsuleParams.y);
    ContactManifold manifold{};
    manifold.contactNormal = planeNormal;
    manifold.bodyA = idxCapsule;
    manifold.bodyB = idxPlane;
    const vec3 caps[2] = {capsulePos - half, capsulePos + half};
    const u32 capCount = capsuleParams.y > 0.f ? 2u : 1u;
    for (u32 i = 0; i < capCount; ++i) {
        const f32 distance = caps[i].dot(planeNormal) - planeDistance - radius;
        if (distance <= margin) {
            manifold.addPoint(caps[i] - planeNormal * (radius + 0.5f * distance), -distance);
        }
    }
    if (manifold.pointCount == 0u) {
        return invalidContactManifold();
    }
    manifold.minSeparation = radius + std::fabs(half.dot(planeNormal));
    manifold.valid = true;
    return manifold;
}

FUSE_HOST_DEVICE inline ContactManifold collideCapsuleBox(
    vec3 capsulePos,
    quat capsuleRotation,
    vec3 capsuleParams,
    vec3 boxPos,
    quat boxRotation,
    vec3 boxHalfExtents,
    u32 idxCapsule,
    u32 idxBox,
    f32 margin) {
    const f32 radius = capsuleParams.x;
    const vec3 half = capsuleHalfAxis(capsuleRotation, capsuleParams.y);
    // Work in the box frame: the box's signed distance is convex along the axis, so a ternary
    // search finds the deepest axis point.
    const vec3 a = inverseRotate(boxRotation, capsulePos - half - boxPos);
    const vec3 b = inverseRotate(boxRotation, capsulePos + half - boxPos);
    f32 lo = 0.f;
    f32 hi = 1.f;
    for (int i = 0; i < 40 && hi - lo > 1e-5f; ++i) {
        const f32 m1 = lo + (hi - lo) / 3.f;
        const f32 m2 = hi - (hi - lo) / 3.f;
        if (boxSignedDistance(a + (b - a) * m1, boxHalfExtents) <= boxSignedDistance(a + (b - a) * m2, boxHalfExtents)) {
            hi = m2;
        } else {
            lo = m1;
        }
    }
    const vec3 deepest = a + (b - a) * (0.5f * (lo + hi));
    const ContactManifold primary = collideBoxSphere(deepest, radius, {}, boxHalfExtents, idxCapsule, idxBox, margin);
    if (!primary.valid) {
        return invalidContactManifold();
    }
    ContactManifold manifold{};
    manifold.contactNormal = rotate(boxRotation, primary.contactNormal);
    manifold.minSeparation = radius;
    manifold.bodyA = idxCapsule;
    manifold.bodyB = idxBox;
    manifold.valid = true;
    manifold.addPoint(boxPos + rotate(boxRotation, primary.points[0].point), primary.points[0].penetration);
    // Cap centres lying flat against the same face add support points (capsule resting on a box).
    const vec3 caps[2] = {a, b};
    for (const vec3& cap : caps) {
        if ((cap - deepest).dot(cap - deepest) < 1e-8f) {
            continue;
        }
        const ContactManifold capContact = collideBoxSphere(cap, radius, {}, boxHalfExtents, idxCapsule, idxBox, margin);
        if (capContact.valid && capContact.contactNormal.dot(primary.contactNormal) > 0.99f) {
            manifold.addPoint(boxPos + rotate(boxRotation, capContact.points[0].point), capContact.points[0].penetration);
        }
    }
    return manifold;
}

/// Re-labels a manifold computed with the bodies swapped (normal flipped to point B -> A).
FUSE_HOST_DEVICE inline ContactManifold flipped(ContactManifold manifold, u32 bodyA, u32 bodyB) {
    if (!manifold.valid) {
        return invalidContactManifold();
    }
    manifold.contactNormal = manifold.contactNormal * -1.f;
    manifold.bodyA = bodyA;
    manifold.bodyB = bodyB;
    return manifold;
}

} // namespace hd

/// Sphere, box, capsule and plane: the shapes the device narrowphase dispatches.
FUSE_HOST_DEVICE inline bool isPrimitiveShape(CollisionShapeType type) {
    return type == CollisionShapeType::Sphere || type == CollisionShapeType::Box ||
           type == CollisionShapeType::Capsule || type == CollisionShapeType::Plane;
}

/// collideShapes for two primitive shapes (isPrimitiveShape): the same branch order and functions as
/// the CPU dispatch (contact_pair.cpp calls this for primitive pairs). Plane-plane is invalid.
FUSE_HOST_DEVICE inline ContactManifold collidePrimitiveShapes(const ShapeInstance& shapeA, const ShapeInstance& shapeB,
                                                               u32 a, u32 b, f32 margin) {
    const CollisionShapeType typeA = shapeA.type;
    const CollisionShapeType typeB = shapeB.type;
    const vec3 posA = shapeA.position;
    const vec3 posB = shapeB.position;
    const quat rotA = shapeA.orientation;
    const quat rotB = shapeB.orientation;
    const vec3 paramsA = shapeA.params;
    const vec3 paramsB = shapeB.params;

    using T = CollisionShapeType;
    const auto is = [&](T first, T second) { return typeA == first && typeB == second; };

    if (is(T::Sphere, T::Sphere)) {
        return collideSphereSphere(posA, paramsA.x, posB, paramsB.x, a, b, margin);
    }
    if (is(T::Sphere, T::Plane)) {
        return collideSpherePlane(posA, paramsA.x, paramsB, shapeB.scalar, a, b, margin);
    }
    if (is(T::Plane, T::Sphere)) {
        return collideSpherePlane(posB, paramsB.x, paramsA, shapeA.scalar, b, a, margin);
    }
    if (is(T::Box, T::Plane)) {
        return hd::collideOrientedBoxPlane(posA, rotA, paramsA, paramsB, shapeB.scalar, a, b, margin);
    }
    if (is(T::Plane, T::Box)) {
        return hd::collideOrientedBoxPlane(posB, rotB, paramsB, paramsA, shapeA.scalar, b, a, margin);
    }
    if (is(T::Sphere, T::Box)) {
        return hd::collideOrientedBoxSphere(posA, paramsA.x, posB, rotB, paramsB, a, b, margin);
    }
    if (is(T::Box, T::Sphere)) {
        return hd::flipped(hd::collideOrientedBoxSphere(posB, paramsB.x, posA, rotA, paramsA, b, a, margin), a, b);
    }
    if (is(T::Box, T::Box)) {
        if (isIdentity(rotA) && isIdentity(rotB)) {
            return hd::collideBoxBox(posA, paramsA, posB, paramsB, a, b, margin);
        }
        return hd::collideOrientedBoxBox(posA, rotA, paramsA, posB, rotB, paramsB, a, b, margin);
    }

    // Capsules: segment along the body's local Y axis.
    if (is(T::Sphere, T::Capsule)) {
        if (isIdentity(rotB)) {
            return collideCapsuleSphere(posA, paramsA.x, posB, paramsB, a, b, margin);
        }
        const vec3 half = capsuleHalfAxis(rotB, paramsB.y);
        return collideCapsuleSegments(posA, posA, paramsA.x, posA, posB - half, posB + half, paramsB.x, posB, a, b,
                                      margin);
    }
    if (is(T::Capsule, T::Sphere)) {
        if (isIdentity(rotA)) {
            return hd::flipped(collideCapsuleSphere(posB, paramsB.x, posA, paramsA, b, a, margin), a, b);
        }
        const vec3 half = capsuleHalfAxis(rotA, paramsA.y);
        return collideCapsuleSegments(posA - half, posA + half, paramsA.x, posA, posB, posB, paramsB.x, posB, a, b,
                                      margin);
    }
    if (is(T::Capsule, T::Capsule)) {
        const vec3 halfA = capsuleHalfAxis(rotA, paramsA.y);
        const vec3 halfB = capsuleHalfAxis(rotB, paramsB.y);
        return collideCapsuleSegments(posA - halfA, posA + halfA, paramsA.x, posA, posB - halfB, posB + halfB,
                                      paramsB.x, posB, a, b, margin);
    }
    if (is(T::Capsule, T::Plane)) {
        return hd::collideCapsulePlane(posA, rotA, paramsA, paramsB, shapeB.scalar, a, b, margin);
    }
    if (is(T::Plane, T::Capsule)) {
        return hd::flipped(hd::collideCapsulePlane(posB, rotB, paramsB, paramsA, shapeA.scalar, b, a, margin), a, b);
    }
    if (is(T::Capsule, T::Box)) {
        return hd::collideCapsuleBox(posA, rotA, paramsA, posB, rotB, paramsB, a, b, margin);
    }
    if (is(T::Box, T::Capsule)) {
        return hd::flipped(hd::collideCapsuleBox(posB, rotB, paramsB, posA, rotA, paramsA, b, a, margin), a, b);
    }
    return invalidContactManifold(); // plane-plane
}

} // namespace fuse::physics::narrowphase
