#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/friction.hpp>
#include <fuse/types.hpp>

namespace fuse::physics::narrowphase {

constexpr u32 kMaxContactPointsPerManifold = 4u;

/// Const preflight for manifold prune dispatch (B4.4 deepen pass).
struct ManifoldPrunePreflight {
    bool hasSeparated = false;
    bool hasDuplicates = false;
    bool exceedsMaxPoints = false;
    bool hasShallow = false;
    bool wouldBeEmpty = false;
    bool skipped = false;

    bool needs_pruning() const {
        return !skipped && (hasSeparated || hasDuplicates || exceedsMaxPoints || hasShallow);
    }

    bool can_prune_in_place() const { return needs_pruning() && !wouldBeEmpty; }

    /// True when no prune step would mutate slots (B4.5 deepen pass).
    bool can_skip() const { return skipped || !needs_pruning(); }
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
    bool hasNonUnitNormal(f32 lengthEpsilon = 1e-4f) const;
    bool needsNormalNormalization(f32 lengthEpsilon = 1e-4f) const;
    /// Returns true when `contactNormal` is approximately unit length (B4.4 deepen follow-up pass).
    bool hasUnitNormal(f32 epsilon = 1e-4f) const;
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

    /// Clears `valid` when the manifold has no contact points (B4.3 deepen pass).
    void invalidateIfEmpty();
    /// Count contact points with penetration >= `-epsilon` (touching or penetrating) (B4.3 deepen).
    u32 penetratingPointCount(f32 epsilon = 1e-6f) const;

    /// Count contact points with penetration < `-epsilon` (separated) (B4.3 deepen).
    u32 separatedPointCount(f32 epsilon = 1e-6f) const;

    /// True when any point has penetration strictly above `epsilon` (B4.3 deepen).

    /// Clear cached friction basis so the next tangent build recomputes (B4.3 deepen).
    void invalidateFrictionBasis();

    /// Returns true when at least one penetrating point is shallower than `minDepth` (B4.3 deepen pass 2).
    bool hasShallowPenetrations(f32 minDepth) const;

    /// Count contact points separated below `-epsilon` (B4.3 deepen pass 2).
    u32 countSeparatedPoints(f32 epsilon = 1e-6f) const;

    /// Run `pruneContactPoints` only when `needsPruning` is true (B4.3 deepen pass 2).
    void pruneContactPointsIfNeeded(
        f32 separationEpsilon = 1e-6f,
        f32 duplicateEpsilon = 1e-4f);

