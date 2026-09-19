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
    UnsupportedShapePair,
    BothStatic,
    DegenerateShape,
    BothSleeping,
    BothKinematic,
    AnyTrigger,
    BothMassless,
};

/// Human-readable label for diagnostics and test assertions (B4.3 deepen pass).
const char* contact_pair_reject_reason_name(ContactPairRejectReason reason);

/// Returns true when `contact_pair_reject_reason` matches `expected` (B4.3 deepen pass).
bool contact_pair_rejects_for_reason(
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
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies);

/// Returns true when both bodies carry `RB_KINEMATIC` (B4.4 deepen follow-up).
bool is_kinematic_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies);

/// Returns true when either body carries `RB_TRIGGER` (B4.4 deepen pass).
bool is_any_trigger_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies);

/// Returns true when both bodies have non-positive inverse mass (B4.4 deepen pass).
bool is_massless_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    f32 invMassEpsilon = 1e-8f);

/// Returns true when either shape has zero or negative extent (B4.3 deepen pass).
bool is_degenerate_shape_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes);

/// Returns true when the resolved shape types have no narrowphase dispatch path.
bool is_unsupported_shape_pair(
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

    bool can_dispatch() const { return !rejected; }
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

/// Extended reject reason including sleeping/kinematic pairs (B4.4 deepen follow-up).
/// Does not alter `contact_pair_reject_reason`; use for additive preflight only.
ContactPairRejectReason contact_pair_deepen_reject_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Const preflight with extended sleeping/kinematic reject checks (B4.4 deepen follow-up).
struct ContactPairDeepenPreflight {
    ContactPairRejectReason reason = ContactPairRejectReason::None;
    bool rejected = false;

    bool can_dispatch() const { return !rejected; }
};

/// Populate extended pair preflight without running shape dispatch (B4.4 deepen follow-up).
ContactPairDeepenPreflight preflight_contact_pair_deepen(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when extended preflight rejects this pair (B4.4 deepen follow-up).
bool should_skip_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// True when all pairs are rejected by extended preflight or the pair list is empty (B4.4 deepen follow-up).
bool can_skip_narrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Count pairs that pass extended deepen preflight (B4.4 deepen pass).
u32 count_dispatchable_contact_pairs(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// True when at least one pair passes extended deepen preflight (B4.4 deepen pass).
bool has_dispatchable_contact_pair(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Returns true when both shapes resolve to plane types (B4.5 deepen follow-up pass).
bool is_plane_plane_contact_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes);

/// Const preflight for narrowphase batch dispatch (B4.5 deepen follow-up pass).
struct NarrowphaseBatchPreflight {
    u32 pairCount = 0u;
    u32 dispatchableCount = 0u;
    u32 rejectedCount = 0u;

    bool can_dispatch() const { return dispatchableCount > 0u; }
    bool can_skip() const { return pairCount == 0u || dispatchableCount == 0u; }
};

/// Populate batch preflight without running shape dispatch (B4.5 deepen follow-up pass).
NarrowphaseBatchPreflight preflight_narrowphase_batch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Returns true when batch preflight reports no dispatchable pairs (B4.5 deepen follow-up pass).
bool narrowphase_batch_rejects_all(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating pair dispatch predicate — inverse of `should_skip_contact_pair_dispatch` (B4.6 deepen pass).
bool should_run_contact_pair_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating deepen dispatch predicate — inverse of `should_skip_contact_pair_deepen_dispatch` (B4.6 deepen pass).
bool should_run_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Non-mutating batch dispatch predicate — inverse of `narrowphase_batch_rejects_all` (B4.6 deepen pass).
bool should_run_narrowphase_batch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

} // namespace fuse::physics::narrowphase

// --- deepen additive from deepen-b4-narrowphase-manifold-prune-4828 ---
const char* contact_pair_reject_reason_label(ContactPairRejectReason reason);

// --- deepen additive from deepen-b4-narrowphase-guards-56bb ---
    ContactPairPreflight preflight{};
const char* contact_pair_preflight_reason_name(const ContactPairPreflight& preflight);

// --- deepen additive from deepen-b4-narrowphase-guards-d8a9 ---
    const ContactPairPreflight& preflight,
struct ManifoldFinalizePreflight {
    bool wouldBeEmptyAfterPrune = false;
ManifoldFinalizePreflight preflight_finalize_contact_manifold(const ContactManifold& manifold);

// --- deepen additive from b4-narrowphase-deepen-guards-e063 ---
    bool rejected() const { return reason != ContactPairRejectReason::None; }

// --- deepen additive from b4-narrowphase-guards-deepen-2074 ---
struct ContactManifoldFinalizePreflight {
ContactManifoldFinalizePreflight preflight_contact_manifold_finalize(
bool should_skip_contact_manifold_finalize(

// --- deepen additive from deepen-b4-narrowphase-guards-d11b ---
bool contact_pair_was_rejected(const ContactPairPreflight& preflight);
bool should_skip_finalize_contact_manifold(const ContactManifold& manifold);

// --- deepen additive from deepen-b4-narrowphase-guards-d755 ---
struct ContactPairRejectPreflight {
ContactPairRejectPreflight preflight_contact_pair_reject(
enum class ManifoldFinalizeRejectReason : u8 {
const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason);
    ManifoldFinalizeRejectReason reason = ManifoldFinalizeRejectReason::None;
ManifoldFinalizeRejectReason manifold_finalize_reject_reason(
struct ManifoldPruneFinalizePreflight {
    ManifoldPrunePreflight prune{};
    ManifoldFinalizePreflight finalize{};
ManifoldPruneFinalizePreflight preflight_manifold_prune_finalize(
bool should_skip_prune_contact_manifold(

// --- deepen additive from b4-narrowphase-guards-deepen-ea96 ---
bool can_dispatch_contact_pair(const ContactPairPreflight& preflight);

// --- deepen additive from deepen-b4-narrowphase-guards-5111 ---
struct ContactPairDispatchPreflight {
ContactPairDispatchPreflight preflight_contact_pair_dispatch(
    ContactPairDispatchPreflight preflight{};

// --- deepen additive from b4-narrowphase-deepen-guards-f32b ---
    ContactPairRejectPreflight reject{};

// --- deepen additive from b4-narrowphase-deepen-guards-a773 ---
    bool can_dispatch() const { return reason == ContactPairRejectReason::None; }
enum class NarrowphaseRejectReason : u8 {
const char* narrowphase_reject_reason_name(NarrowphaseRejectReason reason);
NarrowphaseRejectReason narrowphase_reject_reason(
    NarrowphaseRejectReason expected);
struct NarrowphasePairListPreflight {
    NarrowphaseRejectReason reason = NarrowphaseRejectReason::None;
    bool can_dispatch() const { return reason == NarrowphaseRejectReason::None; }
NarrowphasePairListPreflight preflight_narrowphase_pair_list(

// --- deepen additive from deepen-b4-narrowphase-guards-72f5 ---
struct NarrowphasePreflight {
NarrowphasePreflight preflight_narrowphase(

// --- deepen additive from b4-narrowphase-deepen-guards-699f ---
struct NarrowphasePairBatchPreflight {
NarrowphasePairBatchPreflight preflight_narrowphase_pairs(

// --- deepen additive from deepen-b4-narrowphase-guards-1468 ---
ContactPairRejectReason contact_pair_deepen2_reject_reason(
struct ContactPairDeepen2Preflight {
ContactPairDeepen2Preflight preflight_contact_pair_deepen2(
bool should_skip_contact_pair_deepen2_dispatch(

// --- deepen additive from deepen-b4-narrowphase-guards-b463 ---
struct ContactPairBatchPreflight {
ContactPairBatchPreflight preflight_contact_pair_batch(

// --- deepen additive from deepen-b4-narrowphase-guards-f4c2 ---
        return reason == NarrowphaseRejectReason::None && dispatchableCount > 0u;

// --- deepen additive from b4-narrowphase-deepen-guards-6242 ---
ContactPairRejectReason contact_pair_deepen_pass_reject_reason(
struct ContactPairDeepenPassPreflight {
ContactPairDeepenPassPreflight preflight_contact_pair_deepen_pass(
bool should_skip_contact_pair_deepen_pass_dispatch(
bool should_skip_contact_pair_batch(

// --- deepen additive from b4-narrowphase-deepen-guards-4d64 ---
struct NarrowphaseDispatchPreflight {
NarrowphaseDispatchPreflight preflight_narrowphase_dispatch(

// --- deepen additive from deepen-b4-narrowphase-guards-754b ---
struct ContactPairBatchDeepenPreflight {
ContactPairBatchDeepenPreflight preflight_contact_pair_batch_deepen(

// --- deepen additive from b4-narrowphase-deepen-guards-5d1f ---
ContactPairRejectReason first_contact_pair_deepen_reject_in_batch(

// --- deepen additive from b4-narrowphase-deepen-guards-8a17 ---
enum class NarrowphaseBatchRejectReason : u8 {
    NarrowphaseBatchRejectReason reason = NarrowphaseBatchRejectReason::None;
    bool can_skip() const { return reason != NarrowphaseBatchRejectReason::None; }
    bool can_run() const { return reason == NarrowphaseBatchRejectReason::None && can_dispatch(); }
const char* narrowphase_batch_reject_reason_name(NarrowphaseBatchRejectReason reason);
NarrowphaseBatchRejectReason narrowphase_batch_reject_reason(
    NarrowphaseBatchRejectReason expected);

// --- deepen additive from b4-narrowphase-deepen-guards-68c9 ---
struct NarrowphaseRunPreflight {
    NarrowphaseBatchPreflight batch{};
NarrowphaseRunPreflight preflight_run_narrowphase(

// --- deepen additive from deepen-b4-narrowphase-b46-8196 ---
ContactPairRejectReason first_contact_pair_deepen_reject_reason(
NarrowphaseRunPreflight preflight_narrowphase_run(
bool should_skip_narrowphase_run(

// --- deepen additive from deepen-b4-narrowphase-9067 ---
struct NarrowphasePairSlotPreflight {
NarrowphasePairSlotPreflight preflight_narrowphase_pair_slot(
bool should_skip_narrowphase_pair_slot(

// --- deepen additive from deepen-b4-narrowphase-guards-1644 ---
    ContactPairDeepenPreflight deepen{};
bool should_skip_contact_pair_dispatch_preflight(

// --- deepen additive from b4-narrowphase-deepen-guards-ed7c ---
bool should_skip_narrowphase_batch(

// --- deepen additive from deepen-b4-narrowphase-guards-f881 ---
bool should_skip_contact_pair_for_buffer(
struct NarrowphaseSlotPreflight {
    ContactPairRejectReason pairReason = ContactPairRejectReason::None;
NarrowphaseSlotPreflight preflight_narrowphase_slot(

// --- deepen additive from deepen-b4-narrowphase-guards-56fd ---
ContactPairRejectReason contact_pair_beyond_deepen_reject_reason(
struct ContactPairBeyondDeepenPreflight {
ContactPairBeyondDeepenPreflight preflight_contact_pair_beyond(
bool should_skip_contact_pair_beyond_dispatch(
struct NarrowphaseBeyondBatchPreflight {
NarrowphaseBeyondBatchPreflight preflight_narrowphase_beyond_batch(

// --- deepen additive from b4-narrowphase-deepen-pass-6859 ---
struct ContactPairSlotPreflight {
    ContactPairPreflight base{};
    ContactPairDeepenPassPreflight deepenPass{};
ContactPairSlotPreflight preflight_contact_pair_slot(

// --- deepen additive from deepen-b4-narrowphase-guards-d666 ---
enum class NarrowphaseDispatchRejectReason : u8 {
const char* narrowphase_dispatch_reject_reason_name(NarrowphaseDispatchRejectReason reason);
NarrowphaseDispatchRejectReason narrowphase_dispatch_reject_reason(
    NarrowphaseDispatchRejectReason expected);
    NarrowphaseDispatchRejectReason reason = NarrowphaseDispatchRejectReason::None;
    bool can_dispatch() const { return reason == NarrowphaseDispatchRejectReason::None && dispatchableCount > 0u; }
    bool can_skip() const { return reason != NarrowphaseDispatchRejectReason::None || dispatchableCount == 0u; }
