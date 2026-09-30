#pragma once

// Geometric queries against single shapes (GAP-PHYS-CHARACTER): ray casts for every collision shape and
// translational shape casts (sweeps) of a sphere, capsule or box against every shape, including convex
// hulls, triangle meshes, voxel volumes and SDFs. PhysicsManager::rayCast / shapeCast run these per body.
//
// Shape casts use conservative advancement on the GJK distance (with its lower bound) between the
// caster's core (point / segment / box) and each convex target piece (primitive, hull, triangle, voxel
// box), stepping to the separating plane: the step never overshoots, so a cast cannot tunnel through
// thin geometry whatever its length. The TOI against a concave shape is the minimum over its convex pieces
// (triangles in the swept bounds from the mesh BVH, surface voxels from the SVO). SDF targets are sphere
// traced (exact for spheres; capsules and boxes use the field at sampled surface points).

#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/types.hpp>

namespace fuse::physics {

/// Support-mapped convex core with a rounding radius (a sphere is a point core, a capsule a segment).
struct ConvexCore {
    enum class Kind : u8 { Point, Segment, Triangle, Box, Hull };
    Kind kind = Kind::Point;
    vec3 p0{};
    vec3 p1{};
    vec3 p2{};
    /// Box: centre, orientation, half extents. Hull: position, orientation, body-frame vertices.
    vec3 position{};
    quat orientation{};
    vec3 halfExtents{};
    const vec3* vertices = nullptr;
    u32 vertexCount = 0;
    f32 radius = 0.f;

    [[nodiscard]] vec3 support(vec3 direction) const;
    /// Translated copy.
    [[nodiscard]] ConvexCore moved(vec3 offset) const;
};

struct GjkDistanceResult {
    /// Distance between the cores (0 when they overlap) and its certified lower bound.
    f32 distance = 0.f;
    f32 lowerBound = 0.f;
    vec3 pointA{};
    vec3 pointB{};
    /// Unit separating direction (from B towards A): the final GJK vector. Unreliable at ~zero distance.
    vec3 direction{0.f, 1.f, 0.f};
    bool overlap = false;
    u32 iterations = 0;
};

/// GJK closest points between two convex cores (radii ignored).
GjkDistanceResult gjkDistance(const ConvexCore& a, const ConvexCore& b);

/// Core of a sphere / capsule / box / hull shape instance (false for planes and concave shapes).
bool convexCoreOf(const narrowphase::ShapeInstance& shape, ConvexCore& out);

struct RayHit {
    f32 t = 0.f;
    vec3 normal{};
};

/// Nearest hit of the ray (unit `direction`) on the shape, t in [0, maxT]. A ray starting inside a solid
/// shape hits at t = 0.
bool rayCastShape(const narrowphase::ShapeInstance& shape, vec3 origin, vec3 direction, f32 maxT, RayHit& hit);

struct ShapeCastHit {
    /// Distance travelled along the (unit) direction at first contact.
    f32 distance = 0.f;
    /// Contact normal, from the target towards the caster.
    vec3 normal{};
    /// Contact point on the target's surface.
    vec3 point{};
    /// The caster already overlapped the target at its start pose (distance 0; the normal is the
    /// penetration direction).
    bool startPenetrating = false;
    /// Penetration depth when startPenetrating.
    f32 penetration = 0.f;
};

struct ShapeCastOptions {
    /// Contact is reported once the gap is below this.
    f32 tolerance = 1e-4f;
    u32 maxIterations = 64;
};

/// Sweeps `caster` (sphere, capsule or box at its pose) along unit `direction` by up to `maxDistance`
/// against `target` (any shape). Translation only.
bool shapeCast(const narrowphase::ShapeInstance& caster, vec3 direction, f32 maxDistance,
               const narrowphase::ShapeInstance& target, ShapeCastHit& hit, const ShapeCastOptions& options = {});

/// World bounds of a shape instance (planes: a huge box).
aabb shapeInstanceBounds(const narrowphase::ShapeInstance& shape);

} // namespace fuse::physics
