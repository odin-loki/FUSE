#pragma once

#include <fuse/physics/config.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/narrowphase/friction.hpp>
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
    RestingPair,
    UndispatchableShapePair,
    NonCanonicalPair,
    DuplicatePairInBatch,
    BothNoGravity,
    BothCcd,
    MeshShapePair,
    BoxThinPair,
    NoDispatchPath,
    UnsupportedMeshPair,
    DegeneratePlaneNormal,
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

/// Returns true when both bodies carry `RB_NO_GRAVITY` (B4.6 deepen pass).
bool is_no_gravity_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies);

/// Returns true when both bodies carry `RB_CCD` (B4.6 deepen pass).
bool is_ccd_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies);

/// Returns true when either shape resolves to `SdfMesh` or `Voxel` (B4.6 deepen pass).
bool is_mesh_shape_contact_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes);

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

/// Run shape dispatch with deepen pair-reject guards; valid pairs unchanged (B4.6 deepen pass).
ContactManifold detect_contacts_pair_deepen(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Finalize detected manifold with deepen preflight guards (B4.6 deepen pass).
bool generate_contact_manifold_deepen(ContactManifold& manifold);

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
/// Returns true when one shape is a box and the other is a plane (B4.6 deepen pass).
bool is_box_plane_contact_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes);

