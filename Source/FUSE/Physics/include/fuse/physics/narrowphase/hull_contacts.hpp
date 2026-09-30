#pragma once

// Convex hull contacts (GAP-PHYS-HULL-MESH part 1). CPU only: hull pairs never reach the resident CUDA
// narrowphase (ResidentPhysics::uploadScene refuses scenes with pooled shapes, so those scenes run the
// CPU pipeline; the primitive pairs keep their FUSE_HOST_DEVICE path and are untouched).
//
//   hull - hull   separating-axis test: face normals of both hulls and the edge pairs whose Gauss-map arcs
//                 intersect (Gregorius, "The Separating Axis Test between Convex Polyhedra", GDC 2013);
//                 face contacts clip the incident face against the reference face's side planes (up to four
//                 points kept, deepest first, largest area), edge contacts give the closest edge points.
//   hull - box    the box as an allocation-free BoxHull, then hull - hull.
//   hull - plane  every vertex below the plane (plus margin), reduced to four.
//   sphere - hull closest point on the solid hull (face / edge / vertex regions), or the least-deep face.
//   capsule - hull  exact segment-hull distance while the axis is outside, SAT (face normals and segment x
//                 edge axes) once it is inside; a capsule lying on a face gets both clipped cap points.
//
// Conventions match the primitive pairs: the normal points from B towards A, points sit midway between
// the surfaces for polytope pairs and on the rounded shape's surface for sphere / capsule pairs,
// speculative contacts within `margin` carry negative penetration.

#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/shapes/convex_hull.hpp>
#include <fuse/types.hpp>

namespace fuse::physics::narrowphase {

/// A hull at a pose (body frame -> world: rotate then translate).
struct HullPose {
    HullView hull{};
    vec3 position{};
    quat orientation{};
};

enum class HullSatFeature : u8 {
    FaceA = 0,
    FaceB = 1,
    Edge = 2,
};

struct HullSatResult {
    f32 faceSeparationA = 0.f;
    f32 faceSeparationB = 0.f;
    f32 edgeSeparation = 0.f;
    u32 faceA = 0;
    u32 faceB = 0;
    u32 edgeA = 0;
    u32 edgeB = 0;
    /// Edge axis (unit, pointing out of A towards B); zero when no edge pair qualified.
    vec3 edgeAxis{};

    [[nodiscard]] f32 separation() const;
};

/// Face queries of both hulls plus the Gauss-map pruned edge query (separation > 0: disjoint).
HullSatResult hullSatQuery(const HullPose& a, const HullPose& b);

/// Reference: every face normal of both hulls and every edge-pair cross product (no pruning).
/// Returns the largest separation; `axis` (optional) receives it, pointing from A towards B.
f32 hullSatBruteForce(const HullPose& a, const HullPose& b, vec3* axis = nullptr);

ContactManifold collideHullHull(const HullPose& a, const HullPose& b, u32 idxA, u32 idxB, f32 margin = 0.f);

/// Hull (A) vs plane (B, normal . x = distance).
ContactManifold collideHullPlane(const HullPose& hull, vec3 planeNormal, f32 planeDistance, u32 idxHull,
                                 u32 idxPlane, f32 margin = 0.f);

/// Sphere (A) vs hull (B).
ContactManifold collideSphereHull(vec3 spherePos, f32 sphereRadius, const HullPose& hull, u32 idxSphere,
                                  u32 idxHull, f32 margin = 0.f);

/// Capsule (A: local Y axis, params.x radius, params.y half height) vs hull (B).
ContactManifold collideCapsuleHull(vec3 capsulePos, quat capsuleRotation, vec3 capsuleParams, const HullPose& hull,
                                   u32 idxCapsule, u32 idxHull, f32 margin = 0.f);

/// Closest points between segment [p0, p1] and the solid hull (hull body frame). Returns the distance
/// (0 when the segment touches or enters the hull); `onSegment` / `onHull` receive the points.
f32 segmentHullDistance(const HullView& hull, vec3 p0, vec3 p1, vec3& onSegment, vec3& onHull);

} // namespace fuse::physics::narrowphase
