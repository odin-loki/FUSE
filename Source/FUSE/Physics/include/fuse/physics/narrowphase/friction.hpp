#pragma once

#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/types.hpp>

namespace fuse::physics::narrowphase {

struct ContactManifold;

struct TangentBasis {
    vec3 tangent1{};
    vec3 tangent2{};
};

struct FrictionImpulse {
    f32 normal = 0.f;
    f32 tangent1 = 0.f;
    f32 tangent2 = 0.f;
};

/// Build an orthonormal tangent frame from a contact normal (B4.3 friction stub).
TangentBasis buildTangentBasis(vec3 normal);

/// Build a tangent frame from a contact manifold's normal (B4.3 friction stub).
TangentBasis buildTangentBasisForManifold(const ContactManifold& manifold);

/// Validate that `basis` is unit-length and mutually orthogonal with `normal`.
bool isOrthonormalTangentBasis(vec3 normal, const TangentBasis& basis, f32 epsilon = 1e-4f);

/// Coulomb friction cone clamp for accumulated impulses (CPU stub).
FrictionImpulse clampFrictionImpulse(
    FrictionImpulse accumulated,
    f32 normalImpulse,
    f32 staticFriction,
    f32 dynamicFriction);

/// Project a relative velocity onto the tangent basis.
vec2 projectTangentialVelocity(vec3 relativeVelocity, const TangentBasis& basis);

/// Returns true when friction tangent frames should not be built for this manifold (B4.3 deepen).
bool should_skip_friction_tangents(const ContactManifold& manifold);

/// Returns true when an existing orthonormal basis can be reused (B4.3 deepen pass).
bool has_cached_friction_basis(const ContactManifold& manifold);

/// Returns true when `basis` is orthonormal with `manifold.contactNormal` (B4.3 deepen pass).
bool friction_basis_matches_normal(
    const ContactManifold& manifold,
    f32 epsilon = 1e-4f);

/// Returns true when a valid normal exists but the cached basis is missing or stale (B4.3 deepen pass).
bool needs_friction_basis_rebuild(const ContactManifold& manifold);

/// Clear the manifold friction basis without touching contact points (B4.3 deepen pass).
void invalidate_friction_basis(ContactManifold& manifold);

/// Build or reuse an orthonormal tangent frame; returns false when tangents should be skipped (B4.3 deepen pass).
bool ensure_friction_basis(ContactManifold& manifold);

/// Returns true when both friction coefficients are zero or normal impulse is negligible (B4.3 deepen).
bool should_skip_friction_solve(
    f32 staticFriction,
    f32 dynamicFriction,
    f32 normalImpulse = 0.f,
    f32 impulseEpsilon = 1e-8f);

/// Returns true when tangential speed is below the solver stub threshold (B4.3 deepen).
bool hasNegligibleTangentialVelocity(vec2 projected, f32 speedThreshold = 1e-6f);

/// Scalar tangential speed from a projected velocity (B4.3 deepen pass).
f32 tangentialSpeed(vec2 projected);

/// Combined early-out for tangential velocity solve (B4.3 deepen pass).
bool should_skip_tangential_velocity_solve(
    vec2 projectedVelocity,
    f32 staticFriction,
    f32 dynamicFriction,
    f32 normalImpulse = 0.f,
    f32 speedThreshold = 1e-6f,
    f32 impulseEpsilon = 1e-8f);

/// Returns true when a non-empty basis no longer matches the contact normal (B4.4 deepen pass).
bool friction_basis_is_stale(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Returns true when the basis is missing or stale and should be rebuilt (B4.4 deepen pass).
bool needs_friction_basis_refresh(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Returns true when an existing orthonormal basis can be reused without rebuild (B4.4 deepen pass).
bool can_skip_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Invalidate stale frames and build or reuse an orthonormal basis (B4.4 deepen pass).
bool rebuild_friction_basis_if_needed(ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Rebuild friction tangents only when the cached basis is missing or stale (B4.4 deepen pass).
void compute_friction_tangents_if_needed(ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Returns true when the contact normal is valid but not unit length (B4.4 deepen pass).
bool contact_normal_needs_normalize(const ContactManifold& manifold, f32 lengthEpsilon = 1e-4f);

/// Why friction-basis rebuild would early-out (B4.5 deepen follow-up pass).
enum class FrictionBasisRejectReason : u8 {
    None = 0,
    EmptyManifold,
    InvalidNormal,
};

/// Human-readable label for friction-basis reject reasons (B4.5 deepen follow-up pass).
const char* friction_basis_reject_reason_name(FrictionBasisRejectReason reason);

/// Diagnose why friction-basis rebuild would skip; vacuously succeeds when rebuild may proceed (B4.5 deepen follow-up pass).
FrictionBasisRejectReason friction_basis_reject_reason(const ContactManifold& manifold);

/// Returns true when `friction_basis_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool friction_basis_rejects_for_reason(
    const ContactManifold& manifold,
    FrictionBasisRejectReason expected);

/// Const preflight for friction-basis rebuild dispatch (B4.4 deepen follow-up).
struct FrictionBasisPreflight {
    FrictionBasisRejectReason reason = FrictionBasisRejectReason::None;
    bool skipped = false;
    bool stale = false;
    bool needsNormalNormalize = false;
    bool canReuse = false;
    bool needsRebuild = false;

    bool can_skip_rebuild() const {
        return skipped || reason != FrictionBasisRejectReason::None || canReuse;
    }
};

/// Populate friction-basis preflight without mutating the manifold (B4.4 deepen follow-up).
FrictionBasisPreflight preflight_friction_basis_rebuild(
    const ContactManifold& manifold,
    f32 epsilon = 1e-4f);

/// Returns true when friction-basis rebuild should be skipped (B4.4 deepen follow-up).
bool should_skip_friction_basis_preflight(
    const ContactManifold& manifold,
    f32 epsilon = 1e-4f);

/// Returns true when the normal must be normalized before friction-basis rebuild (B4.4 deepen pass).
bool should_normalize_contact_normal_before_friction(
    const ContactManifold& manifold,
    f32 lengthEpsilon = 1e-4f);

/// Rebuild friction tangents only when preflight allows; returns false when skipped (B4.5 deepen follow-up pass).
bool rebuild_friction_basis_with_preflight(ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Non-mutating friction-basis rebuild predicate — inverse of `should_skip_friction_basis_preflight` (B4.6 deepen pass).
bool should_run_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon = 1e-4f);

} // namespace fuse::physics::narrowphase

// --- deepen additive from deepen-b4-narrowphase-manifold-prune-4828 ---
bool should_skip_friction_for_manifold(

// --- deepen additive from deepen-b4-narrowphase-guards-56bb ---
FrictionBasisPreflight preflight_friction_basis(const ContactManifold& manifold, f32 epsilon = 1e-4f);
bool should_skip_friction_basis_compute(const ContactManifold& manifold, f32 epsilon = 1e-4f);
bool rebuild_friction_basis_from_preflight(ContactManifold& manifold, const FrictionBasisPreflight& preflight);

// --- deepen additive from b4-narrowphase-deepen-guards-e063 ---
bool should_skip_friction_basis_rebuild(

// --- deepen additive from deepen-b4-narrowphase-guards-d11b ---
bool should_skip_friction_basis_rebuild_preflight(

// --- deepen additive from b4-narrowphase-deepen-guards-c64f ---
bool should_skip_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon = 1e-4f);

// --- deepen additive from b4-narrowphase-guards-deepen-ea96 ---
    bool wouldRebuild = false;
bool should_skip_friction_basis_rebuild_preflight(const FrictionBasisPreflight& preflight);
bool can_reuse_friction_basis(const FrictionBasisPreflight& preflight);

// --- deepen additive from deepen-b4-narrowphase-guards-5111 ---
struct FrictionBasisRebuildPreflight {
FrictionBasisRebuildPreflight preflight_friction_basis_rebuild(

// --- deepen additive from deepen-b4-narrowphase-guards-914a ---
bool should_skip_friction_basis_preflight(const ContactManifold& manifold);

// --- deepen additive from b4-narrowphase-deepen-guards-a773 ---
    FrictionBasisRejectReason expected,
        return reason != FrictionBasisRejectReason::None;

// --- deepen additive from deepen-b4-narrowphase-guards-72f5 ---
        return reason != FrictionBasisRejectReason::None || skipped || canReuse;

// --- deepen additive from deepen-b4-narrowphase-guards-1468 ---
struct FrictionBasisDeepenPreflight {
FrictionBasisDeepenPreflight preflight_friction_basis_rebuild_deepen(
bool should_skip_friction_basis_deepen_preflight(

// --- deepen additive from deepen-b4-narrowphase-guards-b463 ---
struct WarmStartFrictionPreflight {
WarmStartFrictionPreflight preflight_warm_start_friction(
bool should_skip_warm_start_friction(

// --- deepen additive from b4-narrowphase-deepen-guards-6e88 ---
struct FrictionBasisEnsurePreflight {
FrictionBasisEnsurePreflight preflight_friction_basis_ensure(

// --- deepen additive from deepen-b4-narrowphase-guards-ddb5 ---
    FrictionBasisRejectReason rejectReason = FrictionBasisRejectReason::None;

// --- deepen additive from deepen-b4-narrowphase-guards-0339 ---
    bool can_skip_rebuild() const { return reason != FrictionBasisRejectReason::None; }
    bool can_rebuild() const { return reason == FrictionBasisRejectReason::None; }

// --- deepen additive from b4-narrowphase-deepen-guards-6242 ---
enum class FrictionBasisRebuildRejectReason : u8 {
const char* friction_basis_rebuild_reject_reason_name(FrictionBasisRebuildRejectReason reason);
FrictionBasisRebuildRejectReason friction_basis_rebuild_reject_reason(
    FrictionBasisRebuildRejectReason expected,
