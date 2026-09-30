#pragma once

// Shapes, contact manifolds and per-shape geometry for the 2D rigid-body world (package G11).
// Manifold generation follows the reference-face clipping scheme used by Box2D: SAT picks the axis of
// least penetration, the incident edge is clipped against the reference face's side planes, and each
// point carries a feature id (which vertex / face produced it) so impulses can be warm-started across
// steps. Chain edges are one-sided and use their neighbours (ghost vertices) to reject normals that
// belong to the adjacent edge, which removes internal-vertex snagging.

#include "math_2d.hpp"

#include <fuse/physics/physics_world_2d.hpp>

namespace fuse::physics::p2d {

struct Shape2D {
    ShapeType2D type = ShapeType2D::Circle;
    bool alive = false;
    bool isSensor = false;
    /// Chain edge: collides on the right-hand side only and has ghost vertices.
    bool oneSided = false;
    u32 body = kInvalidId2D;
    /// Next shape on the same body (intrusive list), kInvalidId2D terminates.
    u32 nextOnBody = kInvalidId2D;
    /// Circle radius, or the collision skin of polygons / edges.
    f32 radius = 0.f;
    /// Circle centre or polygon centroid, body-local.
    vec2 center{0.f, 0.f};
    vec2 vertices[kMaxPolygonVertices2D]{};
    vec2 normals[kMaxPolygonVertices2D]{};
    u32 count = 0u;
    /// Chain neighbours: vertex0 precedes vertices[0], vertex3 follows vertices[1].
    vec2 vertex0{0.f, 0.f};
    vec2 vertex3{0.f, 0.f};
    f32 density = 1.f;
    f32 friction = 0.6f;
    f32 restitution = 0.f;
    Filter2D filter{};
    u64 userData = 0u;
    Aabb2D aabb{};
    Aabb2D fatAabb{};
};

/// Feature id packing: indexA | indexB << 8 | typeA << 16 | typeB << 24 (type 0 = vertex, 1 = face).
inline constexpr u8 kFeatureVertex = 0u;
inline constexpr u8 kFeatureFace = 1u;
struct ContactFeature {
    u8 indexA = 0u;
    u8 indexB = 0u;
    u8 typeA = 0u;
    u8 typeB = 0u;
    u32 key() const {
        return static_cast<u32>(indexA) | (static_cast<u32>(indexB) << 8u) | (static_cast<u32>(typeA) << 16u) |
               (static_cast<u32>(typeB) << 24u);
    }
};

struct ManifoldPoint {
    /// Circles / FaceA: point on B in B's frame; FaceB: point on A in A's frame.
    vec2 localPoint{0.f, 0.f};
    f32 normalImpulse = 0.f;
    f32 tangentImpulse = 0.f;
    u32 id = 0u;
};

enum class ManifoldType : u8 { Circles, FaceA, FaceB };

struct Manifold {
    ManifoldPoint points[2]{};
    vec2 localNormal{0.f, 0.f};
    vec2 localPoint{0.f, 0.f};
    ManifoldType type = ManifoldType::Circles;
    u32 pointCount = 0u;
};

struct WorldManifold {
    vec2 normal{0.f, 0.f};
    vec2 points[2]{};
    f32 separations[2]{};
};

void computeWorldManifold(const Manifold& manifold, const Xf& xfA, f32 radiusA, const Xf& xfB, f32 radiusB,
                          WorldManifold& out);

/// True when the pair has a collision routine (edge-edge has none).
bool canCollide(ShapeType2D a, ShapeType2D b);
/// Collision order for the pair: returns true when (a, b) must be swapped so that the routine's
/// "A" shape comes first (polygon before circle, edge before circle / polygon).
bool collideOrderSwapped(ShapeType2D a, ShapeType2D b);
/// Manifold between two shapes already in collision order.
void collide(const Shape2D& a, const Xf& xfA, const Shape2D& b, const Xf& xfB, Manifold& out);

// Individual routines (exposed for the tests of the collision layer).
void collideCircles(const Shape2D& a, const Xf& xfA, const Shape2D& b, const Xf& xfB, Manifold& out);
void collidePolygonAndCircle(const Shape2D& a, const Xf& xfA, const Shape2D& b, const Xf& xfB, Manifold& out);
void collidePolygons(const Shape2D& a, const Xf& xfA, const Shape2D& b, const Xf& xfB, Manifold& out);
void collideEdgeAndCircle(const Shape2D& a, const Xf& xfA, const Shape2D& b, const Xf& xfB, Manifold& out);
void collideEdgeAndPolygon(const Shape2D& a, const Xf& xfA, const Shape2D& b, const Xf& xfB, Manifold& out);

/// Exact world AABB of the shape (includes the skin radius).
Aabb2D computeAabb(const Shape2D& shape, const Xf& xf);
/// Ray p1 -> p2 (world) against the shape; fraction in [0, maxFraction].
bool rayCast(const Shape2D& shape, const Xf& xf, vec2 p1, vec2 p2, f32 maxFraction, f32& outFraction,
             vec2& outNormal);
bool testPoint(const Shape2D& shape, const Xf& xf, vec2 p);

struct MassData {
    f32 mass = 0.f;
    vec2 center{0.f, 0.f};
    /// Rotational inertia about the body origin.
    f32 inertia = 0.f;
};
MassData computeMass(const Shape2D& shape);

/// Builds a convex polygon (CCW, outward normals, centroid) from arbitrary points. False when the
/// points do not span a polygon with positive area or need more than kMaxPolygonVertices2D vertices.
bool buildPolygon(Shape2D& shape, const vec2* points, u32 count);
void buildEdge(Shape2D& shape, vec2 v1, vec2 v2);

} // namespace fuse::physics::p2d
