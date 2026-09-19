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
    bool hasNonUnitNormal(f32 lengthEpsilon = 1e-4f) const;
    bool needsNormalNormalization(f32 lengthEpsilon = 1e-4f) const;
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

/// Why manifold prune would early-out (B4.5 deepen follow-up pass).
enum class ManifoldPruneRejectReason : u8 {
    None = 0,
    EmptyManifold,
    AllSeparated,
};

/// Human-readable label for manifold prune reject reasons (B4.5 deepen follow-up pass).
const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason);

/// Diagnose why manifold prune would skip; vacuously succeeds when prune may proceed (B4.5 deepen follow-up pass).
ManifoldPruneRejectReason manifold_prune_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Returns true when `manifold_prune_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
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

    bool needs_pruning() const {
        return !skipped && (hasSeparated || hasDuplicates || exceedsMaxPoints);
    }

    bool needs_shallow_pruning(f32 minDepth) const { return !skipped && hasShallow; }

    bool can_prune_in_place() const { return needs_pruning() && !wouldBeEmpty; }

    bool can_skip_prune(f32 shallowMinDepth = 0.f) const {
        return skipped || reason != ManifoldPruneRejectReason::None ||
               (!needs_pruning() && !needs_shallow_pruning(shallowMinDepth));
    }
};

/// Populate prune preflight from a manifold without mutating slots (B4.4 deepen pass).
ManifoldPrunePreflight preflight_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 shallowMinDepth = 0.f);

/// Returns true when manifold prune should be skipped (B4.4 deepen pass).
bool should_skip_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 shallowMinDepth = 0.f);

/// Why manifold finalize would early-out (B4.5 deepen follow-up pass).
enum class ManifoldFinalizeRejectReason : u8 {
    None = 0,
    EmptyManifold,
    InvalidNormal,
    NoPenetratingPoints,
    AllSeparatedAfterPrune,
};

/// Human-readable label for manifold finalize reject reasons (B4.5 deepen follow-up pass).
const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason);

/// Diagnose why manifold finalize would skip; vacuously succeeds when finalize may proceed (B4.5 deepen follow-up pass).
ManifoldFinalizeRejectReason manifold_finalize_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Returns true when `manifold_finalize_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool manifold_finalize_rejects_for_reason(
    const ContactManifold& manifold,
    ManifoldFinalizeRejectReason expected,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Const preflight for manifold finalize dispatch (B4.4 deepen follow-up).
struct ManifoldFinalizePreflight {
    ManifoldFinalizeRejectReason reason = ManifoldFinalizeRejectReason::None;
    bool skipped = false;
    bool canFinalize = false;
    bool needsPruning = false;
    bool wouldBeEmptyAfterPrune = false;
    bool needsNormalNormalize = false;
    bool needsFrictionBasis = false;
    bool canReuseFrictionBasis = false;

    bool can_finalize() const { return !skipped && canFinalize && reason == ManifoldFinalizeRejectReason::None; }
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

/// Prune only when preflight reports in-place pruning is possible; returns true when points remain (B4.5 deepen follow-up pass).
bool prune_contact_manifold_with_preflight(
    ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 shallowMinDepth = 0.f);

/// Finalize only when preflight passes; no-op otherwise (B4.5 deepen follow-up pass).
bool finalize_contact_manifold_with_preflight(
    ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 frictionEpsilon = 1e-4f);

/// Non-mutating prune predicate — inverse of `should_skip_manifold_prune` (B4.6 deepen pass).
bool should_run_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 shallowMinDepth = 0.f);

/// Non-mutating finalize predicate — inverse of `can_skip_manifold_finalize` (B4.6 deepen pass).
bool should_run_manifold_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 frictionEpsilon = 1e-4f);

/// True when prune preflight reports in-place pruning may proceed (B4.6 deepen pass).
bool can_prune_manifold_in_place(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 shallowMinDepth = 0.f);

inline ContactManifold invalidContactManifold() {
    return ContactManifold();
}

} // namespace fuse::physics::narrowphase

