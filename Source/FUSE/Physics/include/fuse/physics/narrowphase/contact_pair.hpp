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
bool is_kinematic_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies);

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
/// Inverse of `should_skip_contact_pair_dispatch` (B4.5 deepen pass).
bool can_dispatch_contact_pair(
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
/// Guarded detect: shape dispatch only when preflight allows (B4.5 deepen pass).
ContactManifold detect_contacts_pair_if_valid(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Combined preflight + detect outcome for parallel dispatch stubs (B4.5 deepen pass).
struct ContactPairDispatchResult {
    ContactPairPreflight preflight{};
    ContactManifold manifold{};
    bool detected = false;

    bool rejected() const { return preflight.rejected; }

/// Preflight then detect; rejected pairs leave `detected` false (B4.5 deepen pass).
ContactPairDispatchResult dispatch_contact_pair_if_valid(
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

/// Count pairs that pass extended deepen preflight (B4.4 deepen pass).
u32 count_dispatchable_contact_pairs(

/// True when at least one pair passes extended deepen preflight (B4.4 deepen pass).
bool has_dispatchable_contact_pair(

/// Returns true when `contact_pair_deepen_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool contact_pair_deepen_rejects_for_reason(
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected);

/// Returns true when both shapes resolve to plane types (B4.5 deepen follow-up pass).
bool is_plane_plane_contact_pair(

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

} // namespace fuse::physics::narrowphase
