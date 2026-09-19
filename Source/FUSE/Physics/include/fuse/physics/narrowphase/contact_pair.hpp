#pragma once

#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics::narrowphase {

/// Diagnostic reason a broadphase pair is rejected before narrowphase dispatch (B4.3 deepen).
enum class ContactPairRejectReason : u8 {
    None = 0,
    SelfPair,
    OutOfRangeBody,
    MissingShape,
    BothTriggers,
    BothStatic,
    BothSleeping,
    UnsupportedShapePair,
    BothStatic,
    BothSleeping,
    BothKinematic,
    DegenerateShape,
    BothSleeping,
    BothKinematic,
    AnyTrigger,
    BothMassless,
    BothZeroInvMass,
    NoColliderDispatch,
    ZeroInvMass,
    NegativeInverseMass,
    BothZeroMass,
    SleepingKinematicMix,
    PlanePlane,
    InvalidPlaneNormal,
    ShapeBodyMismatch,
    BothPlane,
    BothPlanes,
};

/// Human-readable label for diagnostics and test assertions (B4.3 deepen pass).
const char* contact_pair_reject_reason_name(ContactPairRejectReason reason);

/// Returns true when `contact_pair_reject_reason` matches `expected` (B4.3 deepen pass).
bool contact_pair_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.4 deepen pass).
bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Returns true when both indices reference the same body (B4.3 deepen pass).
bool is_self_contact_pair(const broadphase::CandidatePair& pair);

/// Returns true when either body index is out of range (B4.3 deepen pass).
bool is_out_of_range_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies);

/// Returns true when either body has no collision shape (B4.3 deepen pass).
bool is_missing_shape_contact_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes);

/// Returns the first reject reason for a pair, or `None` when dispatch may proceed.
ContactPairRejectReason contact_pair_reject_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Diagnostic label for logging and test assertions (B4.3 deepen).
const char* contact_pair_reject_reason_name(ContactPairRejectReason reason);

/// Returns true when the manifold has no contact points (B4.3 deepen).
bool is_empty_contact_manifold(const ContactManifold& manifold);

/// Returns true when the manifold is marked valid with points and a non-zero normal (B4.3 deepen).
bool is_valid_contact_manifold(const ContactManifold& manifold);

/// Returns true when narrowphase should skip this pair (self, OOB bodies, or missing shapes).
bool is_invalid_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Inverse of `is_invalid_contact_pair` (B4.3 deepen pass).
bool is_valid_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when both bodies are trigger volumes (no contact response stub).
bool is_trigger_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies);

/// Returns true when both bodies carry `RB_STATIC` (no solver response stub, B4.3 deepen pass).
bool is_static_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies);

/// Returns true when both bodies carry `RB_SLEEPING` (B4.4 deepen follow-up).
bool is_sleeping_contact_pair(

/// Returns true when both bodies carry `RB_KINEMATIC` (B4.4 deepen follow-up).
bool is_kinematic_contact_pair(

/// Returns true when either body carries `RB_TRIGGER` (B4.4 deepen pass).
bool is_any_trigger_contact_pair(

/// Returns true when both bodies have non-positive inverse mass (B4.4 deepen pass).
bool is_massless_contact_pair(
    const RigidBodySoA& bodies,
    f32 invMassEpsilon = 1e-8f);
/// Returns true when both bodies carry `RB_SLEEPING` (no narrowphase dispatch stub, B4.3 deepen pass).
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies);

/// Returns true when both bodies carry `RB_KINEMATIC` (no dynamic response stub, B4.3 deepen pass).
/// Returns true when both bodies carry `RB_KINEMATIC` (no solver response stub, B4.5 deepen pass).

/// Returns true when both bodies carry `RB_SLEEPING` (narrowphase skip stub, B4.5 deepen pass).
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies);

/// Returns true when both bodies have negligible inverse mass (B4.4 deepen follow-up pass).
bool is_zero_inv_mass_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    f32 invMassEpsilon = 1e-8f);

/// Returns true when either shape has zero or negative extent (B4.3 deepen pass).
bool is_degenerate_shape_pair(
    const CollisionShapeSoA& shapes);

/// Returns true when both bodies are static (no dynamic response stub).
bool is_static_static_pair(

/// Returns true when both bodies are sleeping (solver early-out stub).
bool is_both_sleeping_pair(

/// Returns true when narrowphase dispatch may proceed for this pair.
bool contact_pair_should_dispatch(

/// Human-readable label for diagnostics and tests (B4.3 deepen).
const char* contact_pair_reject_reason_label(ContactPairRejectReason reason);

/// Returns true when the resolved shape types have no narrowphase dispatch path.
bool is_unsupported_shape_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes);

/// Semantic alias for `is_invalid_contact_pair` in dispatch loops (B4.3 deepen pass 2).
bool should_skip_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when both bodies resolve to plane shapes (unsupported narrowphase stub, B4.3 deepen pass 2).
bool is_plane_plane_contact_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes);

/// Returns true when shapes exist and have a non-degenerate dispatch path (B4.3 deepen pass 2).
bool has_contact_pair_dispatch_path(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes);

/// Run shape dispatch for one broadphase candidate pair (B4.3 deepen).
/// Returns an invalid manifold for empty/self pairs or missing shapes.
ContactManifold detect_contacts_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Preflight guard before finalize: non-empty, unit normal candidate, penetrating points (B4.3 deepen pass).
bool can_finalize_contact_manifold(const ContactManifold& manifold);

/// Finalize a detected manifold: sync legacy fields, friction tangents, validity (B4.3 deepen).
/// Returns false when the manifold has no contact points.
bool generate_contact_manifold(ContactManifold& manifold);

/// Finalize only when `can_finalize_contact_manifold` passes; no-op otherwise (B4.4 deepen follow-up).
bool generate_contact_manifold_if_needed(ContactManifold& manifold);

/// Build and store an orthonormal tangent frame on `manifold` (B4.3 deepen).
void compute_friction_tangents(ContactManifold& manifold);

/// Const preflight for narrowphase pair dispatch (B4.4 deepen pass).
struct ContactPairPreflight {
    ContactPairRejectReason reason = ContactPairRejectReason::None;
    bool rejected = false;
    bool isSelfPair = false;
    bool isOutOfRange = false;
    bool isMissingShape = false;
    bool isBothTriggers = false;
    bool isUnsupportedShape = false;
    bool isBothStatic = false;
    bool isDegenerateShape = false;

    bool can_dispatch() const { return reason == ContactPairRejectReason::None; }
};

/// Populate pair preflight without running shape dispatch (B4.4 deepen pass).
ContactPairPreflight preflight_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when narrowphase should skip this pair before dispatch (B4.4 deepen pass).
bool should_skip_contact_pair_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating dispatch predicate — inverse of `should_skip_contact_pair_dispatch` (B4.4 deepen pass).
bool should_run_contact_pair_dispatch(
/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.4 deepen guard pass).
bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Non-mutating dispatch predicate — inverse of `should_skip_contact_pair_dispatch` (B4.4 deepen guard pass).
    const CollisionShapeSoA& shapes);

