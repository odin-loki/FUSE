#pragma once

// Convex hull collision shape (GAP-PHYS-HULL-MESH part 1): polygonal faces with outward planes, unique
// edges with their two adjacent faces (the Gauss-map arcs the SAT edge query prunes with), bounds and
// mass properties. Vertices are in the body frame (the body position is the frame origin). Hulls live in
// the shared shape pool (shapes/shape_pool.hpp); CollisionShapeSoA references them by index.
//
// `buildConvexHull` is the quickhull builder (double precision, tolerance-based visibility, coplanar
// triangles merged into polygons, vertices left in the middle of an edge removed). It allocates and is
// meant for load / cook time (Tools/FUSE/Cook collision cook, runtime asset loads), never per frame.

#include <fuse/physics/math.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::physics {

/// One polygonal hull face: `indexCount` vertex indices starting at `firstIndex` in
/// ConvexHull::faceIndices, counter-clockwise seen from outside. Plane: normal . x = offset.
struct HullFace {
    u32 firstIndex = 0;
    u32 indexCount = 0;
    vec3 normal{};
    f32 offset = 0.f;
};

/// A unique hull edge. `face0` runs v0 -> v1 in its loop, `face1` runs v1 -> v0.
struct HullEdge {
    u32 v0 = 0;
    u32 v1 = 0;
    u32 face0 = 0;
    u32 face1 = 0;
};

/// Non-owning view of a hull's topology (a pooled ConvexHull or a stack BoxHull).
struct HullView {
    const vec3* vertices = nullptr;
    u32 vertexCount = 0;
    const HullFace* faces = nullptr;
    u32 faceCount = 0;
    const u32* faceIndices = nullptr;
    const HullEdge* edges = nullptr;
    u32 edgeCount = 0;
    vec3 centroid{};

    [[nodiscard]] bool empty() const { return faceCount == 0u || vertexCount == 0u; }
};

struct ConvexHull {
    std::vector<vec3> vertices;
    std::vector<u32> faceIndices;
    std::vector<HullFace> faces;
    std::vector<HullEdge> edges;
    /// Centre of volume and volume (body frame).
    vec3 centroid{};
    f32 volume = 0.f;
    vec3 boundsMin{};
    vec3 boundsMax{};
    /// Largest |coordinate| per axis: the origin-centred box that contains the hull. This is what
    /// CollisionShapeSoA::params holds for a hull (broadphase bounds, inertia).
    vec3 halfExtents{};
    /// Largest vertex distance from the origin.
    f32 radius = 0.f;

    [[nodiscard]] bool empty() const { return faces.empty(); }
    [[nodiscard]] HullView view() const;
};

struct QuickHullOptions {
    /// Points closer than this to the current hull are treated as inside. 0: automatic
    /// (1e-5 of the point cloud extent).
    f32 distanceTolerance = 0.f;
};

/// Quickhull over `count` points. False (with `error`) for fewer than four points or a flat /
/// collinear cloud. Deterministic: the same points always give the same hull.
bool buildConvexHull(const vec3* points, u32 count, ConvexHull& out, const QuickHullOptions& options = {},
                     std::string* error = nullptr);
inline bool buildConvexHull(const std::vector<vec3>& points, ConvexHull& out, const QuickHullOptions& options = {},
                            std::string* error = nullptr) {
    return buildConvexHull(points.data(), static_cast<u32>(points.size()), out, options, error);
}

/// Fills planes (Newell normals, outermost offsets), edges, bounds, centroid and volume from
/// `vertices`, `faceIndices` and each face's firstIndex / indexCount. False when the faces do not
/// form a closed two-manifold (every directed edge matched by its reverse exactly once).
bool finalizeConvexHull(ConvexHull& hull, std::string* error = nullptr);

/// Origin-centred box as a hull (8 vertices, bit 0/1/2 of the index = +x/+y/+z corner).
void makeBoxHull(vec3 halfExtents, ConvexHull& out);

/// Allocation-free box hull (same vertex / face / edge layout as makeBoxHull) used when a Box
/// meets a hull.
struct BoxHull {
    vec3 vertices[8]{};
    HullFace faces[6]{};
    u32 faceIndices[24]{};
    HullEdge edges[12]{};

    [[nodiscard]] HullView view() const;
};
void initBoxHull(BoxHull& box, vec3 halfExtents);

/// Largest face-plane distance of `p` (negative inside); `face` receives that face.
f32 hullMaxFaceDistance(const HullView& hull, vec3 p, u32* face = nullptr);

/// Closest point of the (solid) hull to `p` (p itself when inside).
vec3 hullClosestPoint(const HullView& hull, vec3 p);

/// Support point (vertex) along `direction`.
vec3 hullSupport(const HullView& hull, vec3 direction);

/// Diagonal body-frame inverse inertia of the solid hull (uniform density) about its centroid,
/// projected on the body axes. Used by tests / tools; the solver uses the bounding box of `params`.
vec3 hullInverseInertiaDiagonal(const ConvexHull& hull, f32 invMass);

} // namespace fuse::physics
