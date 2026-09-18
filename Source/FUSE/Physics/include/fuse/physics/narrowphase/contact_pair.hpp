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
    BothSleeping,
    BothKinematic,
    DegenerateShape,
};

/// Preflight result for narrowphase pair dispatch (B4.3 deepen pass).
struct ContactPairDispatchPreflight {
    ContactPairRejectReason reason = ContactPairRejectReason::None;

    bool can_dispatch() const { return reason == ContactPairRejectReason::None; }
};

/// Human-readable label for diagnostics and test assertions (B4.3 deepen pass).
const char* contact_pair_reject_reason_name(ContactPairRejectReason reason);

/// Returns true when the reject reason is structural (self, OOB, missing shape).
bool contact_pair_reject_reason_is_structural(ContactPairRejectReason reason);

/// Run pair reject guards and return a dispatch preflight summary (B4.3 deepen pass).
ContactPairDispatchPreflight preflight_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes);

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

/// Returns true when both bodies carry `RB_SLEEPING` (no narrowphase dispatch stub, B4.3 deepen pass).
bool is_sleeping_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies);

/// Returns true when both bodies carry `RB_KINEMATIC` (no dynamic response stub, B4.3 deepen pass).
bool is_kinematic_contact_pair(
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

} // namespace fuse::physics::narrowphase
