#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/friction.hpp>
#include <fuse/types.hpp>

namespace fuse::physics::narrowphase {

constexpr u32 kMaxContactPointsPerManifold = 4u;

/// Const preflight summary for manifold prune steps (B4.3 deepen pass).
struct ManifoldPrunePreflight {
    u32 separatedCount = 0u;
    u32 duplicatePairs = 0u;
    bool excessPoints = false;

    bool should_prune() const {
        return separatedCount > 0u || duplicatePairs > 0u || excessPoints;
    }
};

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

    /// Returns true when at least one point has penetration above `-epsilon` (B4.3 deepen pass).
    bool hasPenetratingPoints(f32 epsilon = 1e-6f) const;

    /// Count contact points with penetration above `-epsilon` (B4.3 deepen pass).
    u32 countPenetratingPoints(f32 epsilon = 1e-6f) const;

    /// Returns true when any prune step would remove points (B4.3 deepen pass).
    bool needsPruning(
        f32 separationEpsilon = 1e-6f,
        f32 duplicateEpsilon = 1e-4f) const;

    /// Returns true when at least one point is separated below `-epsilon` (B4.3 deepen pass).
    bool hasSeparatedPoints(f32 epsilon = 1e-6f) const;

    /// Returns true when two slots share the same position within `positionEpsilon` (B4.3 deepen pass).
    bool hasDuplicatePoints(f32 positionEpsilon = 1e-4f) const;

    /// Const preflight: true when `pruneContactPoints` would leave no penetrating slots (B4.3 deepen pass).
    bool wouldBeEmptyAfterPrune(
        f32 separationEpsilon = 1e-6f,
        f32 duplicateEpsilon = 1e-4f) const;

    /// Clears `valid` when the manifold has no contact points (B4.3 deepen pass).
    void invalidateIfEmpty();

    /// Count contact points separated below `-epsilon` (B4.3 deepen pass).
    u32 countSeparatedPoints(f32 epsilon = 1e-6f) const;

    /// Returns true when at least one penetrating point is shallower than `minDepth` (B4.3 deepen pass).
    bool hasShallowPenetrations(f32 minDepth) const;

    /// Returns true when `pointCount` exceeds `kMaxContactPointsPerManifold` (B4.3 deepen pass).
    bool hasExcessContactPoints() const;

    /// Build a prune preflight summary without mutating slots (B4.3 deepen pass).
    ManifoldPrunePreflight prunePreflight(
        f32 separationEpsilon = 1e-6f,
        f32 duplicateEpsilon = 1e-4f) const;

    /// Prune only when `needsPruning` is true; returns whether any points were removed (B4.3 deepen pass).
    bool pruneContactPointsIfNeeded(
        f32 separationEpsilon = 1e-6f,
        f32 duplicateEpsilon = 1e-4f);

    /// Drop separated contact points with penetration below `-epsilon` (B4.3 deepen).
    void pruneNonPenetratingPoints(f32 epsilon = 1e-6f);

    /// Drop penetrating points shallower than `minDepth` (B4.3 deepen pass).
    void pruneShallowPenetrations(f32 minDepth);

    /// Keep at most `maxPoints` deepest-penetrating slots (B4.3 deepen).
    void pruneToMaxPoints(u32 maxPoints);

    /// Merge contact points within `positionEpsilon` of an existing slot (B4.3 deepen).
    void pruneDuplicatePoints(f32 positionEpsilon = 1e-4f);

    /// Run non-penetrating, duplicate, and max-point pruning in order (B4.3 deepen).
    void pruneContactPoints(
        f32 separationEpsilon = 1e-6f,
        f32 duplicateEpsilon = 1e-4f);

    /// Prune and return true when the manifold still has points (B4.3 deepen pass).
    bool pruneIfEmpty(
        f32 separationEpsilon = 1e-6f,
        f32 duplicateEpsilon = 1e-4f);
};

inline ContactManifold invalidContactManifold() {
    return ContactManifold();
}

} // namespace fuse::physics::narrowphase