/// Returns true when one shape is a capsule and the other is a plane (B4.6 deepen pass).
bool is_capsule_plane_contact_pair(

/// Returns true when both shapes resolve to capsule types (B4.6 deepen pass).
bool is_capsule_capsule_contact_pair(

/// Returns true when one shape is a box and the other is a capsule (B4.6 deepen pass).

/// Returns true when `dispatchShapePair` has no narrowphase path despite passing base checks (B4.6 deepen pass).
bool is_undispatched_shape_pair(

/// Run shape dispatch only when extended deepen preflight allows (B4.6 deepen pass).
ContactManifold detect_contacts_pair_deepen(
    const RigidBodySoA& bodies,
/// Returns true when either shape is an SdfMesh or Voxel type (B4.6 deepen pass).
bool is_mesh_shape_contact_pair(

/// Returns true when either plane shape has a zero-length normal (B4.6 deepen pass).
bool is_degenerate_plane_normal_pair(

/// Returns true when the resolved shape types have no narrowphase dispatch handler (B4.6 deepen pass).

/// Returns true when shapes pass base checks but have no narrowphase dispatch path (B4.6 deepen follow-up pass).

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

/// Run shape dispatch only when extended deepen preflight passes (B4.6 deepen pass).

/// Returns true when `preflight_contact_pair_deepen` matches `expected` (B4.6 deepen pass).
bool contact_pair_deepen_preflight_rejects_for_reason(

/// Index of the first pair passing deepen preflight, or `pairs.size()` when none (B4.6 deepen pass).
u32 first_dispatchable_contact_pair_index(

/// Returns true when extended deepen preflight allows dispatch (B4.6 deepen pass).
struct ContactBufferSoA;

/// Run shape dispatch with extended deepen preflight reject checks (B4.6 deepen pass).
bool should_dispatch_contact_pair_deepen(
/// Const preflight for per-slot narrowphase dispatch (B4.6 deepen pass).
struct NarrowphasePairSlotPreflight {
    u32 slot = 0u;
/// Combined base+deepen reject reason for additive preflight (B4.5 deepen pass).
ContactPairRejectReason contact_pair_union_reject_reason(
/// Const preflight for guarded pair detection (B4.6 deepen pass).
struct ContactPairDetectPreflight {
    ContactPairDeepenPreflight deepen{};
    bool can_detect = false;

    bool can_dispatch() const { return deepen.can_dispatch(); }
};

/// Populate detect preflight without running shape dispatch (B4.6 deepen pass).
ContactPairDetectPreflight preflight_detect_contacts_pair(

/// Const preflight merging base and deepen pair guards (B4.5 deepen pass).
struct ContactPairUnionPreflight {
    ContactPairRejectReason baseReason = ContactPairRejectReason::None;
    ContactPairRejectReason deepenReason = ContactPairRejectReason::None;
/// Finalize manifold with prune+finalize preflight gates (B4.6 deepen pass).
bool generate_contact_manifold_deepen(ContactManifold& manifold);

    ContactPairRejectReason reason = ContactPairRejectReason::None;
    bool rejected = false;

    bool can_dispatch() const { return !rejected; }

/// Populate per-slot narrowphase preflight without running shape dispatch (B4.6 deepen pass).
NarrowphasePairSlotPreflight preflight_narrowphase_pair_slot(
    u32 slot,
/// Const preflight for per-pair narrowphase dispatch using deepen reject checks (B4.6 deepen pass).
struct ContactPairDispatchPreflight {

    bool can_dispatch() const { return !rejected && deepen.can_dispatch(); }

/// Populate per-pair dispatch preflight without running shape dispatch (B4.6 deepen pass).
ContactPairDispatchPreflight preflight_contact_pair_dispatch(
/// Run shape dispatch only when extended deepen preflight passes (B4.3 deepen follow-up pass).
ContactManifold detect_contacts_pair_with_preflight(
/// Count pairs rejected by extended deepen preflight (B4.5 deepen pass).
u32 count_rejected_contact_pairs(
/// Returns true when the pair is non-responsive (sleeping, static, kinematic, or massless on both sides, B4.6 deepen pass).
bool is_resting_contact_pair(
    f32 invMassEpsilon = 1e-8f);

/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_pair_deepen_rejects_for_reason_v2(

/// Collect pairs that pass extended deepen preflight (B4.6 deepen pass).
std::vector<broadphase::CandidatePair> filter_dispatchable_contact_pairs(

bool preflight_contact_pair_deepen_rejects_for_reason(

/// Const preflight for narrowphase dispatch entry (B4.6 deepen pass).
struct NarrowphaseDispatchPreflight {
    NarrowphaseBatchPreflight batch{};
    bool can_skip_dispatch = false;

    bool can_dispatch() const { return batch.can_dispatch(); }

/// Populate dispatch preflight without running narrowphase (B4.6 deepen pass).
NarrowphaseDispatchPreflight preflight_narrowphase_dispatch(

/// Returns true when extended deepen preflight rejects this pair (B4.5 deepen pass).
bool contact_pair_deepen_rejected(
/// Returns true when extended preflight would reject before buffer slot write (B4.6 deepen pass).
bool should_skip_contact_pair_for_buffer(

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

/// Non-mutating deepen-dispatch skip predicate — mirrors `should_skip_contact_pair_deepen_dispatch` (B4.6 deepen pass).

/// Non-mutating deepen-dispatch predicate — inverse of `can_skip_contact_pair_deepen_dispatch` (B4.6 deepen pass).

/// Human-readable label for narrowphase batch reject reasons (B4.6 deepen pass).
const char* narrowphase_batch_reject_reason_name(NarrowphaseBatchRejectReason reason);

/// Diagnose why batch dispatch would skip; vacuously succeeds when dispatch may proceed (B4.6 deepen pass).
NarrowphaseBatchRejectReason narrowphase_batch_reject_reason(

/// Returns true when `narrowphase_batch_reject_reason` matches `expected` (B4.6 deepen pass).
bool narrowphase_batch_rejects_for_reason(
    NarrowphaseBatchRejectReason expected);

/// Non-mutating batch skip predicate — mirrors `can_skip_narrowphase` (B4.6 deepen pass).
bool can_skip_narrowphase_batch(

/// Non-mutating batch predicate — inverse of `can_skip_narrowphase_batch` (B4.6 deepen pass).
bool should_run_narrowphase_batch(

/// Const preflight for narrowphase run dispatch (B4.6 deepen pass).
struct NarrowphaseRunPreflight {
    bool emptyInput = false;
    bool canSkip = false;

    bool can_run() const { return !canSkip && !emptyInput; }

/// Populate run preflight without mutating buffers or running shape dispatch (B4.6 deepen pass).
NarrowphaseRunPreflight preflight_run_narrowphase(

/// Returns true when narrowphase run should early-out before pair dispatch (B4.6 deepen pass).
bool can_skip_narrowphase_run(

/// Run shape dispatch only when deepen preflight allows; returns invalid manifold when rejected (B4.6 deepen pass).
/// Returns true when `first_contact_pair_deepen_reject_reason` matches `expected` (B4.6 deepen pass).
bool first_contact_pair_deepen_rejects_for_reason(

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


/// Per-slot narrowphase write preflight combining pair reject + manifold validity (B4.6 deepen pass).
struct NarrowphaseSlotPreflight {
    ContactPairRejectReason pairReason = ContactPairRejectReason::None;
    bool pairRejected = false;
    bool canDetect = false;
    bool canWrite = false;

    bool can_dispatch() const { return canDetect && !pairRejected; }

/// Populate slot preflight without running shape dispatch or buffer write (B4.6 deepen pass).
NarrowphaseSlotPreflight preflight_narrowphase_slot(
/// Finalize only when deepen preflight and finalize preflight both pass (B4.6 deepen pass).
bool generate_contact_manifold_with_deepen_preflight(ContactManifold& manifold);

/// Per-reason counts from extended deepen preflight over a batch (B4.6 deepen pass).
struct ContactPairBatchRejectSummary {
    u32 restingCount = 0u;
    u32 planePlaneCount = 0u;

    bool all_rejected() const { return pairCount > 0u && dispatchableCount == 0u; }

/// Populate batch reject summary without running shape dispatch (B4.6 deepen pass).
ContactPairBatchRejectSummary summarize_contact_pair_batch_rejects(

/// Returns true when dispatch preflight reports no dispatchable pairs (B4.6 deepen pass).
bool should_skip_narrowphase_dispatch(

/// Returns true when both shapes are capsules (no narrowphase dispatch stub, B4.6 deepen pass).

/// Returns true when the resolved shape types have no `dispatchShapePair` path (B4.6 deepen pass).
bool is_undispatchable_shape_pair(

/// Extended reject reason including undispatchable shape combinations (B4.6 deepen pass).
/// Does not alter `contact_pair_deepen_reject_reason`; use for additive preflight only.
ContactPairRejectReason contact_pair_beyond_deepen_reject_reason(

/// Const preflight with beyond deepen undispatchable checks (B4.6 deepen pass).
struct ContactPairBeyondDeepenPreflight {


/// Populate beyond deepen pair preflight without running shape dispatch (B4.6 deepen pass).
ContactPairBeyondDeepenPreflight preflight_contact_pair_beyond(

/// Returns true when beyond deepen preflight rejects this pair (B4.6 deepen pass).
bool should_skip_contact_pair_beyond_dispatch(

/// Returns true when `contact_pair_beyond_deepen_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_pair_beyond_rejects_for_reason(

/// Count pairs that pass beyond deepen preflight (B4.6 deepen pass).
u32 count_beyond_dispatchable_contact_pairs(

/// True when at least one pair passes beyond deepen preflight (B4.6 deepen pass).
bool has_beyond_dispatchable_contact_pair(

/// Const preflight for beyond narrowphase batch dispatch (B4.6 deepen pass).
struct NarrowphaseBeyondBatchPreflight {
/// Why narrowphase batch dispatch would early-out (B4.5 deepen pass).
enum class NarrowphaseDispatchRejectReason : u8 {
    None = 0,
    EmptyPairList,
    AllPairsRejected,

/// Human-readable label for narrowphase dispatch reject reasons (B4.5 deepen pass).
const char* narrowphase_dispatch_reject_reason_name(NarrowphaseDispatchRejectReason reason);

/// Diagnose why batch dispatch would skip; vacuously succeeds when dispatch may proceed (B4.5 deepen pass).
NarrowphaseDispatchRejectReason narrowphase_dispatch_reject_reason(

/// Returns true when `narrowphase_dispatch_reject_reason` matches `expected` (B4.5 deepen pass).
bool narrowphase_dispatch_rejects_for_reason(
    NarrowphaseDispatchRejectReason expected);

    NarrowphaseDispatchRejectReason reason = NarrowphaseDispatchRejectReason::None;
    u32 pairCount = 0u;
    u32 dispatchableCount = 0u;
    u32 rejectedCount = 0u;

    bool can_dispatch() const { return reason == NarrowphaseDispatchRejectReason::None && dispatchableCount > 0u; }
    bool can_skip() const { return reason != NarrowphaseDispatchRejectReason::None || dispatchableCount == 0u; }
};

/// Populate beyond batch preflight without running shape dispatch (B4.6 deepen pass).
NarrowphaseBeyondBatchPreflight preflight_narrowphase_beyond_batch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when beyond batch preflight reports no dispatchable pairs (B4.6 deepen pass).
bool narrowphase_beyond_batch_rejects_all(
/// Returns true when `bodyA` is greater than `bodyB` (non-canonical broadphase ordering) (B4.6 deepen follow-up pass).
bool is_swapped_contact_pair(const broadphase::CandidatePair& pair);

/// Returns true when `bodyA` is less than or equal to `bodyB` (B4.6 deepen follow-up pass).
bool is_canonical_contact_pair(const broadphase::CandidatePair& pair);

/// Returns a pair with the lower body index first (B4.6 deepen follow-up pass).
broadphase::CandidatePair canonicalize_contact_pair(const broadphase::CandidatePair& pair);

/// Count pairs rejected by extended deepen preflight (B4.6 deepen follow-up pass).
u32 count_rejected_contact_pairs(
/// Const preflight with B4.6 deepen reject checks layered on batch preflight (B4.6 deepen pass).
struct NarrowphaseBatchDeepenPreflight {
    NarrowphaseBatchPreflight base{};
    u32 noGravityRejectedCount = 0u;
    u32 ccdRejectedCount = 0u;

    bool can_dispatch() const { return base.can_dispatch(); }
    bool can_skip_deepen() const { return base.can_skip(); }
};

/// Populate B4.6 batch deepen preflight without running shape dispatch (B4.6 deepen pass).
NarrowphaseBatchDeepenPreflight preflight_narrowphase_batch_deepen(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Run shape dispatch only when beyond deepen preflight allows (B4.6 deepen pass).
ContactManifold detect_contacts_pair_beyond(
    const broadphase::CandidatePair& pair,
/// Returns true when `bodyA` is greater than `bodyB` (non-canonical ordering, B4.6 deepen pass).
bool is_non_canonical_contact_pair(const broadphase::CandidatePair& pair);

/// Returns true when an identical pair appears earlier in the batch (B4.6 deepen pass).
bool is_duplicate_contact_pair_in_batch(
    u32 pairIndex);

/// Extended reject reason including canonical-order and batch-duplicate checks (B4.6 deepen pass).
/// Does not alter `contact_pair_deepen_reject_reason`; use for additive preflight only.
ContactPairRejectReason contact_pair_deepen_pass_reject_reason(
    const CollisionShapeSoA& shapes,

/// Const preflight with canonical-order and batch-duplicate reject checks (B4.6 deepen pass).
struct ContactPairDeepenPassPreflight {
    ContactPairRejectReason reason = ContactPairRejectReason::None;
    bool rejected = false;
/// Const preflight for `detect_contacts_pair` guarded dispatch (B4.6 deepen follow-up pass).
struct ContactPairDispatchPreflight {
    bool usesDeepenReject = false;
/// Returns true when either shape resolves to mesh/voxel types (B4.6 narrowphase deepen pass).
bool is_mesh_shape_contact_pair(

/// Returns true when either box shape has a positive but near-zero extent (B4.6 narrowphase deepen pass).
bool is_box_thin_shape_pair(
    f32 thinExtentEpsilon = 1e-4f);

/// Extended reject reason including plane-plane and mesh/thin-box pairs (B4.6 narrowphase deepen pass).
ContactPairRejectReason contact_pair_deepen_second_reject_reason(

/// Const preflight with second-layer plane/mesh/thin-box reject checks (B4.6 narrowphase deepen pass).
struct ContactPairDeepenSecondPreflight {

    bool can_dispatch() const { return !rejected; }
};

/// Populate deepen-pass pair preflight without running shape dispatch (B4.6 deepen pass).
ContactPairDeepenPassPreflight preflight_contact_pair_deepen_pass(

/// Returns true when deepen-pass preflight rejects this pair (B4.6 deepen pass).
bool should_skip_contact_pair_deepen_pass_dispatch(

/// Combined base + deepen-pass preflight for one pair slot (B4.6 deepen pass).
struct ContactPairSlotPreflight {
    ContactPairPreflight base{};
    ContactPairDeepenPreflight deepen{};
    ContactPairDeepenPassPreflight deepenPass{};


/// Populate slot preflight without running shape dispatch (B4.6 deepen pass).
ContactPairSlotPreflight preflight_contact_pair_slot(

/// Run shape dispatch only when slot preflight allows; invalid manifold otherwise (B4.6 deepen pass).
ContactManifold detect_contacts_pair_with_preflight(

/// Count pairs rejected by deepen-pass preflight (B4.6 deepen pass).
u32 count_deepen_pass_rejected_contact_pairs(
/// Non-mutating pair-dispatch predicate — inverse of `should_skip_contact_pair_dispatch` (B4.5 deepen pass).
bool should_run_contact_pair_dispatch(

/// Non-mutating deepen pair-dispatch predicate — inverse of `should_skip_contact_pair_deepen_dispatch` (B4.5 deepen pass).
bool should_run_contact_pair_deepen_dispatch(

/// Non-mutating batch skip predicate — mirrors `narrowphase_batch_rejects_all` (B4.5 deepen pass).
bool can_skip_narrowphase_batch(

/// Non-mutating batch predicate — inverse of `can_skip_narrowphase_batch` (B4.5 deepen pass).
bool should_run_narrowphase_batch(
/// Non-mutating deepen dispatch predicate — inverse of `should_skip_contact_pair_deepen_dispatch` (B4.6 deepen pass).

/// Non-mutating batch predicate — inverse of `narrowphase_batch_rejects_all` (B4.6 deepen pass).
/// Populate detect dispatch preflight without running shape dispatch (B4.6 deepen follow-up pass).
ContactPairDispatchPreflight preflight_detect_contacts_pair(
    bool useDeepenReject = false);

/// Returns true when `detect_contacts_pair` should early-out (B4.6 deepen follow-up pass).
bool should_skip_detect_contacts_pair(

/// Run shape dispatch only when preflight allows; returns invalid manifold otherwise (B4.6 deepen follow-up pass).
/// Run shape dispatch only when extended deepen preflight passes (B4.6 deepen pass).
ContactManifold detect_contacts_pair_deepen(

/// Finalize manifold with prune+finalize preflight gates (B4.6 deepen pass).
bool generate_contact_manifold_deepen(ContactManifold& manifold);
/// Returns true when all pairs are rejected by B4.6 deepen preflight (B4.6 deepen pass).
bool narrowphase_batch_deepen_rejects_all(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating pair-dispatch predicate — inverse of `should_skip_contact_pair_dispatch` (B4.6 deepen pass).
/// Populate second-layer pair preflight without running shape dispatch (B4.6 narrowphase deepen pass).
ContactPairDeepenSecondPreflight preflight_contact_pair_deepen_second(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating deepen-dispatch predicate — inverse of `should_skip_contact_pair_deepen_dispatch` (B4.6 deepen pass).
bool should_run_contact_pair_deepen_dispatch(
/// Returns true when second-layer preflight rejects this pair (B4.6 narrowphase deepen pass).
bool should_skip_contact_pair_deepen_second_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating narrowphase predicate — inverse of `can_skip_narrowphase` (B4.6 deepen pass).
bool should_run_narrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating batch-dispatch predicate — inverse of `NarrowphaseBatchPreflight::can_skip` (B4.6 deepen pass).
bool should_run_narrowphase_batch(

/// Run shape dispatch only when extended deepen preflight passes (B4.6 deepen pass).
ContactManifold detect_contacts_pair_deepen(
    const broadphase::CandidatePair& pair,

/// Finalize manifold with prune+finalize preflight gates (B4.6 deepen pass).
bool generate_contact_manifold_deepen(ContactManifold& manifold);
/// Returns true when `contact_pair_deepen_second_reject_reason` matches `expected` (B4.6 narrowphase deepen pass).
bool contact_pair_deepen_second_rejects_for_reason(
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Count pairs that pass second-layer deepen preflight (B4.6 narrowphase deepen pass).
u32 count_dispatchable_contact_pairs_second(

/// True when at least one pair passes second-layer deepen preflight (B4.6 narrowphase deepen pass).
bool has_dispatchable_contact_pair_second(

/// Const preflight for second-layer narrowphase batch dispatch (B4.6 narrowphase deepen pass).
struct NarrowphaseBatchSecondPreflight {
    u32 pairCount = 0u;
    u32 dispatchableCount = 0u;
    u32 rejectedCount = 0u;

    bool can_dispatch() const { return dispatchableCount > 0u; }
    bool can_skip() const { return pairCount == 0u || dispatchableCount == 0u; }
};

/// Populate second-layer batch preflight without running shape dispatch (B4.6 narrowphase deepen pass).
NarrowphaseBatchSecondPreflight preflight_narrowphase_batch_second(

/// Returns true when second-layer batch preflight reports no dispatchable pairs (B4.6 narrowphase deepen pass).
bool narrowphase_batch_second_rejects_all(

/// Const preflight for per-pair narrowphase dispatch with deepen guards (B4.6 deepen pass).
struct NarrowphasePairDispatchPreflight {
    ContactPairRejectReason reason = ContactPairRejectReason::None;
    bool rejected = false;

    bool can_dispatch() const { return !rejected; }
};

/// Populate per-pair dispatch preflight using deepen reject checks (B4.6 deepen pass).
NarrowphasePairDispatchPreflight preflight_narrowphase_pair_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating skip predicate — true when deepen preflight rejects this pair (B4.6 deepen pass).
bool can_skip_narrowphase_pair_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating dispatch predicate — inverse of `can_skip_narrowphase_pair_dispatch` (B4.6 deepen pass).
bool should_run_narrowphase_pair_dispatch(
/// Shape dispatch with extended deepen preflight rejection (B4.5 deepen pass).
/// Returns an invalid manifold when deepen preflight rejects the pair.
/// Returns true when deepen preflight rejects but base preflight allows (B4.6 deepen pass).
bool is_deepen_only_rejected_contact_pair(
/// Returns true when extended deepen preflight allows dispatch (B4.6 deepen pass).
bool is_dispatchable_contact_pair(
/// Run shape dispatch only when extended deepen preflight allows; invalid manifold otherwise (B4.6 deepen pass).
ContactManifold detect_contacts_pair_with_deepen_preflight(
/// Inverse of `should_skip_contact_pair_dispatch` (B4.5 deepen follow-up pass).
/// Returns true when base pair preflight allows shape dispatch (B4.6 deepen pass).
bool should_run_contact_pair_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when both base and deepen preflight reject the pair (B4.6 deepen pass).
bool is_fully_rejected_contact_pair(

/// Shape dispatch guarded by extended deepen preflight; invalid when deepen rejects (B4.6 deepen pass).
ContactManifold detect_contacts_pair_deepen(

/// Shape dispatch guarded by deepen preflight; invalid manifold when rejected (B4.6 deepen pass).
ContactManifold detect_contacts_pair_with_deepen_preflight(

/// Run shape dispatch for one pair after extended deepen preflight (B4.6 deepen follow-up pass).

/// Returns true when `preflight_narrowphase_batch` reports the expected dispatchable count (B4.6 deepen follow-up pass).
bool narrowphase_batch_has_dispatchable_count(
    const std::vector<broadphase::CandidatePair>& pairs,
    const CollisionShapeSoA& shapes,
    u32 expectedCount);
/// Count pairs whose deepen reject reason matches `expected` (B4.6 deepen pass).
u32 count_contact_pairs_rejected_for_reason(
    ContactPairRejectReason expected);

/// Returns true when every pair in the batch matches `expected` deepen reject reason (B4.6 deepen pass).
bool narrowphase_batch_all_reject_for_reason(
/// Finalize friction tangents only when preflight allows; no-op when skipped (B4.5 deepen pass).
bool compute_friction_tangents_with_preflight(ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Returns true when batch preflight reports at least one rejected pair (B4.6 deepen pass).
bool narrowphase_batch_has_rejected_pairs(
/// Finalize only when deepen dispatch and finalize preflight pass (B4.6 deepen pass).
bool generate_contact_manifold_deepen(ContactManifold& manifold);
/// Returns first deepen-rejected pair reason in batch, or `None` when all pairs dispatchable (B4.6 deepen pass).
ContactPairRejectReason narrowphase_batch_first_reject_reason(
/// Run shape dispatch only when extended deepen preflight passes; invalid manifold otherwise (B4.6 deepen pass).

/// Finalize only when manifold finalize preflight passes; no-op otherwise (B4.6 deepen pass).
bool generate_contact_manifold_with_preflight(ContactManifold& manifold);
bool generate_contact_manifold_with_preflight(
    ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 frictionEpsilon = 1e-4f);
/// Inverse of `should_skip_contact_pair_deepen_dispatch` (B4.5 deepen follow-up pass).
/// Returns true when extended deepen preflight allows shape dispatch (B4.6 deepen pass).
bool should_run_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Inverse of `can_skip_narrowphase` (B4.5 deepen follow-up pass).
bool should_run_narrowphase(

/// Detect using extended deepen preflight gate; additive API (B4.5 deepen follow-up pass).
/// Returns true when batch preflight reports at least one dispatchable pair (B4.6 deepen pass).
bool should_run_narrowphase_batch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Why detect_contacts_pair would early-out before shape dispatch (B4.6 deepen pass).
enum class ContactPairDetectRejectReason : u8 {
    None = 0,
    RejectedPair,
};

/// Const preflight for detect_contacts_pair dispatch (B4.6 deepen pass).
struct ContactPairDetectPreflight {
    ContactPairRejectReason rejectReason = ContactPairRejectReason::None;
    ContactPairDetectRejectReason reason = ContactPairDetectRejectReason::None;
    bool rejected = false;

    bool can_detect() const { return !rejected; }
};

/// Human-readable label for detect_contacts_pair reject reasons (B4.6 deepen pass).
inline const char* contact_pair_detect_reject_reason_name(ContactPairDetectRejectReason reason) {
    switch (reason) {
    case ContactPairDetectRejectReason::None:
        return "None";
    case ContactPairDetectRejectReason::RejectedPair:
        return "RejectedPair";
    }
    return "Unknown";
}

/// Diagnose why detect_contacts_pair would skip; vacuously succeeds when detect may proceed (B4.6 deepen pass).
inline ContactPairDetectRejectReason contact_pair_detect_reject_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (is_invalid_contact_pair(pair, bodies, shapes)) {
        return ContactPairDetectRejectReason::RejectedPair;
    }
    return ContactPairDetectRejectReason::None;
}

/// Returns true when `contact_pair_detect_reject_reason` matches `expected` (B4.6 deepen pass).
inline bool contact_pair_detect_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairDetectRejectReason expected) {
    return contact_pair_detect_reject_reason(pair, bodies, shapes) == expected;
}

/// Populate detect preflight without running shape dispatch (B4.6 deepen pass).
inline ContactPairDetectPreflight preflight_detect_contacts_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    ContactPairDetectPreflight preflight{};
    preflight.rejectReason = contact_pair_reject_reason(pair, bodies, shapes);
    preflight.reason = contact_pair_detect_reject_reason(pair, bodies, shapes);
    preflight.rejected = preflight.reason != ContactPairDetectRejectReason::None;
    return preflight;
}

/// Returns true when detect_contacts_pair should skip before shape dispatch (B4.6 deepen pass).
inline bool can_skip_detect_contacts_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return contact_pair_detect_reject_reason(pair, bodies, shapes) !=
           ContactPairDetectRejectReason::None;
}

/// Returns true when both-plane pair would be rejected by deepen preflight (B4.6 deepen pass).
inline bool contact_pair_deepen_rejects_plane_plane(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes) {
    return is_plane_plane_contact_pair(pair, shapes) && is_unsupported_shape_pair(pair, shapes);
}

/// Returns true when extended deepen preflight allows shape dispatch (B4.6 deepen pass).
inline bool should_run_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

/// Detect contacts only when deepen preflight allows; invalid manifold otherwise (B4.6 deepen pass).
inline ContactManifold detect_contacts_pair_with_preflight(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {
        return invalidContactManifold();
    }
    return detect_contacts_pair(pair, bodies, shapes);
}

/// Returns true when base pair preflight allows dispatch (B4.6 deepen pass).
inline bool should_run_contact_pair_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_dispatch(pair, bodies, shapes);
}

/// Run shape dispatch only when extended deepen preflight passes; invalid manifold otherwise (B4.6 deepen pass).
FUSE_PHYSICS_INLINE ContactManifold detect_contacts_pair_with_deepen_preflight(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {
        return invalidContactManifold();
    }
    return detect_contacts_pair(pair, bodies, shapes);
}

/// Finalize only when manifold finalize preflight passes; no-op otherwise (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool generate_contact_manifold_with_preflight(ContactManifold& manifold) {
    return finalize_contact_manifold_with_preflight(manifold);
}

/// Returns true when pair detect should be skipped before shape dispatch (B4.6 deepen follow-up pass).
bool can_skip_detect_contacts_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Detect contacts only when preflight allows; returns invalid manifold when skipped (B4.6 deepen follow-up pass).
ContactManifold detect_contacts_pair_with_preflight(
/// Inverse of `should_skip_contact_pair_deepen_dispatch` (B4.6 deepen pass).
bool is_valid_contact_pair_deepen(
/// Non-mutating pair-dispatch skip predicate for base preflight (B4.6 deepen follow-up pass).
bool can_skip_contact_pair_dispatch(
/// Const preflight for single-pair detect dispatch (B4.6 deepen pass).
struct ContactPairDetectPreflight {
    ContactPairDeepenPreflight pair{};
    bool rejected = false;

    bool can_detect() const { return !rejected && pair.can_dispatch(); }
};

/// Populate detect preflight without running shape dispatch (B4.6 deepen pass).
ContactPairDetectPreflight preflight_detect_contacts_pair(
/// Count pairs rejected by extended deepen preflight (B4.6 deepen follow-up pass).
u32 count_rejected_contact_pairs(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when narrowphase batch dispatch should run (B4.6 deepen follow-up pass).
bool should_run_narrowphase_batch(

/// Run shape dispatch only when extended deepen preflight allows (B4.6 deepen follow-up pass).
/// Run shape dispatch only when pair preflight allows (B4.3 deepen pass).
ContactManifold detect_contacts_pair_with_preflight(
/// Non-mutating pair-dispatch skip predicate — mirrors `should_skip_contact_pair_dispatch` (B4.5 deepen follow-up pass).
bool would_skip_contact_pair_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when detect should skip this pair (B4.6 deepen pass).
bool can_skip_detect_contacts_pair(

/// Run shape dispatch only when deepen preflight passes; invalid manifold otherwise (B4.6 deepen pass).

/// Finalize only when `can_finalize_contact_manifold` passes; no-op otherwise (B4.6 deepen follow-up pass).
bool generate_contact_manifold_with_preflight(ContactManifold& manifold);

/// Returns true when manifold generation should be skipped (B4.6 deepen follow-up pass).
bool can_skip_generate_contact_manifold(const ContactManifold& manifold);

/// Extended deepen reject including plane-plane diagnostic path (B4.6 deepen follow-up pass).
ContactPairRejectReason contact_pair_deepen_followup_reject_reason(
/// Detect contacts only when deepen preflight allows; invalid manifold otherwise (B4.6 deepen pass).
ContactManifold detect_contacts_pair_with_deepen_preflight(
/// Non-mutating pair-dispatch predicate — mirrors `preflight_contact_pair` (B4.6 deepen follow-up pass).
bool should_run_contact_pair_dispatch(

/// Returns true when extended deepen follow-up rejects this pair (B4.6 deepen follow-up pass).
bool should_skip_contact_pair_deepen_followup_dispatch(
/// Non-mutating deepen-dispatch skip predicate (B4.6 deepen follow-up pass).
bool can_skip_contact_pair_deepen_dispatch(

/// Build friction tangents only when preflight allows; no-op when skipped (B4.6 deepen follow-up pass).
void compute_friction_tangents_with_preflight(ContactManifold& manifold, f32 epsilon = 1e-4f);

FUSE_PHYSICS_INLINE bool can_skip_detect_contacts_pair(
/// Non-mutating pair-dispatch skip predicate — mirrors `should_skip_contact_pair_dispatch` (B4.5 deepen follow-up pass).
inline bool wouldSkipContactPairDispatch(
inline bool would_skip_contact_pair_dispatch(
/// Alias for `should_skip_contact_pair_dispatch` (B4.6 deepen pass).
/// Non-mutating deepen pair-dispatch skip predicate — mirrors `should_skip_contact_pair_deepen_dispatch` (B4.5 deepen follow-up pass).
bool would_skip_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating batch skip predicate — mirrors `narrowphase_batch_rejects_all` (B4.5 deepen follow-up pass).
bool would_skip_narrowphase_batch(
    const std::vector<broadphase::CandidatePair>& pairs,

/// Detect contacts only when base pair preflight allows; returns invalid manifold when skipped (B4.5 deepen follow-up pass).
ContactManifold try_detect_contacts_pair(

/// Detect contacts only when deepen pair preflight allows; returns invalid manifold when skipped (B4.5 deepen follow-up pass).
ContactManifold try_detect_contacts_pair_deepen(

/// Finalize only when `can_finalize_contact_manifold` passes; no-op otherwise (B4.5 deepen follow-up pass).
bool try_generate_contact_manifold(ContactManifold& manifold);

} // namespace fuse::physics::narrowphase

#include <fuse/physics/config.hpp>

namespace fuse::physics::narrowphase {

FUSE_PHYSICS_INLINE bool would_skip_contact_pair_dispatch(
    const CollisionShapeSoA& shapes) {
    return should_skip_contact_pair_dispatch(pair, bodies, shapes);
}

FUSE_PHYSICS_INLINE ContactManifold detect_contacts_pair_with_preflight(
    if (can_skip_detect_contacts_pair(pair, bodies, shapes)) {
        return invalidContactManifold();
    return detect_contacts_pair(pair, bodies, shapes);

FUSE_PHYSICS_INLINE bool can_skip_generate_contact_manifold(const ContactManifold& manifold) {
    return !can_finalize_contact_manifold(manifold);

FUSE_PHYSICS_INLINE bool generate_contact_manifold_with_preflight(ContactManifold& manifold) {
    return generate_contact_manifold_if_needed(manifold);

FUSE_PHYSICS_INLINE ContactPairRejectReason contact_pair_deepen_followup_reject_reason(
    const ContactPairRejectReason deepenReason = contact_pair_deepen_reject_reason(pair, bodies, shapes);
    if (deepenReason != ContactPairRejectReason::None) {
        return deepenReason;
    if (is_plane_plane_contact_pair(pair, shapes)) {
        return ContactPairRejectReason::UnsupportedShapePair;
    return ContactPairRejectReason::None;

FUSE_PHYSICS_INLINE bool should_skip_contact_pair_deepen_followup_dispatch(
    return contact_pair_deepen_followup_reject_reason(pair, bodies, shapes) != ContactPairRejectReason::None;

FUSE_PHYSICS_INLINE void compute_friction_tangents_with_preflight(ContactManifold& manifold, f32 epsilon) {
    if (should_skip_friction_basis_preflight(manifold, epsilon)) {
        return;
    compute_friction_tangents(manifold);
/// Finalize only when manifold finalize preflight passes; no-op otherwise (B4.6 deepen pass).
bool generate_contact_manifold_with_preflight(
    ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f,
    f32 frictionEpsilon = 1e-4f);
/// Non-mutating deepen-dispatch predicate — mirrors `preflight_contact_pair_deepen` (B4.6 deepen follow-up pass).
bool should_run_contact_pair_deepen_dispatch(

/// Non-mutating narrowphase predicate — inverse of `can_skip_narrowphase` (B4.6 deepen follow-up pass).
bool should_run_narrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,

/// Non-mutating batch predicate — inverse of `narrowphase_batch_rejects_all` (B4.6 deepen follow-up pass).
inline ContactPairDetectPreflight preflight_detect_contacts_pair(
    ContactPairDetectPreflight preflight{};
    preflight.pair = preflight_contact_pair_deepen(pair, bodies, shapes);
    preflight.rejected = preflight.pair.rejected;
    return preflight;

inline bool can_skip_detect_contacts_pair(
    return !preflight_detect_contacts_pair(pair, bodies, shapes).can_detect();

inline ContactManifold detect_contacts_pair_with_preflight(
/// Extended reject reason including plane-plane pairs (B4.6 deepen pass).
/// Does not alter `contact_pair_deepen_reject_reason`; use for additive preflight only.
inline ContactPairRejectReason contact_pair_deepen_pass_reject_reason(
        return ContactPairRejectReason::PlanePlane;
    return contact_pair_deepen_reject_reason(pair, bodies, shapes);

/// Human-readable label for deepen-pass reject reasons (B4.6 deepen pass).
inline const char* contact_pair_deepen_pass_reject_reason_name(ContactPairRejectReason reason) {
    if (reason == ContactPairRejectReason::PlanePlane) {
        return "PlanePlane";
    return contact_pair_reject_reason_name(reason);

/// Returns true when `contact_pair_deepen_pass_reject_reason` matches `expected` (B4.6 deepen pass).
inline bool contact_pair_deepen_pass_rejects_for_reason(
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_pass_reject_reason(pair, bodies, shapes) == expected;

/// Const preflight with plane-plane reject checks (B4.6 deepen pass).
struct ContactPairDeepenPassPreflight {
    ContactPairRejectReason reason = ContactPairRejectReason::None;

    bool can_dispatch() const { return !rejected; }

/// Populate deepen-pass pair preflight without running shape dispatch (B4.6 deepen pass).
inline ContactPairDeepenPassPreflight preflight_contact_pair_deepen_pass(
    ContactPairDeepenPassPreflight preflight{};
    preflight.reason = contact_pair_deepen_pass_reject_reason(pair, bodies, shapes);
    preflight.rejected = preflight.reason != ContactPairRejectReason::None;

/// Returns true when deepen-pass preflight rejects this pair (B4.6 deepen pass).
inline bool should_skip_contact_pair_deepen_pass_dispatch(
    return contact_pair_deepen_pass_reject_reason(pair, bodies, shapes) != ContactPairRejectReason::None;

/// Count pairs that pass deepen-pass preflight (B4.6 deepen pass).
inline u32 count_deepen_pass_dispatchable_contact_pairs(
    u32 dispatchable = 0u;
    for (const broadphase::CandidatePair& pair : pairs) {
        if (!should_skip_contact_pair_deepen_pass_dispatch(pair, bodies, shapes)) {
            ++dispatchable;
    return dispatchable;

/// True when at least one pair passes deepen-pass preflight (B4.6 deepen pass).
inline bool has_deepen_pass_dispatchable_contact_pair(
    if (pairs.empty()) {
        return false;
    return count_deepen_pass_dispatchable_contact_pairs(pairs, bodies, shapes) > 0u;

/// True when all pairs are rejected by deepen-pass preflight or the pair list is empty (B4.6 deepen pass).
inline bool can_skip_narrowphase_deepen_pass(
    return !has_deepen_pass_dispatchable_contact_pair(pairs, bodies, shapes);
/// True when extended preflight allows pair dispatch (B4.3 deepen pass).
/// Run shape dispatch only when extended deepen preflight allows; invalid manifold otherwise (B4.6 deepen pass).

/// Finalize only when deepen preflight and `can_finalize_contact_manifold` pass; no-op otherwise (B4.6 deepen pass).
bool generate_contact_manifold_with_deepen_preflight(ContactManifold& manifold);

/// Returns true when narrowphase batch dispatch should proceed (B4.6 deepen pass).
bool should_run_narrowphase_batch_dispatch(

/// Const preflight for detect_contacts_pair dispatch (B4.5 deepen pass).
struct ContactPairDetectPreflight {
    bool rejected = false;

    bool can_detect() const { return !rejected; }
};

/// Populate detect preflight without running shape dispatch (B4.5 deepen pass).
    preflight.reason = contact_pair_reject_reason(pair, bodies, shapes);

/// Returns true when detect_contacts_pair would return an invalid manifold (B4.5 deepen pass).
inline bool should_skip_detect_contacts_pair(

/// Non-mutating detect skip predicate — mirrors `should_skip_detect_contacts_pair` (B4.5 deepen pass).
    return should_skip_detect_contacts_pair(pair, bodies, shapes);

/// Detect only when base pair preflight passes; invalid manifold otherwise (B4.5 deepen pass).
inline ContactManifold detect_contacts_pair_if_valid(
    if (should_skip_detect_contacts_pair(pair, bodies, shapes)) {

/// Returns true when plane-plane pairs are rejected by base preflight (B4.5 deepen pass).
inline bool contact_pair_rejects_plane_plane(
    return contact_pair_reject_reason(pair, bodies, shapes) ==
           ContactPairRejectReason::UnsupportedShapePair &&
           is_plane_plane_contact_pair(pair, shapes);
/// Returns true when base pair preflight may dispatch (B4.5 deepen pass).
inline bool should_run_contact_pair_dispatch(
    return !should_skip_contact_pair_dispatch(pair, bodies, shapes);

/// Returns true when extended deepen preflight may dispatch (B4.5 deepen pass).
inline bool should_run_contact_pair_deepen_dispatch(
    return !should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
/// Non-mutating deepen pair-dispatch skip predicate — mirrors `should_skip_contact_pair_deepen_dispatch` (B4.5 deepen follow-up pass).
inline bool wouldSkipContactPairDeepenDispatch(
inline bool would_skip_contact_pair_deepen_dispatch(
    return should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);

/// Non-mutating narrowphase skip predicate — mirrors `can_skip_narrowphase` (B4.5 deepen follow-up pass).
inline bool wouldSkipNarrowphase(
    return can_skip_narrowphase(pairs, bodies, shapes);

/// Guarded pair dispatch — returns invalid manifold when preflight rejects (B4.5 deepen follow-up pass).
inline ContactManifold tryDetectContactsPair(
    if (wouldSkipContactPairDispatch(pair, bodies, shapes)) {

/// Guarded manifold finalize — returns false when preflight rejects (B4.5 deepen follow-up pass).
inline bool tryGenerateContactManifold(ContactManifold& manifold) {
    if (!can_finalize_contact_manifold(manifold)) {
    return generate_contact_manifold(manifold);
/// Non-mutating batch skip predicate — mirrors `can_skip_narrowphase` (B4.5 deepen follow-up pass).
inline bool would_skip_narrowphase_batch(

/// Detect contacts only when base preflight allows; returns false when skipped (B4.5 deepen follow-up pass).
inline bool try_detect_contacts_pair(
    ContactManifold& out) {
    if (would_skip_contact_pair_dispatch(pair, bodies, shapes)) {
        out = invalidContactManifold();
    out = detect_contacts_pair(pair, bodies, shapes);
    return out.valid;

/// Detect contacts only when deepen preflight allows; returns false when skipped (B4.5 deepen follow-up pass).
inline bool try_detect_contacts_pair_deepen(
    if (would_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {

/// Finalize manifold only when preflight allows; no-op otherwise (B4.5 deepen follow-up pass).
inline bool try_generate_contact_manifold(ContactManifold& manifold) {
/// Non-mutating pair-dispatch skip predicate with optional reject reason (B4.6 deepen pass).
    ContactPairRejectReason* reason = nullptr) {
    const ContactPairRejectReason rejectReason = contact_pair_reject_reason(pair, bodies, shapes);
    if (reason != nullptr) {
        *reason = rejectReason;
    return rejectReason != ContactPairRejectReason::None;

/// Non-mutating deepen pair-dispatch skip predicate with optional reject reason (B4.6 deepen pass).
    const ContactPairRejectReason rejectReason =
        contact_pair_deepen_reject_reason(pair, bodies, shapes);

/// Detect contacts only when pair preflight allows; returns false when skipped (B4.6 deepen pass).
inline bool tryDetectContactsPair(
    ContactManifold& outManifold) {
        outManifold = invalidContactManifold();
    outManifold = detect_contacts_pair(pair, bodies, shapes);
    return true;

/// Detect contacts only when deepen preflight allows; returns false when skipped (B4.6 deepen pass).
inline bool tryDetectContactsPairDeepen(
    if (wouldSkipContactPairDeepenDispatch(pair, bodies, shapes)) {

/// Finalize manifold only when preflight allows; returns false when skipped (B4.6 deepen pass).

/// Finalize manifold only when `can_finalize_contact_manifold` passes; no-op otherwise (B4.6 deepen pass).
inline bool tryGenerateContactManifoldIfNeeded(ContactManifold& manifold) {
/// Alias for `should_skip_contact_pair_deepen_dispatch` (B4.6 deepen pass).
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
}

/// Alias for `can_skip_narrowphase` (B4.6 deepen pass).
inline bool wouldSkipNarrowphaseBatch(

/// Detect contacts only when pair preflight allows; returns invalid manifold when skipped (B4.6 deepen pass).

/// Finalize manifold only when `can_finalize_contact_manifold` passes; returns false when skipped (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool would_skip_contact_pair_deepen_dispatch(

FUSE_PHYSICS_INLINE bool would_skip_narrowphase_batch(
    return narrowphase_batch_rejects_all(pairs, bodies, shapes);

FUSE_PHYSICS_INLINE ContactManifold try_detect_contacts_pair(

FUSE_PHYSICS_INLINE ContactManifold try_detect_contacts_pair_deepen(

FUSE_PHYSICS_INLINE bool try_generate_contact_manifold(ContactManifold& manifold) {
    return generate_contact_manifold_if_needed(manifold);
}

} // namespace fuse::physics::narrowphase
