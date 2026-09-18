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

    /// True when no separated slots would be removed by `pruneNonPenetratingPoints` (B4.4 deepen pass).
    bool canSkipPruneNonPenetrating(f32 epsilon = 1e-6f) const;

    /// True when no duplicate slots would be merged by `pruneDuplicatePoints` (B4.4 deepen pass).
    bool canSkipPruneDuplicates(f32 positionEpsilon = 1e-4f) const;

    /// True when point count is already within `maxPoints` (B4.4 deepen pass).
    bool canSkipPruneToMaxPoints(u32 maxPoints = kMaxContactPointsPerManifold) const;

    /// True when `pruneContactPoints` would be a no-op (B4.4 deepen pass).
    bool canSkipPruneContactPoints(
        f32 separationEpsilon = 1e-6f,
        f32 duplicateEpsilon = 1e-4f) const;

    /// Returns true when at least one penetrating point is shallower than `minDepth` (B4.4 deepen pass).
    bool hasShallowPenetrations(f32 minDepth) const;

    /// Prune only when `needsPruning`; returns true when points remain (B4.4 deepen pass).
    bool pruneContactPointsIfNeeded(
        f32 separationEpsilon = 1e-6f,
        f32 duplicateEpsilon = 1e-4f);

    /// True when no shallow slots would be removed by `pruneShallowPenetrations` (B4.4 deepen follow-up).
    bool canSkipPruneShallowPenetrations(f32 minDepth) const;

    /// Prune shallow slots only when `hasShallowPenetrations`; returns true when points remain (B4.4 deepen follow-up).
    bool pruneShallowPenetrationsIfNeeded(f32 minDepth);
};

/// Const preflight for manifold prune dispatch (B4.4 deepen pass).
struct ManifoldPrunePreflight {
    bool hasSeparated = false;
    bool hasDuplicates = false;
    bool hasShallow = false;
    bool exceedsMaxPoints = false;
    bool wouldBeEmpty = false;
    bool skipped = false;

    bool needs_pruning() const {
        return !skipped && (hasSeparated || hasDuplicates || exceedsMaxPoints);
    }

    bool needs_shallow_pruning(f32 minDepth) const { return !skipped && hasShallow; }

    bool can_prune_in_place() const { return needs_pruning() && !wouldBeEmpty; }
};

/// Populate prune preflight from a manifold without mutating slots (B4.4 deepen pass).
ManifoldPrunePreflight preflight_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 shallowMinDepth = 0.f);

/// Const preflight for manifold finalize dispatch (B4.4 deepen follow-up).
struct ManifoldFinalizePreflight {
    bool skipped = false;
    bool canFinalize = false;
    bool needsPruning = false;
    bool wouldBeEmptyAfterPrune = false;
    bool needsFrictionBasis = false;
    bool canReuseFrictionBasis = false;

    bool can_finalize() const { return !skipped && canFinalize; }
};

/// Populate finalize preflight without mutating the manifold (B4.4 deepen follow-up).
ManifoldFinalizePreflight preflight_manifold_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 frictionEpsilon = 1e-4f);

/// Returns true when finalize should be skipped for this manifold (B4.4 deepen follow-up).
bool can_skip_manifold_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 frictionEpsilon = 1e-4f);

inline ContactManifold invalidContactManifold() {
    return ContactManifold();
}

} // namespace fuse::physics::narrowphase
