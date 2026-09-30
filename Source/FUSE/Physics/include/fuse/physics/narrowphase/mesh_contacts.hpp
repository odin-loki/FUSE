#pragma once

// Convex shapes vs a static triangle mesh (GAP-PHYS-HULL-MESH part 2). The convex shape is moved into the
// mesh frame, its bounds (plus margin) query the mesh BVH, and every overlapping triangle contributes
// contacts:
//   sphere / capsule   closest features (Voronoi region of the point / segment on the triangle);
//   box / hull         separating-axis test against the triangle (triangle normal, hull face normals, every
//                      hull-edge x triangle-edge axis) with face clipping.
// Internal-edge fix: a contact on an edge the mesh marked as internal (shared with a coplanar or concave
// neighbour, TriMesh::edgeFlags) takes the triangle's face normal, so bodies slide across seams without
// catching. The per-triangle contacts are merged per body pair (ContactClusterer): one manifold per normal
// cluster, deepest first.

#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/contact_cluster.hpp>
#include <fuse/physics/narrowphase/hull_contacts.hpp>
#include <fuse/physics/shapes/tri_mesh.hpp>
#include <fuse/types.hpp>

namespace fuse::physics::narrowphase {

struct MeshPose {
    const TriMesh* mesh = nullptr;
    vec3 position{};
    quat orientation{};
};

/// Sphere / box / capsule (A) vs mesh (B). Returns the manifold count written to `out`.
u32 collideConvexMesh(const ShapeInstance& convex, const MeshPose& mesh, u32 idxA, u32 idxB, f32 margin,
                      ContactManifold* out, u32 maxOut);

/// Convex hull (A) vs mesh (B).
u32 collideHullMesh(const HullPose& hull, const MeshPose& mesh, u32 idxA, u32 idxB, f32 margin,
                    ContactManifold* out, u32 maxOut);

/// Closest point on triangle abc to p (Ericson 5.1.5). `region` receives the feature: 0..2 vertex a/b/c,
/// 3..5 edge ab/bc/ca, 6 face interior.
vec3 closestPointOnTriangle(vec3 p, vec3 a, vec3 b, vec3 c, u32* region = nullptr);

/// Closest points between segment [p0, p1] and triangle abc; returns the squared distance.
f32 closestPointsSegmentTriangle(vec3 p0, vec3 p1, vec3 a, vec3 b, vec3 c, vec3& onSegment, vec3& onTriangle);

} // namespace fuse::physics::narrowphase