/// Extended reject reason including sleeping/kinematic pairs (B4.4 deepen follow-up).
/// Does not alter `contact_pair_reject_reason`; use for additive preflight only.
ContactPairRejectReason contact_pair_deepen_reject_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Extended reject reason including sleeping/kinematic pairs (B4.4 deepen follow-up).
/// Does not alter `contact_pair_reject_reason`; use for additive preflight only.
ContactPairRejectReason contact_pair_deepen_reject_reason(
/// Returns true when both shapes are planes (no narrowphase dispatch path, B4.5 deepen pass).
bool is_plane_plane_contact_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes);

/// Inverse of `should_skip_contact_pair_dispatch` (B4.5 deepen pass).
bool can_dispatch_contact_pair(
    const RigidBodySoA& bodies,
/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.4 deepen pass).
bool contact_pair_deepen_rejects_for_reason(
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Const preflight with extended sleeping/kinematic reject checks (B4.4 deepen follow-up).
struct ContactPairDeepenPreflight {
    ContactPairRejectReason reason = ContactPairRejectReason::None;
    bool rejected = false;
/// Extended pair reject preflight with per-check flags (B4.5 deepen pass).
struct ContactPairRejectPreflight {
    bool selfPair = false;
    bool outOfRange = false;
    bool missingShape = false;
    bool bothTriggers = false;
    bool unsupportedPair = false;
/// Per-reason flags for pair reject preflight (B4.5 deepen pass).
    bool outOfRangeBody = false;
    bool unsupportedShapePair = false;
    bool bothStatic = false;
    bool degenerateShape = false;
    bool isSleeping = false;
    bool isKinematic = false;
    bool isAnyTrigger = false;
    bool isMassless = false;
    bool isDeepenDegenerate = false;
    bool bothSleeping = false;
    bool bothKinematic = false;
    bool anyTrigger = false;
    bool bothMassless = false;

    bool can_dispatch() const { return reason == ContactPairRejectReason::None; }
};

/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Populate extended pair preflight without running shape dispatch (B4.4 deepen follow-up).
ContactPairDeepenPreflight preflight_contact_pair_deepen(
/// Guarded detect: shape dispatch only when preflight allows (B4.5 deepen pass).
ContactManifold detect_contacts_pair_if_valid(
/// Per-reason reject flags for const pair preflight (B4.4 deepen pass 2).
struct ContactPairRejectBreakdown {

    bool rejected() const { return reason != ContactPairRejectReason::None; }
    bool can_dispatch() const { return !rejected(); }

/// Populate reject breakdown without running shape dispatch (B4.4 deepen pass 2).
ContactPairRejectBreakdown contact_pair_reject_breakdown(

/// Combined preflight + detect outcome for parallel dispatch stubs (B4.5 deepen pass).
struct ContactPairDispatchResult {
    ContactPairPreflight preflight{};
    ContactManifold manifold{};
    bool detected = false;

    bool rejected() const { return preflight.rejected; }

/// Preflight then detect; rejected pairs leave `detected` false (B4.5 deepen pass).
ContactPairDispatchResult dispatch_contact_pair_if_valid(

/// Returns true when extended preflight rejects this pair (B4.4 deepen follow-up).
bool should_skip_contact_pair_deepen_dispatch(

/// True when all pairs are rejected by extended preflight or the pair list is empty (B4.4 deepen follow-up).
bool can_skip_narrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,

/// Count pairs that pass extended deepen preflight (B4.4 deepen pass).
u32 count_dispatchable_contact_pairs(

/// True when at least one pair passes extended deepen preflight (B4.4 deepen pass).
bool has_dispatchable_contact_pair(

/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool contact_pair_deepen_rejects_for_reason(
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Returns true when both shapes resolve to plane types (B4.5 deepen follow-up pass).

/// Const preflight for narrowphase batch dispatch (B4.5 deepen follow-up pass).
struct NarrowphaseBatchPreflight {
    u32 pairCount = 0u;
    u32 dispatchableCount = 0u;
    u32 rejectedCount = 0u;

    bool can_dispatch() const { return dispatchableCount > 0u; }
    bool can_skip() const { return pairCount == 0u || dispatchableCount == 0u; }

/// Populate batch preflight without running shape dispatch (B4.5 deepen follow-up pass).
NarrowphaseBatchPreflight preflight_narrowphase_batch(

/// Returns true when batch preflight reports no dispatchable pairs (B4.5 deepen follow-up pass).
bool narrowphase_batch_rejects_all(

/// Non-mutating pair dispatch predicate — inverse of `should_skip_contact_pair_dispatch` (B4.6 deepen pass).
bool should_run_contact_pair_dispatch(

/// Non-mutating deepen dispatch predicate — inverse of `should_skip_contact_pair_deepen_dispatch` (B4.6 deepen pass).
bool should_run_contact_pair_deepen_dispatch(

/// Non-mutating batch dispatch predicate — inverse of `narrowphase_batch_rejects_all` (B4.6 deepen pass).
bool should_run_narrowphase_batch(
/// Human-readable reject reason from a preflight snapshot (B4.5 deepen pass).
const char* contact_pair_preflight_reason_name(const ContactPairPreflight& preflight);

/// Guarded finalize using `can_finalize_contact_manifold` preflight (B4.5 deepen pass).
bool generate_contact_manifold_if_valid(ContactManifold& manifold);
/// Returns true when `preflight_contact_pair` reports `expected` (B4.5 deepen pass).
bool contact_pair_preflight_matches(
    const ContactPairPreflight& preflight,

/// Const preflight for manifold finalize dispatch (B4.5 deepen pass).
struct ManifoldFinalizePreflight {
    bool empty = false;
    bool invalidNormal = false;
    bool noPenetratingPoints = false;
    bool wouldBeEmptyAfterPrune = false;
    bool skipped = false;

    bool can_finalize() const {
        return !skipped && !empty && !invalidNormal && !noPenetratingPoints && !wouldBeEmptyAfterPrune;
    }

/// Populate finalize preflight without mutating manifold slots (B4.5 deepen pass).
ManifoldFinalizePreflight preflight_finalize_contact_manifold(const ContactManifold& manifold);

/// Returns true when the manifold is already finalized and valid (B4.5 deepen pass).
bool can_skip_finalize_contact_manifold(const ContactManifold& manifold);

/// Finalize only when preflight passes and the manifold is not yet valid (B4.5 deepen pass).
bool generate_contact_manifold_if_needed(ContactManifold& manifold);

/// Returns true when breakdown matches the expected reject reason (B4.4 deepen pass 2).
bool contact_pair_rejects_with_breakdown(
/// Const preflight for manifold finalize dispatch (B4.4 deepen pass).
struct ContactManifoldFinalizePreflight {
    bool pruneWouldEmpty = false;

        return !skipped && !empty && !invalidNormal && !noPenetratingPoints && !pruneWouldEmpty;

/// Populate finalize preflight without mutating manifold slots (B4.4 deepen pass).
ContactManifoldFinalizePreflight preflight_contact_manifold_finalize(
/// Populate extended pair reject preflight without shape dispatch (B4.5 deepen pass).
ContactPairRejectPreflight preflight_contact_pair_reject(

/// Explicit reject predicate mirroring `is_invalid_contact_pair` (B4.5 deepen pass).
bool should_reject_contact_pair(

/// True when pair indices are in range and distinct (B4.5 deepen pass).
bool contact_pair_has_valid_indices(
    const RigidBodySoA& bodies);

/// True when both bodies have collision shapes (B4.5 deepen pass).
bool contact_pair_has_shapes(

/// Diagnostic reason finalize rejects a manifold (B4.5 deepen pass).
enum class ManifoldFinalizeRejectReason : u8 {
    None = 0,
    Empty,
    InvalidNormal,
    NoPenetratingPoints,
    PruneWouldEmpty,

/// Human-readable label for finalize reject diagnostics (B4.5 deepen pass).
const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason);

/// Const preflight for finalize dispatch (B4.5 deepen pass).
    ManifoldFinalizeRejectReason reason = ManifoldFinalizeRejectReason::None;

    bool can_finalize() const { return !rejected; }

/// Populate finalize preflight without mutating slots (B4.5 deepen pass).
ManifoldFinalizePreflight preflight_finalize_contact_manifold(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Returns true when `generate_contact_manifold` would clear and return false (B4.4 deepen pass).
bool should_skip_contact_manifold_finalize(
/// Empty-set guard: true when narrowphase has no bodies or no collision shapes (B4.4 deepen pass).
bool is_empty_narrowphase_input(

/// True when narrowphase may early-out before pair dispatch (B4.4 deepen pass).
bool can_skip_narrowphase_for_empty_input(

/// Returns true when `preflight.rejected` is set (B4.4 deepen pass).
bool contact_pair_was_rejected(const ContactPairPreflight& preflight);

    bool allSeparated = false;

        return !skipped && !empty && !invalidNormal && !allSeparated;

/// Populate finalize preflight without mutating the manifold (B4.4 deepen pass).

/// Returns true when finalize should be skipped before mutation (B4.4 deepen pass).
bool should_skip_finalize_contact_manifold(const ContactManifold& manifold);

/// Guarded finalize: preflight then `generate_contact_manifold` (B4.4 deepen pass).
bool generate_contact_manifold_guarded(ContactManifold& manifold);

/// Guarded pair detect: preflight then `detect_contacts_pair` (B4.4 deepen pass).
ContactManifold detect_contacts_pair_guarded(
/// Body-index overload mirroring broadphase reject diagnostics (B4.5 deepen pass).
ContactPairRejectReason contact_pair_reject_reason(
    u32 bodyA,
    u32 bodyB,

/// Returns true when narrowphase may dispatch this pair (B4.5 deepen pass).
bool is_contact_pair_dispatchable(

/// Explicit reject guard; inverse of `is_contact_pair_dispatchable` (B4.5 deepen pass).

/// Returns true when `preflight.reason` matches `expected` (B4.5 deepen pass).
bool contact_pair_preflight_matches_reason(

/// Count pairs rejected before narrowphase dispatch (B4.5 deepen pass).
u32 count_rejected_contact_pairs(

/// Count pairs that pass contact-pair preflight (B4.5 deepen pass).
/// First finalize reject reason, or `None` when finalize may proceed (B4.5 deepen pass).
ManifoldFinalizeRejectReason manifold_finalize_reject_reason(

/// Returns true when finalize should be skipped (B4.5 deepen pass).
bool should_skip_finalize_contact_manifold(

/// Finalize only when preflight passes; returns false without mutation on reject (B4.5 deepen pass).

/// Combined prune+finalize pipeline preflight (B4.5 deepen pass).
struct ManifoldPruneFinalizePreflight {
    ManifoldPrunePreflight prune{};
    ManifoldFinalizePreflight finalize{};

    bool can_finalize_after_prune() const {
        return !skipped && finalize.can_finalize() &&
               (!prune.needs_pruning() || !prune.wouldBeEmpty);

/// Combined prune+finalize preflight without mutation (B4.5 deepen pass).
ManifoldPruneFinalizePreflight preflight_manifold_prune_finalize(

/// True when prune should be skipped because the manifold is empty or prune would clear all slots (B4.5 deepen pass).
bool should_skip_prune_contact_manifold(
/// Returns true when `preflight.reason` matches `expected` (B4.4 deepen pass).

/// Returns true when the preflight reports a specific reject category (B4.4 deepen pass).
bool contact_pair_preflight_rejects_for_reason(

/// Returns true when preflight indicates dispatch may proceed without shape tests (B4.4 deepen pass).
bool can_dispatch_contact_pair(const ContactPairPreflight& preflight);
/// Returns true when `contact_pair_reject_reason` is not `None` (B4.5 deepen pass).
bool contact_pair_has_reject_reason(

/// Alias for `is_invalid_contact_pair` (B4.5 deepen pass).

/// Richer dispatch preflight with body/shape validity flags (B4.5 deepen pass).
struct ContactPairDispatchPreflight {
    bool has_valid_bodies = false;
    bool has_valid_shapes = false;

    bool can_dispatch() const { return !rejected && has_valid_bodies && has_valid_shapes; }

/// Populate dispatch preflight without running shape dispatch (B4.5 deepen pass).
ContactPairDispatchPreflight preflight_contact_pair_dispatch(

/// Dispatch outcome for guarded pair entry points (B4.5 deepen pass).
    ContactPairDispatchPreflight preflight{};
    bool dispatched = false;

/// Run preflight then shape dispatch when allowed (B4.5 deepen pass).
ContactPairDispatchResult dispatch_contact_pair(
/// Populate reject preflight with per-reason flags (B4.5 deepen pass).
/// Per-pair narrowphase dispatch outcome (skip vs detect) for parallel batch stubs (B4.4 deepen pass).

/// Guarded pair dispatch with explicit skip/detect outcome (B4.4 deepen pass).
ContactPairDispatchResult detect_contacts_pair_guarded(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when the pair should be rejected before dispatch (B4.5 deepen pass).

/// Combined dispatch preflight with reject flags and skip guard (B4.5 deepen pass).
    ContactPairRejectPreflight reject{};

    bool can_dispatch() const { return !skipped && reject.can_dispatch(); }
};

/// Populate combined pair dispatch preflight (B4.5 deepen pass).
/// Guarded pair dispatch returning skip/detect outcome without mutating on reject (B4.4 deepen pass).
ContactPairDispatchResult detect_contacts_pair_result(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Why narrowphase pair dispatch would early-out (B4.4 deepen follow-up pass).
enum class NarrowphaseRejectReason : u8 {
    None = 0,
    EmptyPairList,
    AllPairsRejected,
};

/// Human-readable label for narrowphase batch reject reasons (logging / tests).
const char* narrowphase_reject_reason_name(NarrowphaseRejectReason reason);

/// Diagnose why narrowphase would skip; vacuously succeeds when dispatch may proceed.
NarrowphaseRejectReason narrowphase_reject_reason(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when `narrowphase_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool narrowphase_rejects_for_reason(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    NarrowphaseRejectReason expected);

/// Read-only narrowphase pair-list diagnostics — no mutation (B4.4 deepen follow-up pass).
struct NarrowphasePairListPreflight {
    NarrowphaseRejectReason reason = NarrowphaseRejectReason::None;
    bool emptyPairList = false;
    bool allPairsRejected = false;
    u32 dispatchablePairCount = 0;

    bool can_dispatch() const { return reason == NarrowphaseRejectReason::None; }
};

/// Populate pair-list preflight without running shape dispatch (B4.4 deepen follow-up pass).
NarrowphasePairListPreflight preflight_narrowphase_pair_list(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Count pairs that pass extended preflight dispatch (B4.4 deepen follow-up pass).
u32 count_dispatchable_contact_pairs(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when at least one pair passes extended preflight (B4.4 deepen follow-up pass).
bool has_dispatchable_contact_pair(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Read-only narrowphase batch diagnostics — no mutation (B4.4 deepen follow-up pass).
struct NarrowphasePreflight {
    u32 totalPairs = 0;
    u32 rejectedPairs = 0;
    u32 dispatchablePairs = 0;

    bool can_run() const { return dispatchablePairs > 0u; }
};

/// Populate batch narrowphase preflight without running shape dispatch (B4.4 deepen follow-up pass).
NarrowphasePreflight preflight_narrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.5 deepen follow-up).
bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Returns true when both shapes resolve to planes (B4.5 deepen follow-up).
bool is_plane_plane_contact_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes);

/// Returns true when both bodies have zero static and dynamic friction (B4.5 deepen follow-up).
bool is_zero_friction_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies);

/// Count pairs that pass deepen preflight without rejection (B4.5 deepen follow-up).
u32 count_dispatchable_contact_pairs(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Const batch preflight for narrowphase dispatch (B4.5 deepen follow-up).
struct NarrowphaseBatchPreflight {
    u32 totalPairs = 0;
    u32 rejectedCount = 0;
    u32 dispatchableCount = 0;
    bool allRejected = false;

    bool can_dispatch() const { return dispatchableCount > 0; }
    bool can_skip() const { return allRejected; }
};

/// Populate batch preflight without running shape dispatch (B4.5 deepen follow-up).
NarrowphaseBatchPreflight preflight_narrowphase_batch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Inverse of `can_skip_narrowphase` (B4.5 deepen follow-up).
bool should_run_narrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.4 deepen pass).
bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Count pairs that pass extended deepen preflight (B4.4 deepen pass).
u32 count_dispatchable_contact_pairs_deepen(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Const preflight for narrowphase pair-list dispatch (B4.4 deepen pass).
struct NarrowphasePairListPreflight {
    u32 totalPairs = 0u;
    u32 dispatchableCount = 0u;
    u32 rejectedCount = 0u;

    bool can_dispatch() const { return dispatchableCount > 0u; }
    bool can_skip() const { return totalPairs == 0u || dispatchableCount == 0u; }
};

/// Populate pair-list preflight without running shape dispatch (B4.4 deepen pass).
NarrowphasePairListPreflight preflight_narrowphase_pair_list(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating narrowphase predicate — inverse of `can_skip_narrowphase` (B4.4 deepen pass).
bool should_run_narrowphase_dispatch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Per-batch narrowphase pair dispatch counts (B4.5 deepen follow-up).
struct NarrowphasePairBatchStats {
    u32 totalPairs = 0u;
    u32 dispatchablePairs = 0u;
    u32 rejectedPairs = 0u;
};

/// Count dispatchable vs rejected pairs without running shape dispatch (B4.5 deepen follow-up).
NarrowphasePairBatchStats compute_narrowphase_pair_stats(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Convenience count of pairs that pass deepen preflight (B4.5 deepen follow-up).
u32 count_dispatchable_contact_pairs(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Const batch preflight for narrowphase pair dispatch (B4.5 deepen follow-up).
struct NarrowphasePairBatchPreflight {
    NarrowphasePairBatchStats stats{};
    bool allRejected = false;
    bool hasDispatchable = false;

    bool can_skip_batch() const { return allRejected; }
    bool can_dispatch_any() const { return hasDispatchable; }
};

/// Populate batch pair preflight without running shape dispatch (B4.5 deepen follow-up).
NarrowphasePairBatchPreflight preflight_narrowphase_pairs(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Run shape dispatch only when deepen preflight allows; invalid manifold otherwise (B4.5 deepen follow-up).
ContactManifold detect_contacts_pair_if_needed(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when both bodies carry zero inverse mass (B4.5 deepen pass).
bool is_zero_inv_mass_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies);

/// Returns true when the resolved shape types have no collider dispatch path (B4.5 deepen pass).
bool is_undispatched_shape_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes);

/// Further extended reject reason including zero-inv-mass and undispatched collider pairs (B4.5 deepen pass).
/// Does not alter `contact_pair_deepen_reject_reason`; use for additive preflight only.
ContactPairRejectReason contact_pair_deepen2_reject_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Const preflight with zero-inv-mass and undispatched collider reject checks (B4.5 deepen pass).
struct ContactPairDeepen2Preflight {
    ContactPairRejectReason reason = ContactPairRejectReason::None;
    bool rejected = false;

    bool can_dispatch() const { return reason == ContactPairRejectReason::None; }
};

/// Populate second deepen pair preflight without running shape dispatch (B4.5 deepen pass).
ContactPairDeepen2Preflight preflight_contact_pair_deepen2(
/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Populate extended pair preflight without running shape dispatch (B4.4 deepen follow-up).
ContactPairDeepenPreflight preflight_contact_pair_deepen(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when second deepen preflight rejects this pair (B4.5 deepen pass).
bool should_skip_contact_pair_deepen2_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// True when all pairs are rejected by second deepen preflight or the pair list is empty (B4.5 deepen pass).
bool can_skip_narrowphase_deepen2(
/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.4 deepen pass).
bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Non-mutating deepen dispatch predicate — inverse of `should_skip_contact_pair_deepen_dispatch` (B4.4 deepen pass).
bool should_run_contact_pair_deepen_dispatch(
/// Non-mutating pair-dispatch predicate — inverse of `should_skip_contact_pair_dispatch` (B4.4 deepen pass).
bool should_run_contact_pair_dispatch(
/// Non-mutating deepen dispatch predicate — inverse of `should_skip_contact_pair_deepen_dispatch` (B4.4 deepen guard pass).
/// Non-mutating deepen skip predicate — mirrors `should_skip_contact_pair_deepen_dispatch` (B4.4 deepen pass).
bool can_skip_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating deepen-dispatch predicate — inverse of `should_skip_contact_pair_deepen_dispatch` (B4.4 deepen pass).
/// Non-mutating deepen predicate — inverse of `can_skip_contact_pair_deepen_dispatch` (B4.4 deepen pass).
bool should_run_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.4 deepen guard pass).
bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Non-mutating deepen dispatch predicate — inverse of `should_skip_contact_pair_deepen_dispatch` (B4.4 deepen guard pass).
bool should_run_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// True when all pairs are rejected by extended preflight or the pair list is empty (B4.4 deepen follow-up).
bool can_skip_narrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when both shapes resolve to plane types (B4.4 deepen follow-up pass).
bool is_plane_plane_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes);

/// Returns true when at least one pair passes extended preflight (B4.4 deepen follow-up pass).
bool has_dispatchable_contact_pairs(
/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool contact_pair_deepen_rejects_for_reason(
/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.5 deepen follow-up).
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Inverse of `can_skip_narrowphase` (B4.4 deepen follow-up pass).
bool can_run_narrowphase(
/// Aggregate dispatch counts for narrowphase batch guards (B4.4 deepen follow-up pass).
struct NarrowphaseBatchStats {
    u32 totalPairs = 0;
    u32 dispatchableCount = 0;
    u32 rejectedCount = 0;
};

/// Const preflight for narrowphase batch dispatch (B4.4 deepen follow-up pass).
struct NarrowphaseBatchPreflight {
    NarrowphaseBatchStats stats{};
    bool skipped = false;

    bool can_dispatch() const { return !skipped && stats.dispatchableCount > 0u; }

/// Populate batch preflight without running shape dispatch (B4.4 deepen follow-up pass).
NarrowphaseBatchPreflight preflight_narrowphase_batch(
    const std::vector<broadphase::CandidatePair>& pairs,

/// Const batch preflight for narrowphase pair dispatch (B4.4 deepen follow-up pass).
struct ContactPairBatchPreflight {
    u32 totalPairs = 0u;
    u32 pairCount = 0u;
    u32 rejectedCount = 0u;
    u32 dispatchableCount = 0u;

    bool can_dispatch() const { return !skipped && dispatchableCount > 0u; }

ContactPairBatchPreflight preflight_contact_pair_batch(
    bool can_run() const { return !skipped && dispatchableCount > 0u; }

/// Per-batch narrowphase pair dispatch counts (B4.5 deepen follow-up).
struct NarrowphasePairBatchStats {
    u32 dispatchablePairs = 0u;
    u32 rejectedPairs = 0u;

/// Count dispatchable vs rejected pairs without running shape dispatch (B4.5 deepen follow-up).
NarrowphasePairBatchStats compute_narrowphase_pair_stats(

/// Run shape dispatch with extended deepen reject guards (B4.4 deepen follow-up pass).
/// Does not alter `detect_contacts_pair`; use for additive preflight dispatch only.
/// Run shape dispatch only when base preflight allows; invalid manifold otherwise (B4.4 deepen follow-up pass).
ContactManifold detect_contacts_pair_if_valid(

/// Run shape dispatch only when extended deepen preflight allows (B4.4 deepen follow-up pass).
ContactManifold detect_contacts_pair_deepen(
/// Convenience count of pairs that pass deepen preflight (B4.5 deepen follow-up).
u32 count_dispatchable_contact_pairs(

/// Const batch preflight for narrowphase pair dispatch (B4.5 deepen follow-up).
struct NarrowphasePairBatchPreflight {
    NarrowphasePairBatchStats stats{};
    bool allRejected = false;
    bool hasDispatchable = false;

    bool can_skip_batch() const { return allRejected; }
    bool can_dispatch_any() const { return hasDispatchable; }

/// Populate batch pair preflight without running shape dispatch (B4.5 deepen follow-up).
NarrowphasePairBatchPreflight preflight_narrowphase_pairs(

/// Run shape dispatch only when deepen preflight allows; invalid manifold otherwise (B4.5 deepen follow-up).
ContactManifold detect_contacts_pair_if_needed(
/// Count pairs that pass extended deepen preflight (B4.4 deepen follow-up pass).

/// Count pairs rejected by extended deepen preflight (B4.4 deepen follow-up pass).
u32 count_rejected_contact_pairs_deepen(

/// Why narrowphase batch dispatch would early-out (B4.4 deepen follow-up pass).
/// Why narrowphase pair dispatch would early-out (B4.4 deepen follow-up pass).
enum class NarrowphaseRejectReason : u8 {
    None = 0,
    EmptyPairList,
    AllPairsRejected,
};

/// Human-readable label for narrowphase batch reject reasons (logging / tests).
const char* narrowphase_reject_reason_name(NarrowphaseRejectReason reason);

/// Diagnose why narrowphase batch would skip; vacuously succeeds when dispatch may proceed.
NarrowphaseRejectReason narrowphase_reject_reason(

/// Returns true when `narrowphase_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool narrowphase_rejects_for_reason(
/// Diagnose why narrowphase batch dispatch would skip; vacuously succeeds when any pair may dispatch.
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

    const CollisionShapeSoA& shapes,
    NarrowphaseRejectReason expected);

/// Read-only narrowphase batch diagnostics — no mutation (B4.4 deepen follow-up pass).
struct NarrowphasePreflight {
    NarrowphaseRejectReason reason = NarrowphaseRejectReason::None;
    bool emptyPairList = false;
    bool allPairsRejected = false;
    u32 dispatchablePairs = 0;
    u32 rejectedPairs = 0;

    bool can_dispatch() const { return reason == NarrowphaseRejectReason::None; }

/// Populate narrowphase batch preflight without running shape dispatch (B4.4 deepen follow-up pass).
NarrowphasePreflight preflight_narrowphase(

/// Non-mutating narrowphase skip predicate — mirrors `can_skip_narrowphase` (B4.4 deepen follow-up pass).
bool can_skip_narrowphase_preflight(

/// True when at least one pair passes extended deepen preflight (B4.4 deepen follow-up pass).
bool has_dispatchable_contact_pairs(

/// Returns true when extended preflight rejects this pair (B4.5 deepen follow-up).
bool is_invalid_contact_pair_deepen(

/// Inverse of `is_invalid_contact_pair_deepen` (B4.5 deepen follow-up).
bool is_valid_contact_pair_deepen(

/// Returns true when either body has negative inverse mass (B4.5 deepen follow-up).
bool is_negative_inverse_mass_pair(
    const RigidBodySoA& bodies);

/// Returns true when both bodies have non-positive inverse mass (B4.5 deepen follow-up).
bool is_zero_mass_contact_pair(

/// Returns true when one body is sleeping and the other is kinematic (B4.5 deepen follow-up).
bool is_sleeping_kinematic_mix_pair(
struct NarrowphaseBatchPreflight {
    u32 pairCount = 0;
    u32 rejectedCount = 0;
    u32 dispatchableCount = 0;

    bool can_dispatch() const {
        return reason == NarrowphaseRejectReason::None && dispatchableCount > 0u;
    }
};

/// Populate batch narrowphase preflight without running shape dispatch (B4.4 deepen follow-up pass).
NarrowphaseBatchPreflight preflight_narrowphase_batch(
/// Diagnose why narrowphase would skip; vacuously succeeds when dispatch may proceed.


/// Read-only narrowphase pair-list diagnostics — no mutation (B4.4 deepen follow-up pass).
struct NarrowphasePairListPreflight {
    u32 dispatchablePairCount = 0;


/// Populate pair-list preflight without running shape dispatch (B4.4 deepen follow-up pass).
NarrowphasePairListPreflight preflight_narrowphase_pair_list(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating batch skip predicate — mirrors `can_skip_narrowphase` (B4.4 deepen follow-up pass).
bool can_skip_narrowphase_dispatch(

/// Detect contacts only when deepen preflight allows dispatch; invalid manifold otherwise (B4.4 deepen follow-up pass).
ContactManifold detect_contacts_pair_if_needed(
    const broadphase::CandidatePair& pair,
/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.5 deepen follow-up).
bool contact_pair_deepen_rejects_for_reason(
/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.4 deepen pass).
/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.5 deepen pass).
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Per-batch narrowphase pair dispatch counts (B4.5 deepen follow-up).
struct NarrowphasePairBatchStats {
    u32 totalPairs = 0u;
    u32 dispatchablePairs = 0u;
    u32 rejectedPairs = 0u;
};

/// Count dispatchable vs rejected pairs without running shape dispatch (B4.5 deepen follow-up).
NarrowphasePairBatchStats compute_narrowphase_pair_stats(
/// Non-mutating narrowphase predicate — inverse of `can_skip_narrowphase` (B4.4 deepen pass).
bool should_run_narrowphase(
/// Returns true when extended deepen preflight allows dispatch (B4.4 deepen pass).
bool can_dispatch_contact_pair_deepen(
    const CollisionShapeSoA& shapes);

/// Non-mutating deepen dispatch predicate — mirrors `preflight_contact_pair_deepen` (B4.4 deepen pass).
bool should_run_contact_pair_deepen_dispatch(

/// Count pairs rejected by extended deepen preflight (B4.4 deepen pass).
u32 count_rejected_contact_pairs(
    const std::vector<broadphase::CandidatePair>& pairs,

/// Const batch preflight for narrowphase pair dispatch (B4.5 deepen follow-up).
struct NarrowphasePairBatchPreflight {
    NarrowphasePairBatchStats stats{};
    bool allRejected = false;
    bool hasDispatchable = false;

    bool can_skip_batch() const { return allRejected; }
    bool can_dispatch_any() const { return hasDispatchable; }

/// Populate batch pair preflight without running shape dispatch (B4.5 deepen follow-up).
NarrowphasePairBatchPreflight preflight_narrowphase_pairs(

/// Run shape dispatch only when deepen preflight allows; invalid manifold otherwise (B4.5 deepen follow-up).


bool should_run_narrowphase_dispatch(

/// Returns true when both resolved shapes are planes (B4.4 deepen pass).
bool is_plane_plane_contact_pair(

/// Returns true when either plane shape has a degenerate normal vector (B4.4 deepen pass).
bool is_invalid_plane_normal_pair(

/// Returns true when resolved shapes are not bound to the pair body indices (B4.4 deepen pass).
bool is_shape_body_mismatch_contact_pair(

/// Extended deepen-pass reject checks beyond `contact_pair_deepen_reject_reason` (B4.4 deepen pass).
/// Does not alter `contact_pair_deepen_reject_reason`; use for additive preflight only.
ContactPairRejectReason contact_pair_deepen_pass_reject_reason(

/// Const preflight with deepen-pass plane/mismatch reject checks (B4.4 deepen pass).
struct ContactPairDeepenPassPreflight {
    ContactPairRejectReason reason = ContactPairRejectReason::None;
    bool rejected = false;

    bool can_dispatch() const { return !rejected; }

/// Populate deepen-pass pair preflight without running shape dispatch (B4.4 deepen pass).
ContactPairDeepenPassPreflight preflight_contact_pair_deepen_pass(

/// Returns true when deepen-pass preflight rejects this pair (B4.4 deepen pass).
bool should_skip_contact_pair_deepen_pass_dispatch(

/// Read-only batch diagnostics for pair-list narrowphase launch (B4.4 deepen pass).
struct ContactPairBatchPreflight {
    u32 dispatchableCount = 0u;
    u32 rejectedCount = 0u;
    bool emptyInput = false;

    bool can_dispatch() const { return dispatchableCount > 0u; }

/// Populate batch pair preflight without running shape dispatch (B4.4 deepen pass).
ContactPairBatchPreflight preflight_contact_pair_batch(

/// Returns true when no pair passes deepen-pass preflight (B4.4 deepen pass).
bool should_skip_contact_pair_batch(
/// Const preflight for batch narrowphase dispatch (B4.4 deepen pass).
struct NarrowphaseBatchPreflight {
    u32 pairCount = 0u;
    bool canSkip = false;

    bool has_dispatchable() const { return dispatchableCount > 0u; }

/// Populate batch narrowphase preflight without running shape dispatch (B4.4 deepen pass).
NarrowphaseBatchPreflight preflight_narrowphase_batch(

/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.4 deepen guard pass).
/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.4 guard pass).
/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.4 deepen pass follow-up).
bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Read-only batch narrowphase diagnostics — no mutation (B4.4 deepen pass).

    bool allRejected() const { return totalPairs > 0u && dispatchableCount == 0u; }

/// Populate batch preflight without running shape dispatch (B4.4 deepen pass).

/// Non-mutating pair dispatch predicate — inverse of `should_skip_contact_pair_deepen_dispatch` (B4.4 deepen pass).

struct NarrowphaseDispatchPreflight {
    bool skipped = false;
    u32 dispatchableCount = 0;

    bool can_dispatch() const { return !skipped && dispatchableCount > 0u; }

/// Populate batch narrowphase dispatch preflight without running shape dispatch (B4.4 deepen pass).
/// Inverse of `should_skip_contact_pair_deepen_dispatch` (B4.4 deepen follow-up pass).
bool is_dispatchable_contact_pair(

/// Inverse of `can_skip_narrowphase` (B4.4 deepen follow-up pass).

/// Const preflight for batch narrowphase dispatch (B4.4 deepen follow-up pass).

    bool can_skip_dispatch() const { return pairCount == 0u || dispatchableCount == 0u; }

/// Populate batch narrowphase preflight without running shape dispatch (B4.4 deepen follow-up pass).
NarrowphaseDispatchPreflight preflight_narrowphase_dispatch(
/// Count pairs rejected by extended deepen preflight (B4.5 deepen pass).
u32 count_rejected_contact_pairs_deepen(

/// Const batch preflight for deepen pair dispatch (B4.5 deepen pass).
struct ContactPairBatchDeepenPreflight {

    bool can_dispatch_any() const { return dispatchableCount > 0u; }

/// Populate batch deepen preflight without running shape dispatch (B4.5 deepen pass).
ContactPairBatchDeepenPreflight preflight_contact_pair_batch_deepen(
/// Inverse of `should_skip_contact_pair_deepen_dispatch` (B4.4 deepen pass follow-up).
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Run shape dispatch with extended deepen preflight reject checks (B4.4 deepen pass follow-up).
/// Does not alter `detect_contacts_pair`; base path may still dispatch sleeping/trigger pairs.
ContactManifold detect_contacts_pair_deepen(

/// Copy pairs that pass extended deepen preflight (B4.4 deepen pass follow-up).
std::vector<broadphase::CandidatePair> filter_dispatchable_contact_pairs(
/// Why narrowphase batch dispatch would early-out (B4.6 deepen pass).
enum class NarrowphaseBatchRejectReason : u8 {
    None = 0,
    EmptyPairList,
    AllRejected,
};
/// Returns true when both shapes resolve to capsule types (B4.5 deepen pass).
bool is_capsule_capsule_contact_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes);

/// Returns true when one shape is a box and the other is a capsule (B4.5 deepen pass).
bool is_box_capsule_contact_pair(

/// Const preflight for narrowphase batch dispatch (B4.5 deepen follow-up pass).
struct NarrowphaseBatchPreflight {
    NarrowphaseBatchRejectReason reason = NarrowphaseBatchRejectReason::None;
    u32 pairCount = 0u;
    u32 dispatchableCount = 0u;
    u32 rejectedCount = 0u;

    bool can_dispatch() const { return dispatchableCount > 0u; }
    bool can_skip() const { return reason != NarrowphaseBatchRejectReason::None; }
    bool can_run() const { return reason == NarrowphaseBatchRejectReason::None && can_dispatch(); }

/// Populate batch preflight without running shape dispatch (B4.5 deepen follow-up pass).
NarrowphaseBatchPreflight preflight_narrowphase_batch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating batch skip predicate — inverse of `has_dispatchable_contact_pair` (B4.4 deepen pass).
bool narrowphase_batch_all_rejected(

/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.4 deepen pass).
bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Non-mutating deepen dispatch predicate — inverse of `should_skip_contact_pair_deepen_dispatch` (B4.4 deepen pass).
/// Non-mutating deepen dispatch predicate — inverse of `should_skip_contact_pair_deepen_dispatch` (B4.4 deepen guard pass).
bool should_run_contact_pair_deepen_dispatch(

/// Non-mutating deepen dispatch skip predicate — mirrors `should_skip_contact_pair_deepen_dispatch` (B4.4 deepen pass).
bool can_skip_contact_pair_deepen_dispatch(
/// Returns true when either shape is a plane with a non-unit normal (B4.5 deepen pass).
bool is_unnormalized_plane_shape_pair(
    f32 lengthEpsilon = 1e-4f);

/// Returns true when either shape extent is positive but below the deepen epsilon (B4.5 deepen pass).
bool is_near_degenerate_shape_pair(
    f32 extentEpsilon = 1e-4f);
/// Shape dispatch with extended deepen preflight guard; invalid manifold when deepen-rejected (B4.4 deepen follow-up pass).
ContactManifold detect_contacts_pair_deepen(
/// Non-mutating pair-dispatch predicate — inverse of `should_skip_contact_pair_dispatch` (B4.4 guard pass).
bool should_run_contact_pair_dispatch(

/// Non-mutating pair-dispatch skip predicate — mirrors `preflight_contact_pair` (B4.4 guard pass).
bool can_skip_contact_pair_dispatch(

/// Non-mutating deepen-dispatch predicate — inverse of `should_skip_contact_pair_deepen_dispatch` (B4.4 guard pass).

/// Non-mutating deepen-dispatch skip predicate — mirrors `preflight_contact_pair_deepen` (B4.4 guard pass).
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Run shape dispatch only when extended deepen preflight passes (B4.6 deepen pass).
ContactManifold detect_contacts_pair_deepen(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when `preflight_contact_pair_deepen` matches `expected` (B4.6 deepen pass).
bool contact_pair_deepen_preflight_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Index of the first pair passing deepen preflight, or `pairs.size()` when none (B4.6 deepen pass).
u32 first_dispatchable_contact_pair_index(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when extended deepen preflight allows dispatch (B4.6 deepen pass).
bool should_run_contact_pair_deepen_dispatch(
struct ContactBufferSoA;

/// Run shape dispatch with extended deepen preflight reject checks (B4.6 deepen pass).
ContactManifold detect_contacts_pair_deepen(
bool should_dispatch_contact_pair_deepen(
/// Const preflight for per-slot narrowphase dispatch (B4.6 deepen pass).
struct NarrowphasePairSlotPreflight {
    u32 slot = 0u;
/// Combined base+deepen reject reason for additive preflight (B4.5 deepen pass).
ContactPairRejectReason contact_pair_union_reject_reason(
/// Run shape dispatch only when extended deepen preflight passes (B4.6 deepen pass).
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Const preflight merging base and deepen pair guards (B4.5 deepen pass).
struct ContactPairUnionPreflight {
    ContactPairRejectReason baseReason = ContactPairRejectReason::None;
    ContactPairRejectReason deepenReason = ContactPairRejectReason::None;
/// Finalize manifold with prune+finalize preflight gates (B4.6 deepen pass).
bool generate_contact_manifold_deepen(ContactManifold& manifold);

/// Const preflight for per-slot narrowphase dispatch (B4.6 deepen pass).
struct NarrowphasePairSlotPreflight {
    u32 slot = 0u;
    ContactPairRejectReason reason = ContactPairRejectReason::None;
    bool rejected = false;

    bool can_dispatch() const { return !rejected; }
};

/// Populate per-slot narrowphase preflight without running shape dispatch (B4.6 deepen pass).
NarrowphasePairSlotPreflight preflight_narrowphase_pair_slot(
    u32 slot,
/// Const preflight for per-pair narrowphase dispatch using deepen reject checks (B4.6 deepen pass).
struct ContactPairDispatchPreflight {
    ContactPairDeepenPreflight deepen{};

    bool can_dispatch() const { return !rejected && deepen.can_dispatch(); }

/// Populate per-pair dispatch preflight without running shape dispatch (B4.6 deepen pass).
ContactPairDispatchPreflight preflight_contact_pair_dispatch(
/// Run shape dispatch only when extended deepen preflight passes (B4.3 deepen follow-up pass).
/// Run shape dispatch only when extended deepen preflight passes (B4.6 deepen pass).
ContactManifold detect_contacts_pair_with_preflight(
/// Count pairs rejected by extended deepen preflight (B4.5 deepen pass).
u32 count_rejected_contact_pairs(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when extended deepen preflight rejects this pair (B4.5 deepen pass).
bool contact_pair_deepen_rejected(
/// Returns true when extended preflight would reject before buffer slot write (B4.6 deepen pass).
bool should_skip_contact_pair_for_buffer(
    const broadphase::CandidatePair& pair,

/// Detect contacts only when extended deepen preflight passes; invalid manifold otherwise (B4.6 deepen pass).

/// Returns true when both shapes resolve to capsule types (B4.6 deepen pass).
bool is_capsule_capsule_contact_pair(

/// Returns true when one shape is a box and the other is a plane (B4.6 deepen pass).
bool is_box_plane_contact_pair(

/// First deepen reject reason in a pair list, or `None` when at least one pair may dispatch (B4.6 deepen pass).
ContactPairRejectReason first_contact_pair_deepen_reject_in_batch(
/// Returns the first deepen reject reason in `pairs`, or `None` when all may dispatch (B4.6 deepen pass).
ContactPairRejectReason first_contact_pair_deepen_reject_reason(
ContactManifold detect_contacts_pair_with_deepen_preflight(

/// Collect pairs that pass extended deepen preflight without mutating input (B4.6 deepen pass).
std::vector<broadphase::CandidatePair> filter_dispatchable_contact_pairs(

/// Non-mutating deepen-dispatch skip predicate — mirrors `should_skip_contact_pair_deepen_dispatch` (B4.6 deepen pass).
bool can_skip_contact_pair_deepen_dispatch(

/// Non-mutating deepen-dispatch predicate — inverse of `can_skip_contact_pair_deepen_dispatch` (B4.6 deepen pass).

/// Human-readable label for narrowphase batch reject reasons (B4.6 deepen pass).
const char* narrowphase_batch_reject_reason_name(NarrowphaseBatchRejectReason reason);

/// Diagnose why batch dispatch would skip; vacuously succeeds when dispatch may proceed (B4.6 deepen pass).
NarrowphaseBatchRejectReason narrowphase_batch_reject_reason(

/// Returns true when `narrowphase_batch_reject_reason` matches `expected` (B4.6 deepen pass).
bool narrowphase_batch_rejects_for_reason(
    const CollisionShapeSoA& shapes,
    NarrowphaseBatchRejectReason expected);

/// Non-mutating batch skip predicate — mirrors `can_skip_narrowphase` (B4.6 deepen pass).
bool can_skip_narrowphase_batch(

/// Non-mutating batch predicate — inverse of `can_skip_narrowphase_batch` (B4.6 deepen pass).
bool should_run_narrowphase_batch(

/// Const preflight for narrowphase run dispatch (B4.6 deepen pass).
struct NarrowphaseRunPreflight {
    u32 pairCount = 0u;
    bool emptyInput = false;
    NarrowphaseBatchPreflight batch{};
    bool canSkip = false;

    bool can_run() const { return !canSkip && !emptyInput; }

/// Populate run preflight without mutating buffers or running shape dispatch (B4.6 deepen pass).
NarrowphaseRunPreflight preflight_run_narrowphase(

/// Returns true when narrowphase run should early-out before pair dispatch (B4.6 deepen pass).
bool can_skip_narrowphase_run(

/// Run shape dispatch only when deepen preflight allows; returns invalid manifold when rejected (B4.6 deepen pass).
/// Returns true when `first_contact_pair_deepen_reject_reason` matches `expected` (B4.6 deepen pass).
bool first_contact_pair_deepen_rejects_for_reason(
    ContactPairRejectReason expected);

/// Shape dispatch only when deepen preflight passes; invalid manifold otherwise (B4.6 deepen pass).

    bool skipped = false;

    bool can_run() const { return !skipped && batch.can_dispatch(); }

/// Populate run preflight without mutating buffers (B4.6 deepen pass).
NarrowphaseRunPreflight preflight_narrowphase_run(

/// Returns true when narrowphase run should skip all pair dispatch (B4.6 deepen pass).
bool should_skip_narrowphase_run(
/// Returns true when a finalized manifold may be written to a contact-buffer slot (B4.6 deepen pass).
bool can_write_contact_manifold_to_buffer(const ContactManifold& manifold);

/// Write a finalized manifold to a buffer slot only when preflight allows (B4.6 deepen pass).
bool write_contact_manifold_to_buffer_with_preflight(
    ContactBufferSoA& buffer,
    const ContactManifold& manifold);
/// Count pairs rejected by a specific deepen reason (B4.6 deepen pass).
u32 count_contact_pairs_rejected_for_reason(
    ContactPairRejectReason reason);

    bool canWriteSlot = false;


/// Populate per-slot dispatch preflight without running shape dispatch (B4.6 deepen pass).

/// Returns true when narrowphase should skip this pair slot before dispatch (B4.6 deepen pass).
bool should_skip_narrowphase_pair_slot(
/// Returns true when per-slot narrowphase should skip dispatch (B4.6 deepen pass).
/// Returns true when per-pair dispatch preflight rejects this pair (B4.6 deepen pass).
bool should_skip_contact_pair_dispatch_preflight(
/// Run shape dispatch only when pair preflight allows; returns invalid manifold when skipped (B4.6 deepen follow-up pass).

/// Finalize manifold with prune+finalize preflight gates (B4.6 deepen pass).
bool generate_contact_manifold_deepen(ContactManifold& manifold);
/// Finalize only when manifold finalize preflight passes; no-op otherwise (B4.3 deepen follow-up pass).
bool generate_contact_manifold_with_preflight(
    ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 frictionEpsilon = 1e-4f);
/// Count pairs rejected by extended deepen preflight (B4.6 deepen pass).

/// Returns true when narrowphase batch should be skipped before dispatch (B4.6 deepen pass).
bool should_skip_narrowphase_batch(
/// Finalize only when `preflight_manifold_finalize` passes; no-op otherwise (B4.6 deepen pass).
bool finalize_contact_manifold_if_needed(ContactManifold& manifold);
/// Detect contacts only when extended deepen preflight allows dispatch (B4.6 deepen pass).
ContactManifold detect_contacts_pair_if_dispatchable(
/// Run shape dispatch only when extended deepen preflight allows (B4.6 deepen pass).
/// Populate union preflight without running shape dispatch (B4.5 deepen pass).
ContactPairUnionPreflight preflight_contact_pair_union(

    u32 slot,
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Per-slot narrowphase write preflight combining pair reject + manifold validity (B4.6 deepen pass).
struct NarrowphaseSlotPreflight {
    ContactPairRejectReason pairReason = ContactPairRejectReason::None;
    bool pairRejected = false;
    bool canDetect = false;
    bool canWrite = false;

    bool can_dispatch() const { return canDetect && !pairRejected; }
};

/// Populate slot preflight without running shape dispatch or buffer write (B4.6 deepen pass).
NarrowphaseSlotPreflight preflight_narrowphase_slot(
/// Finalize only when deepen preflight and finalize preflight both pass (B4.6 deepen pass).
bool generate_contact_manifold_with_deepen_preflight(ContactManifold& manifold);

} // namespace fuse::physics::narrowphase