    /// Prune, invalidate when empty, and return whether points remain (B4.3 deepen pass 2).
    bool pruneAndInvalidateIfEmpty(
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

    /// True when no separated slots would be removed by `pruneNonPenetratingPoints` (B4.4 deepen pass).
    bool canSkipPruneNonPenetrating(f32 epsilon = 1e-6f) const;

    /// True when no duplicate slots would be merged by `pruneDuplicatePoints` (B4.4 deepen pass).
    bool canSkipPruneDuplicates(f32 positionEpsilon = 1e-4f) const;

    /// True when point count is already within `maxPoints` (B4.4 deepen pass).
    bool canSkipPruneToMaxPoints(u32 maxPoints = kMaxContactPointsPerManifold) const;

    /// True when `pruneContactPoints` would be a no-op (B4.4 deepen pass).
    bool canSkipPruneContactPoints(
        f32 duplicateEpsilon = 1e-4f) const;

    /// Returns true when at least one penetrating point is shallower than `minDepth` (B4.4 deepen pass).
    bool hasShallowPenetrations(f32 minDepth) const;

    /// Prune only when `needsPruning`; returns true when points remain (B4.4 deepen pass).
    bool pruneContactPointsIfNeeded(

    /// True when no shallow slots would be removed by `pruneShallowPenetrations` (B4.4 deepen follow-up).
    bool canSkipPruneShallowPenetrations(f32 minDepth) const;

    /// Prune shallow slots only when `hasShallowPenetrations`; returns true when points remain (B4.4 deepen follow-up).
    bool pruneShallowPenetrationsIfNeeded(f32 minDepth);
    /// Prune contact points; returns false when the manifold is empty afterward (B4.3 deepen).
    /// Count contact points with penetration below `-epsilon` (B4.3 deepen pass).
    u32 countSeparatedPoints(f32 epsilon = 1e-6f) const;

    /// Returns true when the contact normal is unit length within `epsilon` (B4.3 deepen pass).
    bool hasUnitNormal(f32 epsilon = 1e-4f) const;

    /// Normalize `contactNormal`; returns false when length is below `epsilon`.
    bool normalizeContactNormal(f32 epsilon = 1e-6f);

    /// Returns true when finalize/generate may proceed after conceptual pruning (B4.3 deepen pass).
    bool canFinalize(
        f32 separationEpsilon = 1e-6f,

    /// Run `pruneContactPoints` and return true when penetrating points remain (B4.3 deepen pass).
    bool pruneForFinalization(
        f32 duplicateEpsilon = 1e-4f) const;




        f32 duplicateEpsilon = 1e-4f);

/// Why manifold prune would early-out (B4.5 deepen follow-up pass).
enum class ManifoldPruneRejectReason : u8 {
    None = 0,
    EmptyManifold,
    AllSeparated,

/// Human-readable label for manifold prune reject reasons (B4.5 deepen follow-up pass).
const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason);

/// Diagnose why manifold prune would skip; vacuously succeeds when prune may proceed (B4.5 deepen follow-up pass).
ManifoldPruneRejectReason manifold_prune_reject_reason(
    const ContactManifold& manifold,

/// Returns true when `manifold_prune_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool manifold_prune_rejects_for_reason(
    ManifoldPruneRejectReason expected,

/// Why manifold prune would early-out (B4.4 deepen follow-up pass).
    WouldBeEmptyAfterPrune,

    /// Returns true when the contact normal length deviates from unit length (B4.5 deepen pass).
    bool hasUnnormalizedNormal(f32 epsilon = 1e-4f) const;

    /// True when `normalizeContactNormalIfNeeded` would be a no-op (B4.5 deepen pass).
    bool canSkipNormalizeContactNormal(f32 epsilon = 1e-4f) const;

    /// Normalize the contact normal only when length deviates from unity (B4.5 deepen pass).
    void normalizeContactNormalIfNeeded(f32 epsilon = 1e-4f);
};

/// Human-readable label for manifold prune reject reasons (B4.4 deepen follow-up pass).
const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason);

/// Diagnose why manifold prune would skip; vacuously succeeds when prune may proceed.
/// Why manifold prune would early-out (B4.4 deepen follow-up pass).
enum class ManifoldPruneRejectReason : u8 {
    None = 0,
    EmptyManifold,
    AllSeparated,
};


/// Diagnose why manifold prune would skip; vacuously succeeds on prunable manifolds (B4.4 deepen follow-up pass).
ManifoldPruneRejectReason manifold_prune_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Returns true when `manifold_prune_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool manifold_prune_rejects_for_reason(
    const ContactManifold& manifold,
    ManifoldPruneRejectReason expected,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Const preflight for manifold prune dispatch (B4.4 deepen pass).
struct ManifoldPrunePreflight {
    ManifoldPruneRejectReason reason = ManifoldPruneRejectReason::None;
    bool hasSeparated = false;
    bool hasDuplicates = false;
    bool hasShallow = false;
    bool exceedsMaxPoints = false;
    bool wouldBeEmpty = false;
    bool needsNormalNormalize = false;
    bool skipped = false;
    /// True when shallow penetration prune would be a no-op (B4.5 deepen pass).
    bool canSkipPruneShallowPenetrations(f32 minDepth) const;

    /// Prune shallow penetrations only when needed; returns true when points remain (B4.5 deepen pass).
    bool pruneShallowPenetrationsIfNeeded(f32 minDepth);

    bool needs_shallow_pruning(f32 minDepth) const { return !skipped && hasShallow; }

    bool needs_any_pruning(f32 shallowMinDepth = 0.f) const {
        return needs_pruning() || needs_shallow_pruning(shallowMinDepth);
    }

    bool can_prune_in_place() const { return needs_pruning() && !wouldBeEmpty; }

    bool can_skip_prune(f32 shallowMinDepth = 0.f) const {
        return skipped || reason != ManifoldPruneRejectReason::None ||
               (!needs_pruning() && !needs_shallow_pruning(shallowMinDepth));
    /// Prune using preflight result; returns true when points remain (B4.5 deepen pass).
    bool pruneFromPreflight(
        const ManifoldPrunePreflight& preflight,
        f32 separationEpsilon = 1e-6f,
        f32 duplicateEpsilon = 1e-4f);

    /// True when no penetrating slots are shallower than `minDepth` (B4.4 deepen pass).
    /// True when no shallow penetrating slots would be removed (B4.5 deepen pass).

    /// Prune shallow slots only when `hasShallowPenetrations`; returns true when points remain (B4.5 deepen pass).
};

/// Why manifold prune would early-out (B4.4 deepen follow-up pass).
enum class ManifoldPruneRejectReason : u8 {
    None = 0,
    EmptyManifold,
    WouldBeEmptyAfterPrune,
    NoPruningNeeded,

/// Human-readable label for manifold prune reject reasons (logging / tests).
const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason);

/// Diagnose why prune would skip; vacuously succeeds when prune may proceed.
ManifoldPruneRejectReason manifold_prune_reject_reason(
    const ContactManifold& manifold,

/// Returns true when `manifold_prune_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool manifold_prune_rejects_for_reason(
    ManifoldPruneRejectReason expected,

/// Non-mutating prune skip predicate — inverse of in-place prune dispatch (B4.4 deepen follow-up pass).
bool can_skip_manifold_prune(

/// Prune only when preflight allows in-place pruning; returns true when points remain (B4.4 deepen follow-up pass).
bool prune_contact_manifold_if_needed(
    ContactManifold& manifold,

/// Const preflight for manifold prune dispatch (B4.4 deepen pass).
struct ManifoldPrunePreflight {
    ManifoldPruneRejectReason reason = ManifoldPruneRejectReason::None;
    bool hasSeparated = false;
    bool hasDuplicates = false;
    bool hasShallowPenetrations = false;
    bool exceedsMaxPoints = false;
    bool hasShallow = false;
    bool wouldBeEmpty = false;
    bool skipped = false;

    bool needs_pruning() const {
        return !skipped && (hasSeparated || hasDuplicates || exceedsMaxPoints || hasShallow);
        return !skipped &&
               (hasSeparated || hasDuplicates || hasShallowPenetrations || exceedsMaxPoints);
    }

        return reason == ManifoldPruneRejectReason::None &&
               (hasSeparated || hasDuplicates || exceedsMaxPoints);

    bool needs_shallow_pruning(f32 minDepth) const {
        return reason == ManifoldPruneRejectReason::None && hasShallow;

    bool can_prune_in_place() const { return needs_pruning() && !wouldBeEmpty; }

    bool can_skip_prune() const { return !needs_pruning(); }
    bool can_skip_prune() const {
        return skipped || reason != ManifoldPruneRejectReason::None || !needs_pruning();
    /// True when `pruneContactPoints` would be a no-op (B4.5 deepen follow-up).
    bool can_skip_prune() const { return skipped || !needs_pruning(); }
    /// True when prune would be a no-op or would leave no points (B4.4 deepen pass).
    bool can_skip_prune() const { return skipped || wouldBeEmpty || !needs_pruning(); }
    bool can_prune_shallow_in_place(f32 shallowMinDepth) const {
        return needs_shallow_pruning(shallowMinDepth) && !wouldBeEmpty;
};

/// Populate prune preflight from a manifold without mutating slots (B4.4 deepen pass).
ManifoldPrunePreflight preflight_manifold_prune(
    f32 duplicateEpsilon = 1e-4f,
    f32 shallowMinDepth = 0.f);

/// Returns true when manifold prune should be skipped (B4.4 deepen pass).
bool should_skip_manifold_prune(

/// Why manifold finalize would early-out (B4.5 deepen follow-up pass).
enum class ManifoldFinalizeRejectReason : u8 {
    InvalidNormal,
    NoPenetratingPoints,
    AllSeparatedAfterPrune,

/// Human-readable label for manifold finalize reject reasons (B4.5 deepen follow-up pass).
const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason);

/// Diagnose why manifold finalize would skip; vacuously succeeds when finalize may proceed (B4.5 deepen follow-up pass).
ManifoldFinalizeRejectReason manifold_finalize_reject_reason(

/// Returns true when `manifold_finalize_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool manifold_finalize_rejects_for_reason(
    ManifoldFinalizeRejectReason expected,
/// Why manifold finalize would early-out (B4.4 deepen follow-up pass).
    None = 0,
    EmptyManifold,
    WouldBeEmptyAfterPrune,
};

/// Human-readable label for manifold finalize reject reasons (logging / tests).

/// Diagnose why finalize would skip; vacuously succeeds when finalize may proceed.
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 frictionEpsilon = 1e-4f);

/// Returns true when `manifold_finalize_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
/// Non-mutating prune skip predicate — inverse of `ContactManifold::needsPruning` (B4.4 deepen follow-up pass).
    f32 duplicateEpsilon = 1e-4f);

/// Non-mutating prune skip predicate including empty-manifold guard (B4.4 deepen follow-up pass).
bool can_skip_manifold_prune(


/// Human-readable label for manifold finalize reject reasons (B4.4 deepen follow-up pass).

/// Diagnose why manifold finalize would skip; vacuously succeeds when finalize may proceed.



/// Diagnose why manifold finalize would skip (B4.4 deepen follow-up pass).

    NoPenetration,
    EmptyAfterPrune,




/// Const preflight for manifold finalize dispatch (B4.4 deepen follow-up).
struct ManifoldFinalizePreflight {
    ManifoldFinalizeRejectReason reason = ManifoldFinalizeRejectReason::None;
    bool skipped = false;
    bool canFinalize = false;
    bool needsPruning = false;
    bool wouldBeEmptyAfterPrune = false;
    bool needsFrictionBasis = false;
    bool canReuseFrictionBasis = false;

    bool can_finalize() const { return !skipped && canFinalize && reason == ManifoldFinalizeRejectReason::None; }
    bool can_finalize() const { return reason == ManifoldFinalizeRejectReason::None; }
    bool can_finalize() const { return reason == ManifoldFinalizeRejectReason::None && canFinalize; }
    bool can_finalize() const { return !skipped && canFinalize; }

    /// True when finalize should be skipped for this manifold (B4.5 deepen follow-up).
    bool can_skip_finalize() const { return skipped || !canFinalize; }
    bool needs_any_work() const { return needsPruning || needsFrictionBasis; }
    bool can_finalize() const {
        return reason == ManifoldFinalizeRejectReason::None && !skipped && canFinalize;
    }
};

/// Populate finalize preflight without mutating the manifold (B4.4 deepen follow-up).
ManifoldFinalizePreflight preflight_manifold_finalize(
    f32 frictionEpsilon = 1e-4f);

/// Returns true when finalize should be skipped for this manifold (B4.4 deepen follow-up).
bool can_skip_manifold_finalize(

/// Prune only when preflight reports in-place pruning is possible; returns true when points remain (B4.5 deepen follow-up pass).
bool prune_contact_manifold_with_preflight(
    ContactManifold& manifold,

/// Finalize only when preflight passes; no-op otherwise (B4.5 deepen follow-up pass).
bool finalize_contact_manifold_with_preflight(

/// Non-mutating prune predicate — inverse of `should_skip_manifold_prune` (B4.6 deepen pass).
bool should_run_manifold_prune(

/// Non-mutating finalize predicate — inverse of `can_skip_manifold_finalize` (B4.6 deepen pass).
bool should_run_manifold_finalize(

/// True when prune preflight reports in-place pruning may proceed (B4.6 deepen pass).
bool can_prune_manifold_in_place(
    /// Drop contact points with penetration below `minPenetration` (B4.3 deepen).
    void pruneShallowPenetrationPoints(f32 minPenetration = 1e-6f);

    /// Run `pruneContactPoints` and return false when no penetrating points remain (B4.3 deepen).
    bool pruneAndRetainPenetrating(

    /// Clear warm-start impulses when pruning emptied the manifold (B4.3 deepen).
    void clearWarmStartIfEmpty();

/// Returns true when chained prune helpers would change `pointCount` (B4.3 deepen).
bool manifold_needs_prune(
    u32 maxPoints = kMaxContactPointsPerManifold,
    f32 shallowPenetration = 1e-6f);

/// Extended preflight including shallow-penetration flag when `shallowMinDepth > 0` (B4.5 deepen pass).
ManifoldPrunePreflight preflight_manifold_prune_ex(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 shallowMinDepth = 0.f);

/// True when preflight reports no pruning work (B4.5 deepen pass).
bool can_skip_manifold_prune(const ManifoldPrunePreflight& preflight);
/// Returns true when `pruneContactPointsIfNeeded` would be a no-op (B4.5 deepen pass).
bool can_skip_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 shallowMinDepth = 0.f);

/// Const preflight for manifold finalize dispatch (B4.4 deepen pass).
/// Why manifold finalize would reject (B4.5 deepen follow-up).
enum class ManifoldFinalizeRejectReason : u8 {
    None = 0,
    Empty,
    InvalidNormal,
    NoPenetratingPoints,
    EmptyAfterPrune,
};

/// Human-readable label for manifold finalize reject reasons (B4.5 deepen follow-up).
const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason);

/// Diagnose why finalize would reject; vacuously succeeds when finalize may proceed (B4.5 deepen follow-up).
ManifoldFinalizeRejectReason manifold_finalize_reject_reason(
    f32 duplicateEpsilon = 1e-4f);

/// Returns true when `manifold_finalize_reject_reason` matches `expected` (B4.5 deepen follow-up).
bool manifold_finalize_rejects_for_reason(
    ManifoldFinalizeRejectReason expected,

/// Const preflight for manifold finalize dispatch (B4.4 deepen follow-up).
struct ManifoldFinalizePreflight {
    bool isEmpty = false;
    bool hasInvalidNormal = false;
    bool hasNoPenetratingPoints = false;
    bool skipped = false;

    bool can_finalize() const {
        return !skipped && !isEmpty && !hasInvalidNormal && !hasNoPenetratingPoints;
    }
};

/// Populate finalize preflight without mutating the manifold (B4.4 deepen pass).
ManifoldFinalizePreflight preflight_manifold_finalize(const ContactManifold& manifold);

/// Returns true when finalize should be skipped for this manifold (B4.4 deepen pass).
bool should_skip_manifold_finalize(const ContactManifold& manifold);

/// Returns true when finalize preflight indicates the manifold is ready (B4.4 deepen pass).
bool can_finalize_with_preflight(const ManifoldFinalizePreflight& preflight);

/// Const preflight for manifold finalize dispatch (B4.4 deepen pass 2).
struct ManifoldFinalizePreflight {
    bool empty = false;
    bool invalidNormal = false;
    bool noPenetratingPoints = false;
    bool needsPrune = false;
    bool wouldBeEmptyAfterPrune = false;
    bool skipped = false;

    bool can_finalize() const {
        return !skipped && !empty && !invalidNormal && !noPenetratingPoints &&
               !wouldBeEmptyAfterPrune;
    }

    bool needs_prune_before_finalize() const {
        return needsPrune && !wouldBeEmptyAfterPrune;
};

/// Populate finalize preflight without mutating slots (B4.4 deepen pass 2).
ManifoldFinalizePreflight preflight_manifold_finalize(
/// Returns true when `preflight_manifold_prune` reports no prune work (B4.4 deepen pass).
/// True when `pruneContactPoints` would be a no-op (B4.5 deepen pass).
bool should_skip_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 shallowMinDepth = 0.f);

/// Diagnostic reason finalize rejects a manifold before friction sync (B4.4 deepen pass).
enum class ManifoldFinalizeFailureReason : u8 {
    None = 0,
    Empty,
    InvalidNormal,
    NoPenetratingPoints,
    PruneWouldEmpty,
    MissingFrictionBasis,
};

/// Human-readable label for finalize failure diagnostics (B4.4 deepen pass).
const char* manifold_finalize_failure_reason_name(ManifoldFinalizeFailureReason reason);

/// Const preflight for manifold finalize dispatch (B4.4 deepen pass).
struct ManifoldFinalizePreflight {
    ManifoldFinalizeFailureReason reason = ManifoldFinalizeFailureReason::None;
    ManifoldPrunePreflight prune{};
    bool skipped = false;

    bool can_finalize() const {
        return !skipped && reason == ManifoldFinalizeFailureReason::None;
    }
};

/// Populate finalize preflight without mutating the manifold (B4.4 deepen pass).
ManifoldFinalizePreflight preflight_manifold_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Returns true when finalize should be skipped (B4.4 deepen pass 2).
bool should_skip_manifold_finalize(
/// Const preflight for manifold finalize dispatch (B4.5 deepen pass).
struct ManifoldFinalizePreflight {
    ManifoldPrunePreflight prune{};
    bool canFinalize = false;
    bool wouldFail = false;
    bool skipped = false;

    bool can_finalize() const { return !skipped && canFinalize; }
};

/// Populate finalize preflight without mutating slots (B4.5 deepen pass).
ManifoldFinalizePreflight preflight_finalize_contact_manifold(
    bool hasValidNormal = false;
    bool hasPenetrating = false;
    bool needsFrictionBasis = false;

    bool can_finalize() const {
        return !skipped && hasValidNormal && hasPenetrating && !prune.wouldBeEmpty;
    }

ManifoldFinalizePreflight preflight_manifold_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Finalize only when preflight allows; clears on skip (B4.4 deepen pass 2).
bool generate_contact_manifold_if_needed(ContactManifold& manifold);

/// Guarded prune: preflight then `pruneContactPointsIfNeeded` (B4.4 deepen pass).
bool prune_contact_points_guarded(
    ContactManifold& manifold,
/// Early-out guard before `generate_contact_manifold` (B4.5 deepen pass).
bool should_skip_finalize_contact_manifold(const ContactManifold& manifold);

/// Prune shallow penetrations only when `hasShallowPenetrations` (B4.5 deepen pass).
bool prune_shallow_penetrations_if_needed(ContactManifold& manifold, f32 minDepth);

/// Finalize only when preflight passes; returns false when skipped or failed (B4.5 deepen pass).
bool generate_contact_manifold_if_ready(ContactManifold& manifold);
    bool empty = true;
    bool invalidNormal = false;
    bool noPenetrating = false;
    bool needsPruning = false;
    bool needsNormalization = false;

        return !skipped && !empty && !invalidNormal && !noPenetrating;

/// Populate finalize preflight without mutating the manifold (B4.5 deepen pass).
ManifoldFinalizePreflight preflight_manifold_finalize(const ContactManifold& manifold);

/// Returns true when the contact normal length deviates from unit length (B4.5 deepen pass).
bool needs_normal_normalization(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Returns true when `generate_contact_manifold` may proceed (B4.5 deepen pass).
bool can_skip_manifold_finalize(const ContactManifold& manifold);

/// Finalize only when preflight allows; returns false without clearing on preflight failure (B4.5 deepen pass).
bool generate_contact_manifold_guarded(ContactManifold& manifold);
/// Returns true when finalize should be skipped (B4.5 deepen pass).
bool should_skip_manifold_finalize(const ContactManifold& manifold);

/// Returns true when `generate_contact_manifold` would be a no-op failure (B4.5 deepen pass).
bool can_skip_manifold_finalize(

/// Finalize only when preflight allows; returns false otherwise (B4.5 deepen pass).
bool generate_contact_manifold_if_valid(ContactManifold& manifold);
/// Returns true when finalize should be skipped for this manifold (B4.4 deepen pass).

/// Per-manifold finalize outcome (skip vs finalize) for batch stubs (B4.4 deepen pass).
struct ManifoldFinalizeResult {
    bool finalized = false;
    ManifoldFinalizeFailureReason reason = ManifoldFinalizeFailureReason::None;

/// Guarded finalize with explicit skip/finalize outcome (B4.4 deepen pass).
ManifoldFinalizeResult generate_contact_manifold_guarded(ContactManifold& manifold);

/// Guarded finalize returning skip/finalize outcome (B4.4 deepen pass).
ManifoldFinalizeResult generate_contact_manifold_result(ContactManifold& manifold);

/// Finalize only when preflight passes; returns false when skipped (B4.4 deepen pass).

/// Non-mutating finalize skip predicate — mirrors `can_skip_manifold_finalize` (B4.4 deepen follow-up pass).
bool should_skip_manifold_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 frictionEpsilon = 1e-4f);

/// Returns true when manifold prune would be a no-op (B4.5 deepen follow-up).
bool should_skip_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Prune only when preflight reports pruning is needed; returns true when points remain (B4.5 deepen follow-up).
bool prune_manifold_if_needed(
    ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Finalize only when `preflight_manifold_finalize` passes; no-op otherwise (B4.5 deepen follow-up).
bool finalize_manifold_if_needed(
    ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 frictionEpsilon = 1e-4f);

/// Why manifold prune would early-out (B4.4 deepen pass).
enum class ManifoldPruneRejectReason : u8 {
    None = 0,
    Empty,
    WouldBeEmpty,
};

/// Human-readable label for manifold prune reject reasons (B4.4 deepen pass).
const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason);

/// Diagnose why prune would skip; vacuously succeeds when prune may proceed (B4.4 deepen pass).
ManifoldPruneRejectReason manifold_prune_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Returns true when `manifold_prune_reject_reason` matches `expected` (B4.4 deepen pass).
bool manifold_prune_rejects_for_reason(
    const ContactManifold& manifold,
    ManifoldPruneRejectReason expected,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Non-mutating prune skip predicate — mirrors `ManifoldPrunePreflight::can_skip_prune` (B4.4 deepen pass).
bool should_skip_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Non-mutating prune predicate — inverse of `should_skip_manifold_prune` (B4.4 deepen pass).
bool should_run_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Prune only when `needsPruning`; returns true when points remain (B4.4 deepen pass).
bool prune_contact_manifold_if_needed(
    ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Non-mutating finalize predicate — inverse of `can_skip_manifold_finalize` (B4.4 deepen pass).
bool should_run_manifold_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 frictionEpsilon = 1e-4f);

/// Returns true when manifold prune should be skipped (B4.5 deepen follow-up).
bool can_skip_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 shallowMinDepth = 0.f);

/// Prune only when preflight indicates work is needed; returns true when points remain (B4.5 deepen follow-up).
bool prune_manifold_if_needed(
    ContactManifold& manifold,

/// Finalize using deepen preflight; no-op when finalize preflight rejects (B4.5 deepen follow-up).
bool finalize_contact_manifold_if_needed(
    f32 frictionEpsilon = 1e-4f);

/// Const preflight for manifold finalize with normal-normalization checks (B4.5 deepen pass).
struct ManifoldFinalizeDeepenPreflight {
    bool skipped = false;
    bool canFinalize = false;
    bool needsNormalNormalization = false;
    bool needsPruning = false;
    bool wouldBeEmptyAfterPrune = false;
    bool needsFrictionBasis = false;
    bool canReuseFrictionBasis = false;
    ManifoldFinalizeRejectReason rejectReason = ManifoldFinalizeRejectReason::None;

    bool can_finalize() const { return !skipped && canFinalize; }
};

/// Populate second deepen finalize preflight without mutating the manifold (B4.5 deepen pass).
ManifoldFinalizeDeepenPreflight preflight_manifold_finalize_deepen(
    f32 frictionEpsilon = 1e-4f,
    f32 normalEpsilon = 1e-4f);

/// Returns true when second deepen finalize should be skipped (B4.5 deepen pass).
bool can_skip_manifold_finalize_deepen(

/// Finalize only when second deepen preflight passes; no-op otherwise (B4.5 deepen pass).
bool generate_contact_manifold_deepen_if_needed(ContactManifold& manifold);

/// Returns true when manifold prune should be skipped (B4.5 deepen pass).

/// Finalize with conditional prune and friction rebuild; no-op when preflight rejects (B4.4 deepen follow-up pass).

/// Finalize with optional shallow penetration prune in deepen path only (B4.4 deepen follow-up pass).
bool generate_contact_manifold_deepen(
    f32 shallowMinDepth = 0.f,

/// Prune only when preflight allows; returns true when points remain (B4.4 deepen follow-up pass).
    f32 duplicateEpsilon = 1e-4f);

/// Finalize only when preflight allows; no-op otherwise (B4.4 deepen follow-up pass).
bool finalize_contact_manifold_if_needed(ContactManifold& manifold);

/// Why manifold prune would early-out (B4.4 deepen follow-up pass).
enum class ManifoldPruneRejectReason : u8 {
    None = 0,
    EmptyManifold,
    WouldBeEmptyAfterPrune,

/// Human-readable label for manifold prune reject reasons (logging / tests).
const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason);

/// Diagnose why prune would skip; vacuously succeeds when prune may proceed.
ManifoldPruneRejectReason manifold_prune_reject_reason(

/// Returns true when `manifold_prune_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool manifold_prune_rejects_for_reason(
    ManifoldPruneRejectReason expected,

/// Non-mutating prune skip predicate — mirrors `canSkipPruneContactPoints` (B4.4 deepen follow-up pass).

/// Why manifold finalize would early-out (B4.4 deepen follow-up pass).
enum class ManifoldFinalizeRejectReason : u8 {
    InvalidNormal,
    NoPenetratingPoints,

/// Human-readable label for manifold finalize reject reasons (logging / tests).
const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason);

/// Diagnose why finalize would skip; vacuously succeeds when finalize may proceed.
ManifoldFinalizeRejectReason manifold_finalize_reject_reason(

/// Returns true when `manifold_finalize_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool manifold_finalize_rejects_for_reason(
    ManifoldFinalizeRejectReason expected,

/// Const preflight for generate dispatch (B4.4 deepen follow-up pass).
struct ManifoldGeneratePreflight {
    bool canGenerate = false;

    bool can_generate() const { return !skipped && canGenerate; }

/// Populate generate preflight without mutating the manifold (B4.4 deepen follow-up pass).
ManifoldGeneratePreflight preflight_generate_contact_manifold(

/// Returns true when generate should be skipped for this manifold (B4.4 deepen follow-up pass).
bool can_skip_generate_contact_manifold(

/// Per-step prune chain preflight (B4.4 deepen follow-up pass).
struct ManifoldPruneChainPreflight {
    bool needsSeparationPrune = false;
    bool needsDuplicatePrune = false;
    bool needsMaxPointPrune = false;
    bool needsShallowPrune = false;
    bool wouldBeEmpty = false;

    bool needs_pruning() const {
        return !skipped &&
               (needsSeparationPrune || needsDuplicatePrune || needsMaxPointPrune || needsShallowPrune);
    }

    bool can_prune_in_place() const { return needs_pruning() && !wouldBeEmpty; }

/// Populate prune-chain preflight without mutating slots (B4.4 deepen follow-up pass).
ManifoldPruneChainPreflight preflight_manifold_prune_chain(
/// Read-only manifold prune dispatch diagnostics — no mutation (B4.4 deepen follow-up pass).
struct ManifoldPruneDispatchPreflight {
    bool needsRegularPrune = false;

    bool can_prune() const {
        return !skipped && (needsRegularPrune || needsShallowPrune) && !wouldBeEmpty;

    bool can_skip() const {
        return skipped || wouldBeEmpty || (!needsRegularPrune && !needsShallowPrune);

/// Populate combined regular/shallow prune preflight without mutating slots (B4.4 deepen follow-up pass).
ManifoldPruneDispatchPreflight preflight_manifold_prune_dispatch(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 shallowMinDepth = 0.f);

/// Returns true when prune chain would be a no-op (B4.4 deepen follow-up pass).
bool can_skip_manifold_prune(

/// Prune only when preflight reports work; returns true when points remain (B4.4 deepen follow-up pass).
bool prune_contact_points_if_needed(
    ContactManifold& manifold,
    f32 duplicateEpsilon = 1e-4f);

/// Combined finalize-chain preflight (prune + generate) (B4.4 deepen follow-up pass).
struct ManifoldFinalizeChainPreflight {
    ManifoldPruneChainPreflight prune{};
    ManifoldGeneratePreflight generate{};

    bool can_finalize() const { return !skipped && generate.can_generate(); }

/// Populate finalize-chain preflight without mutating the manifold (B4.4 deepen follow-up pass).
ManifoldFinalizeChainPreflight preflight_manifold_finalize_chain(
    f32 shallowMinDepth = 0.f,
    f32 frictionEpsilon = 1e-4f);

/// Finalize only when preflight allows; no-op otherwise (B4.4 deepen follow-up pass).
bool finalize_contact_manifold_if_needed(ContactManifold& manifold);
/// Returns true when manifold prune dispatch may be skipped (B4.5 deepen follow-up).
bool should_skip_manifold_prune(

/// Returns true when in-place prune is safe per preflight (B4.5 deepen follow-up).
bool can_prune_manifold_in_place(
/// Non-mutating prune dispatch skip predicate (B4.4 deepen follow-up pass).
bool can_skip_manifold_prune_dispatch(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 shallowMinDepth = 0.f);

/// Prune regular and shallow slots only when dispatch preflight allows (B4.4 deepen follow-up pass).
bool prune_contact_manifold_if_needed(

inline ContactManifold invalidContactManifold() {
    return ContactManifold();
}

} // namespace fuse::physics::narrowphase