// --- deepen additive from deepen-b4-narrowphase-guards-56bb ---
    bool pruneFromPreflight(
        const ManifoldPrunePreflight& preflight,
ManifoldPrunePreflight preflight_manifold_prune_ex(
bool can_skip_manifold_prune(const ManifoldPrunePreflight& preflight);

// --- deepen additive from b4-narrowphase-deepen-guards-e063 ---
bool should_skip_manifold_finalize(

// --- deepen additive from b4-narrowphase-deepen-guards-c64f ---
    ManifoldPrunePreflight prune{};
    bool wouldFail = false;
ManifoldFinalizePreflight preflight_finalize_contact_manifold(
bool should_skip_finalize_contact_manifold(const ContactManifold& manifold);

// --- deepen additive from b4-narrowphase-guards-deepen-ea96 ---
ManifoldFinalizePreflight preflight_manifold_finalize(const ContactManifold& manifold);
bool should_skip_manifold_finalize(const ContactManifold& manifold);
bool can_finalize_with_preflight(const ManifoldFinalizePreflight& preflight);

// --- deepen additive from b4-narrowphase-deepen-guards-a773 ---
        return reason == ManifoldPruneRejectReason::None &&
        return reason == ManifoldPruneRejectReason::None && hasShallow;
    bool can_finalize() const { return reason == ManifoldFinalizeRejectReason::None; }

// --- deepen additive from deepen-b4-narrowphase-guards-72f5 ---
        return skipped || reason != ManifoldPruneRejectReason::None || !needs_pruning();
    bool can_finalize() const { return reason == ManifoldFinalizeRejectReason::None && canFinalize; }

// --- deepen additive from deepen-b4-narrowphase-guards-1468 ---
struct ManifoldFinalizeDeepenPreflight {
ManifoldFinalizeDeepenPreflight preflight_manifold_finalize_deepen(

// --- deepen additive from b4-narrowphase-deepen-guards-6e88 ---
struct ManifoldGeneratePreflight {
ManifoldGeneratePreflight preflight_generate_contact_manifold(
struct ManifoldPruneChainPreflight {
ManifoldPruneChainPreflight preflight_manifold_prune_chain(
struct ManifoldFinalizeChainPreflight {
    ManifoldPruneChainPreflight prune{};
    ManifoldGeneratePreflight generate{};
ManifoldFinalizeChainPreflight preflight_manifold_finalize_chain(

// --- deepen additive from deepen-b4-narrowphase-guards-ddb5 ---
    ManifoldFinalizeRejectReason rejectReason = ManifoldFinalizeRejectReason::None;

// --- deepen additive from deepen-b4-narrowphase-guards-f4c2 ---
        return reason == ManifoldFinalizeRejectReason::None && !skipped && canFinalize;
struct ManifoldPruneDispatchPreflight {
ManifoldPruneDispatchPreflight preflight_manifold_prune_dispatch(

// --- deepen additive from deepen-b4-narrowphase-guards-0339 ---
        return reason == ManifoldPruneRejectReason::Empty ||
               reason == ManifoldPruneRejectReason::AlreadyClean;
    bool can_prune() const { return reason == ManifoldPruneRejectReason::None; }

// --- deepen additive from deepen-b4-narrowphase-guards-071f ---
        if (reason != ManifoldPruneRejectReason::None) {

// --- deepen additive from b4-narrowphase-deepen-guards-046d ---
struct ManifoldPruneFinalizePreflight {
    ManifoldFinalizePreflight finalize{};
ManifoldPruneFinalizePreflight preflight_manifold_prune_finalize(
bool should_skip_manifold_prune_finalize(

// --- deepen additive from deepen-b4-narrowphase-guards-754b ---
struct ManifoldProcessPreflight {
ManifoldProcessPreflight preflight_manifold_process(

// --- deepen additive from b4-narrowphase-deepen-guards-68c9 ---
enum class ManifoldShallowPruneRejectReason : u8 {
const char* manifold_shallow_prune_reject_reason_name(ManifoldShallowPruneRejectReason reason);
ManifoldShallowPruneRejectReason manifold_shallow_prune_reject_reason(
    ManifoldShallowPruneRejectReason expected,

// --- deepen additive from deepen-b4-narrowphase-6c66 ---
enum class ContactManifoldWriteRejectReason : u8 {
const char* contact_manifold_write_reject_reason_name(ContactManifoldWriteRejectReason reason);
ContactManifoldWriteRejectReason contact_manifold_write_reject_reason(const ContactManifold& manifold);
    ContactManifoldWriteRejectReason expected);
struct ContactManifoldWritePreflight {
    ContactManifoldWriteRejectReason reason = ContactManifoldWriteRejectReason::None;
    bool can_write() const { return !skipped && reason == ContactManifoldWriteRejectReason::None; }
ContactManifoldWritePreflight preflight_contact_manifold_buffer_write(const ContactManifold& manifold);
bool should_skip_contact_manifold_buffer_write(const ContactManifold& manifold);

// --- deepen additive from deepen-b4-narrowphase-guards-1644 ---
struct ManifoldShallowPrunePreflight {
    ManifoldShallowPruneRejectReason reason = ManifoldShallowPruneRejectReason::None;
    bool can_prune() const { return !skipped && reason == ManifoldShallowPruneRejectReason::None; }
ManifoldShallowPrunePreflight preflight_manifold_shallow_prune(

// --- deepen additive from deepen-b4-narrowphase-guards-56fd ---
struct ManifoldBeyondPrunePreflight {
ManifoldBeyondPrunePreflight preflight_manifold_beyond_prune(
bool should_skip_manifold_beyond_prune(

// --- deepen additive from deepen-narrowphase-b4-guards-2406 ---
ManifoldPruneRejectReason manifold_prune_second_reject_reason(
ManifoldFinalizeRejectReason manifold_finalize_second_reject_reason(

// --- deepen additive from b4-narrowphase-deepen-8324 ---
    return !should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);

// --- deepen additive from b4-narrowphase-deepen-guards-1595 ---
    if (should_skip_friction_basis_preflight(manifold, epsilon)) {
        if (friction_basis_reject_reason(manifold) != FrictionBasisRejectReason::None) {

// --- deepen additive from b4-narrowphase-deepen-guards-af6c ---
    const ManifoldPrunePreflight preflight =
    if (preflight.reason != ManifoldPruneRejectReason::None) {
        if (preflight.reason == ManifoldPruneRejectReason::AllSeparated) {

// --- deepen additive from b4-narrowphase-deepen-guards-ea87 ---
        return skipped || reason != ManifoldPruneRejectReason::None || !needs_shallow_pruning(shallowMinDepth);

// --- deepen additive from b4-narrowphase-deepen-guards-04be ---
struct ManifoldPruneAndFinalizePreflight {
        return !skipped && finalize.can_finalize() && prune.reason == ManifoldPruneRejectReason::None;
ManifoldPruneAndFinalizePreflight preflight_manifold_prune_and_finalize(
inline ManifoldPruneAndFinalizePreflight preflight_manifold_prune_and_finalize(
    ManifoldPruneAndFinalizePreflight preflight{};
    const ManifoldPruneAndFinalizePreflight preflight = preflight_manifold_prune_and_finalize(
        if (preflight.finalize.reason == ManifoldFinalizeRejectReason::AllSeparatedAfterPrune ||
            preflight.prune.reason == ManifoldPruneRejectReason::AllSeparated) {

// --- deepen additive from deepen-b4-narrowphase-guards-950d ---
enum class ManifoldNormalizeRejectReason : u8 {
inline const char* manifold_normalize_reject_reason_name(ManifoldNormalizeRejectReason reason) {
    case ManifoldNormalizeRejectReason::None:
    case ManifoldNormalizeRejectReason::EmptyManifold:
    case ManifoldNormalizeRejectReason::InvalidNormal:
inline ManifoldNormalizeRejectReason manifold_normalize_reject_reason(const ContactManifold& manifold) {
        return ManifoldNormalizeRejectReason::EmptyManifold;
        return ManifoldNormalizeRejectReason::InvalidNormal;
    return ManifoldNormalizeRejectReason::None;
    ManifoldNormalizeRejectReason expected) {
struct ManifoldNormalizePreflight {
    ManifoldNormalizeRejectReason reason = ManifoldNormalizeRejectReason::None;
        return !skipped && reason == ManifoldNormalizeRejectReason::None && needsNormalize;
        return skipped || reason != ManifoldNormalizeRejectReason::None || !needsNormalize;
inline ManifoldNormalizePreflight preflight_manifold_normalize(
    ManifoldNormalizePreflight preflight{};
    if (preflight.reason != ManifoldNormalizeRejectReason::None) {
    const ManifoldNormalizePreflight preflight = preflight_manifold_normalize(manifold, lengthEpsilon);
inline ManifoldPruneFinalizePreflight preflight_manifold_prune_finalize(
    ManifoldPruneFinalizePreflight preflight{};
    if (preflight.prune.reason != ManifoldPruneRejectReason::None) {

// --- deepen additive from b4-narrowphase-deeper-guards-3864 ---
    return should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);

// --- deepen additive from deepen-b4-narrowphase-guards-5907 ---
inline bool wouldSkipManifoldPrune(
inline bool wouldSkipManifoldFinalize(
inline bool tryPruneContactManifold(
inline bool tryFinalizeContactManifold(

// --- deepen additive from b4-narrowphase-deepen-guards-3dcc ---
inline bool would_skip_manifold_prune(
inline bool would_skip_manifold_finalize(
inline bool try_prune_contact_manifold(
inline bool try_finalize_contact_manifold(

// --- deepen additive from b4-narrowphase-deepen-guards-27c6 ---
    ManifoldPruneRejectReason* reason = nullptr,
    if (should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)) {
        *reason = ManifoldPruneRejectReason::None;
    ManifoldFinalizeRejectReason* reason = nullptr,
    const ManifoldFinalizeRejectReason rejectReason =
    if (wouldSkipManifoldPrune(manifold, nullptr, separationEpsilon, duplicateEpsilon, shallowMinDepth)) {
    if (wouldSkipManifoldFinalize(manifold, nullptr, separationEpsilon, duplicateEpsilon, frictionEpsilon)) {

// --- deepen additive from deepen-b4-narrowphase-guards-6ef0 ---
FUSE_PHYSICS_INLINE bool would_skip_manifold_prune(
FUSE_PHYSICS_INLINE bool would_skip_manifold_finalize(
FUSE_PHYSICS_INLINE bool try_prune_contact_manifold(
FUSE_PHYSICS_INLINE bool try_finalize_contact_manifold(

// --- deepen additive from b4-narrowphase-deepen-guards-bcad ---
inline bool try_preflight_manifold_prune(
    ManifoldPruneRejectReason& reason,
    const ManifoldFinalizePreflight preflight =
inline bool try_preflight_manifold_finalize(
    ManifoldFinalizeRejectReason& reason,

// --- deepen additive from deepen-b4-narrowphase-b3e2 ---
FUSE_PHYSICS_INLINE bool try_prune_contact_manifold_with_preflight(
    if (would_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)) {
FUSE_PHYSICS_INLINE bool try_finalize_contact_manifold_with_preflight(
    if (would_skip_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon)) {

// --- deepen additive from b4-narrowphase-deepen-guards-2edd ---
    return preflight.reason == ManifoldPruneRejectReason::None &&
    return !should_skip_friction_basis_preflight(manifold, epsilon);
    if (should_skip_friction_tangents(manifold)) {
    const FrictionBasisPreflight preflight = preflight_friction_basis_rebuild(manifold, epsilon);
    return preflight.reason == FrictionBasisRejectReason::None && preflight.needsRebuild;
FUSE_PHYSICS_INLINE bool would_skip_friction_basis_rebuild(
    return should_skip_friction_basis_preflight(manifold, epsilon);
FUSE_PHYSICS_INLINE bool try_rebuild_friction_basis(ContactManifold& manifold, f32 epsilon) {
    if (would_skip_friction_basis_rebuild(manifold, epsilon)) {
FUSE_PHYSICS_INLINE bool try_compute_friction_tangents(ContactManifold& manifold, f32 epsilon) {
