#pragma once

#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/types.hpp>

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

/// Extended pair reject preflight with per-check flags (B4.5 deepen pass).
struct ContactPairRejectPreflight {
    ContactPairRejectReason reason = ContactPairRejectReason::None;
    bool rejected = false;
    bool selfPair = false;
    bool outOfRange = false;
    bool missingShape = false;
    bool bothTriggers = false;
    bool unsupportedPair = false;
    bool bothStatic = false;
    bool degenerateShape = false;

    bool can_dispatch() const { return !rejected; }
};

/// Populate extended pair reject preflight without shape dispatch (B4.5 deepen pass).
ContactPairRejectPreflight preflight_contact_pair_reject(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// Explicit reject predicate mirroring `is_invalid_contact_pair` (B4.5 deepen pass).
bool should_reject_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

/// True when pair indices are in range and distinct (B4.5 deepen pass).
bool contact_pair_has_valid_indices(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies);

/// True when both bodies have collision shapes (B4.5 deepen pass).
bool contact_pair_has_shapes(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes);

/// Diagnostic reason finalize rejects a manifold (B4.5 deepen pass).
enum class ManifoldFinalizeRejectReason : u8 {
    None = 0,
    Empty,
    InvalidNormal,
    NoPenetratingPoints,
    PruneWouldEmpty,
};

/// Human-readable label for finalize reject diagnostics (B4.5 deepen pass).
const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason);

/// Const preflight for finalize dispatch (B4.5 deepen pass).
struct ManifoldFinalizePreflight {
    ManifoldFinalizeRejectReason reason = ManifoldFinalizeRejectReason::None;
    bool rejected = false;
    bool empty = false;
    bool invalidNormal = false;
    bool noPenetratingPoints = false;
    bool pruneWouldEmpty = false;

    bool can_finalize() const { return !rejected; }
};

/// Populate finalize preflight without mutating slots (B4.5 deepen pass).
ManifoldFinalizePreflight preflight_finalize_contact_manifold(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// First finalize reject reason, or `None` when finalize may proceed (B4.5 deepen pass).
ManifoldFinalizeRejectReason manifold_finalize_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Returns true when finalize should be skipped (B4.5 deepen pass).
bool should_skip_finalize_contact_manifold(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// Finalize only when preflight passes; returns false without mutation on reject (B4.5 deepen pass).
bool generate_contact_manifold_if_needed(ContactManifold& manifold);

/// Combined prune+finalize pipeline preflight (B4.5 deepen pass).
struct ManifoldPruneFinalizePreflight {
    ManifoldPrunePreflight prune{};
    ManifoldFinalizePreflight finalize{};
    bool skipped = false;

    bool can_finalize_after_prune() const {
        return !skipped && finalize.can_finalize() &&
               (!prune.needs_pruning() || !prune.wouldBeEmpty);
    }
};

/// Combined prune+finalize preflight without mutation (B4.5 deepen pass).
ManifoldPruneFinalizePreflight preflight_manifold_prune_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

/// True when prune should be skipped because the manifold is empty or prune would clear all slots (B4.5 deepen pass).
bool should_skip_prune_contact_manifold(
    const ContactManifold& manifold,
    f32 separationEpsilon = 1e-6f,
    f32 duplicateEpsilon = 1e-4f);

} // namespace fuse::physics::narrowphase
