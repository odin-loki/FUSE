#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/friction.hpp>
#include <fuse/types.hpp>

namespace fuse::physics::narrowphase {

constexpr u32 kMaxContactPointsPerManifold = 4u;

struct ContactPoint {
    vec3 point{};
    f32 penetration = 0.f;

    /// Per-point tangent frame sharing the manifold contact normal (B4.3 deepen).
    TangentBasis tangent_basis(vec3 contactNormal) const;
};

using ContactPointSlot = ContactPoint;

/// Narrowphase contact manifold with multi-point slots and warm-start impulse stubs (B4.3 deepen).
struct ContactManifold {
    ContactManifold() = default;

    vec3 contactNormal{};
    f32 minSeparation = 0.f;
    u32 bodyA = 0;
    u32 bodyB = 0;
    u32 pointCount = 0;
    ContactPointSlot points[kMaxContactPointsPerManifold]{};
    f32 warmNormalImpulse = 0.f;
    vec2 warmTangentImpulse{};
    TangentBasis frictionBasis{};

    // Legacy single-point mirror used by PBD solver and existing tests.
    vec3 contactPoint{};
    f32 penetrationDepth = 0.f;
    bool valid = false;

    void reset();
    void clear();
    void addPoint(vec3 point, f32 penetration);
    void syncLegacyFields();
    void buildFrictionBasis();

    bool empty() const { return pointCount == 0u; }
    bool hasValidNormal(f32 epsilon = 1e-6f) const;
    bool hasFrictionBasis() const;
    const ContactPoint& pointAt(u32 index) const;
    f32 maxPenetration() const;

    /// Count contact points with penetration >= `-epsilon` (touching or penetrating) (B4.3 deepen).
    u32 penetratingPointCount(f32 epsilon = 1e-6f) const;

    /// Count contact points with penetration < `-epsilon` (separated) (B4.3 deepen).
    u32 separatedPointCount(f32 epsilon = 1e-6f) const;

    /// True when any point has penetration strictly above `epsilon` (B4.3 deepen).
    bool hasPenetratingPoints(f32 epsilon = 1e-6f) const;

    /// Clear cached friction basis so the next tangent build recomputes (B4.3 deepen).
    void invalidateFrictionBasis();

    /// Drop separated contact points with penetration below `-epsilon` (B4.3 deepen).
    void pruneNonPenetratingPoints(f32 epsilon = 1e-6f);

    /// Keep at most `maxPoints` deepest-penetrating slots (B4.3 deepen).
    void pruneToMaxPoints(u32 maxPoints);

    /// Merge contact points within `positionEpsilon` of an existing slot (B4.3 deepen).
    void pruneDuplicatePoints(f32 positionEpsilon = 1e-4f);

    /// Run non-penetrating, duplicate, and max-point pruning in order (B4.3 deepen).
    void pruneContactPoints(
        f32 separationEpsilon = 1e-6f,
        f32 duplicateEpsilon = 1e-4f);

    /// Prune contact points; returns false when the manifold is empty afterward (B4.3 deepen).
    bool pruneIfEmpty(
        f32 separationEpsilon = 1e-6f,
        f32 duplicateEpsilon = 1e-4f);
};

inline ContactManifold invalidContactManifold() {
    return ContactManifold();
}

} // namespace fuse::physics::narrowphase
