#pragma once

#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/physics_data.hpp>
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
bool needs_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Epsilon-aware rebuild check: missing basis or no longer aligned with `contactNormal` (B4.3 deepen pass).
bool needs_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon);
/// Returns true when `basis` is orthonormal and aligned with `normal` (B4.3 deepen pass).
bool isValidFrictionBasisForNormal(vec3 normal, const TangentBasis& basis, f32 epsilon = 1e-4f);

/// Alias for `needs_friction_basis_rebuild` (B4.3 deepen pass).
bool should_rebuild_friction_basis(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Clear the manifold friction basis without touching contact points (B4.3 deepen pass).
void invalidate_friction_basis(ContactManifold& manifold);

/// Build or reuse an orthonormal tangent frame; returns false when tangents should be skipped (B4.3 deepen pass).
bool ensure_friction_basis(ContactManifold& manifold);
/// Returns true when the manifold already stores a valid orthonormal friction basis (B4.3 deepen).
bool hasCachedFrictionBasis(const ContactManifold& manifold);

/// Returns true when friction tangents must be rebuilt (missing or stale basis) (B4.3 deepen).
bool should_rebuild_friction_tangents(const ContactManifold& manifold, f32 normalEpsilon = 1e-4f);

/// Build friction basis only when skip/rebuild guards allow it (B4.3 deepen).
void ensureFrictionBasis(ContactManifold& manifold);

/// Epsilon-aware ensure: rebuilds when the cached basis is missing or stale (B4.3 deepen pass).
bool ensure_friction_basis(ContactManifold& manifold, f32 epsilon);
bool ensure_friction_basis(ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Returns true when `basis` is orthonormal and aligned with `normal` (B4.3 deepen pass).
bool isValidFrictionBasisForNormal(vec3 normal, const TangentBasis& basis, f32 epsilon = 1e-4f);

/// Alias for `needs_friction_basis_rebuild(manifold, epsilon)` (B4.3 deepen pass).
bool should_rebuild_friction_basis(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Returns true when a cached basis no longer matches the manifold contact normal (B4.3 deepen pass 2).
bool friction_basis_is_stale(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Rebuild the tangent frame when missing or stale; returns false when tangents should be skipped (B4.3 deepen pass 2).
bool rebuild_friction_basis_if_needed(ContactManifold& manifold);
/// Alias for `needs_friction_basis_rebuild` (B4.3 deepen pass).

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
/// Why friction-basis rebuild would early-out (B4.4 deepen follow-up pass).
    SkippedManifold,
    ValidCachedBasis,

/// Human-readable label for friction-basis reject reasons (logging / tests).

/// Diagnose why friction-basis rebuild would skip; vacuously succeeds when rebuild may proceed.
FrictionBasisRejectReason friction_basis_reject_reason(
    f32 epsilon = 1e-4f);

/// Returns true when `friction_basis_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
    FrictionBasisRejectReason expected,
    BasisCurrent,

/// Human-readable label for friction-basis reject reasons (B4.4 deepen follow-up pass).



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
        return reason != FrictionBasisRejectReason::None;
        return reason != FrictionBasisRejectReason::None || skipped || canReuse;
    bool can_skip_rebuild() const { return skipped || canReuse; }

    /// True when a friction-basis rebuild pass has work to do (B4.5 deepen follow-up).
    bool needs_work() const { return !skipped && needsRebuild; }
    bool can_rebuild() const { return !skipped && needsRebuild; }
};

/// Populate friction-basis preflight without mutating the manifold (B4.4 deepen follow-up).
FrictionBasisPreflight preflight_friction_basis_rebuild(
    f32 epsilon = 1e-4f);

/// Returns true when friction-basis rebuild should be skipped (B4.4 deepen follow-up).
bool should_skip_friction_basis_preflight(

/// Returns true when the normal must be normalized before friction-basis rebuild (B4.4 deepen pass).
bool should_normalize_contact_normal_before_friction(
    f32 lengthEpsilon = 1e-4f);

/// Rebuild friction tangents only when preflight allows; returns false when skipped (B4.5 deepen follow-up pass).
bool rebuild_friction_basis_with_preflight(ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Non-mutating friction-basis rebuild predicate — inverse of `should_skip_friction_basis_preflight` (B4.6 deepen pass).
bool should_run_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon = 1e-4f);
/// Returns true when friction response should be skipped for this manifold and coefficients (B4.3 deepen).
bool should_skip_friction_for_manifold(

/// Combine per-body friction coefficients using geometric-mean stub (B4.3 deepen).
vec2 combine_body_friction_coefficients(
    const RigidBodySoA& bodies,
    u32 bodyA,
    u32 bodyB);

/// Returns true when the contact normal changed enough to invalidate a cached basis (B4.3 deepen).
bool should_rebuild_friction_basis(
    vec3 previousNormal,
    vec3 currentNormal,
    f32 angleThresholdRadians = 1e-3f);
/// Returns true when `basis` is orthonormal and aligned with `normal` (B4.3 deepen pass).
bool isValidFrictionBasisForNormal(vec3 normal, const TangentBasis& basis, f32 epsilon = 1e-4f);

/// Returns true when an existing basis is missing or no longer matches the manifold normal (B4.3 deepen pass).
bool needs_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Alias for `needs_friction_basis_rebuild` (B4.3 deepen pass).
bool should_rebuild_friction_basis(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Build or refresh the manifold friction basis when required; returns false on skip/failure (B4.3 deepen pass).
bool ensure_friction_basis(ContactManifold& manifold, f32 epsilon = 1e-4f);


/// Preflight diagnostics for friction-basis rebuild dispatch (B4.5 deepen pass).
    bool needsRefresh = false;
    bool canSkipRebuild = false;

    bool can_skip_compute() const { return skipped || canSkipRebuild; }

/// Populate friction-basis preflight without mutating the manifold (B4.5 deepen pass).
FrictionBasisPreflight preflight_friction_basis(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Early-out guard for friction tangent compute (B4.5 deepen pass).
bool should_skip_friction_basis_compute(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Clear cached basis when the preflight reports staleness (B4.5 deepen pass).
void invalidate_friction_basis_if_stale(ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Rebuild from preflight; returns false when tangents should be skipped (B4.5 deepen pass).
bool rebuild_friction_basis_from_preflight(ContactManifold& manifold, const FrictionBasisPreflight& preflight);
/// Const preflight for friction-basis rebuild dispatch (B4.5 deepen pass).
    bool missing = false;

    bool can_reuse() const { return !skipped && !missing && !stale; }

    bool needs_rebuild() const { return !skipped && (missing || stale); }


/// Returns true when friction tangents can be skipped without rebuild (B4.5 deepen pass).
bool can_skip_friction_tangents_rebuild(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Rebuild or reuse friction basis only when preflight reports `needs_rebuild` (B4.5 deepen pass).
bool ensure_friction_basis_if_needed(ContactManifold& manifold, f32 epsilon = 1e-4f);
/// Const preflight for friction basis rebuild dispatch (B4.4 deepen pass 2).
    bool missingBasis = false;
    bool staleBasis = false;

    bool needs_rebuild() const { return !skipped && (missingBasis || staleBasis); }

    bool can_reuse_cached() const { return !skipped && !missingBasis && !staleBasis; }

/// Populate friction basis preflight without mutating the manifold (B4.4 deepen pass 2).

/// Returns true when rebuild can be skipped (B4.4 deepen pass 2).
bool should_skip_friction_basis_rebuild(

/// Rebuild friction basis only when preflight requires it (B4.4 deepen pass 2).
bool rebuild_friction_basis_from_preflight(ContactManifold& manifold, f32 epsilon = 1e-4f);
/// Const preflight for friction-basis rebuild dispatch (B4.4 deepen pass).
    bool shouldSkipTangents = false;

    bool needs_rebuild() const {
        return !skipped && !shouldSkipTangents && (missingBasis || staleBasis);

        if (skipped || shouldSkipTangents) {
            return true;
        return !missingBasis && !staleBasis;

/// Populate friction-basis preflight without mutating the manifold (B4.4 deepen pass).

/// Returns true when friction-basis rebuild should be skipped (B4.4 deepen pass).
bool should_skip_friction_basis_rebuild_preflight(

/// Guarded rebuild: preflight then `compute_friction_tangents_if_needed` (B4.4 deepen pass).
void rebuild_friction_basis_guarded(ContactManifold& manifold, f32 epsilon = 1e-4f);
    bool needsNormalization = false;


/// Populate friction-basis rebuild preflight without mutating the manifold (B4.5 deepen pass).

/// Early-out guard before friction-basis rebuild (B4.5 deepen pass).
bool should_skip_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Returns true when `contactNormal` length deviates from unit length (B4.5 deepen pass).
bool contact_normal_needs_normalization(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Outcome for guarded friction-basis rebuild (B4.5 deepen pass).
struct FrictionBasisRebuildResult {
    bool rebuilt = false;

/// Rebuild friction basis only when preflight requires it (B4.5 deepen pass).
FrictionBasisRebuildResult rebuild_friction_basis_guarded(
    ContactManifold& manifold,
/// Const preflight for friction basis rebuild dispatch (B4.5 deepen pass).

    bool can_skip_rebuild() const { return skipped || canReuse; }

/// Populate friction basis rebuild preflight without mutation (B4.5 deepen pass).

/// Ensure basis is valid; no-op when rebuild can be skipped (B4.5 deepen pass).
    bool hasCachedBasis = false;
    bool wouldRebuild = false;

    bool can_reuse() const { return !skipped && hasCachedBasis && !stale; }

    bool needs_rebuild() const { return !skipped && wouldRebuild; }

/// Populate friction-basis rebuild preflight without mutating the manifold (B4.4 deepen pass).

/// Returns true when friction-basis rebuild may be skipped per preflight (B4.4 deepen pass).
bool should_skip_friction_basis_rebuild_preflight(const FrictionBasisPreflight& preflight);

/// Returns true when preflight indicates an existing basis can be reused (B4.4 deepen pass).
bool can_reuse_friction_basis(const FrictionBasisPreflight& preflight);
struct FrictionBasisRebuildPreflight {
    bool skipTangents = false;
    bool isStale = false;

    bool needs_rebuild() const { return !skipped && !skipTangents && (!hasCachedBasis || isStale); }

    bool can_skip_rebuild() const { return !needs_rebuild(); }

FrictionBasisRebuildPreflight preflight_friction_basis_rebuild(

/// Build friction tangents only when rebuild preflight requires it (B4.5 deepen pass).
bool compute_friction_tangents_guarded(ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Build or reuse friction basis via rebuild preflight (B4.5 deepen pass).
bool ensure_friction_basis_guarded(ContactManifold& manifold, f32 epsilon = 1e-4f);

    bool needs_rebuild() const { return !skipped && !canReuse && (missing || stale); }

/// Populate friction-basis rebuild preflight without mutation (B4.5 deepen pass).

/// Returns true when friction-basis rebuild should be skipped (B4.5 deepen pass).

/// Rebuild friction basis using preflight guards; returns false when skipped (B4.5 deepen pass).
    bool shouldSkip = false;

    bool needs_rebuild() const { return !skipped && !shouldSkip && (missing || stale); }

    bool can_reuse() const { return !skipped && !shouldSkip && !missing && !stale; }


/// Returns true when friction-basis rebuild should be skipped entirely (B4.4 deepen pass).
bool should_skip_friction_basis_preflight(const ContactManifold& manifold);

/// Rebuild friction basis only when preflight allows; no-op when skip/reuse (B4.4 deepen follow-up pass).
bool rebuild_friction_basis_with_preflight(ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Compute friction tangents only when preflight needs rebuild (B4.4 deepen follow-up pass).
void compute_friction_tangents_with_preflight(ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Rebuild friction basis using preflight dispatch; no-op when skip is allowed (B4.5 deepen follow-up).
bool rebuild_friction_basis_from_preflight(ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Build or reuse friction basis using preflight dispatch (B4.5 deepen follow-up).
bool ensure_friction_basis_from_preflight(ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Non-mutating rebuild predicate — inverse of `should_skip_friction_basis_preflight` (B4.4 deepen pass).
bool should_rebuild_friction_basis(
    const ContactManifold& manifold,
    f32 epsilon = 1e-4f);

/// Rebuild friction tangents only when preflight reports `needsRebuild` (B4.4 deepen pass).
bool rebuild_friction_basis_from_preflight(ContactManifold& manifold, f32 epsilon = 1e-4f);
/// Returns true when friction-basis rebuild can proceed (B4.5 deepen follow-up).
bool can_run_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Build or reuse friction basis using deepen preflight; no-op when preflight says skip (B4.5 deepen follow-up).
bool ensure_friction_basis_if_needed(ContactManifold& manifold, f32 epsilon = 1e-4f);
/// Returns true when only one tangent axis is populated or lengths are non-unit (B4.5 deepen pass).
bool has_partial_friction_basis(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Returns true when a valid normal exists but the cached basis is incomplete (B4.5 deepen pass).
bool friction_basis_needs_completion(const ContactManifold& manifold, f32 epsilon = 1e-4f);

/// Const preflight for friction-basis rebuild with partial-basis checks (B4.5 deepen pass).
struct FrictionBasisDeepenPreflight {
    bool skipped = false;
    bool stale = false;
    bool partial = false;
    bool canReuse = false;
    bool needsRebuild = false;
    bool needsNormalNormalization = false;

    bool can_skip_rebuild() const { return skipped || canReuse; }
};

/// Populate second deepen friction-basis preflight without mutating the manifold (B4.5 deepen pass).
FrictionBasisDeepenPreflight preflight_friction_basis_rebuild_deepen(
    const ContactManifold& manifold,
    f32 epsilon = 1e-4f,
    f32 normalEpsilon = 1e-4f);

/// Returns true when second deepen friction-basis rebuild should be skipped (B4.5 deepen pass).
bool should_skip_friction_basis_deepen_preflight(

/// Rebuild friction tangents with partial-basis and normal-normalization preflights (B4.5 deepen pass).
void compute_friction_tangents_deepen_if_needed(
    ContactManifold& manifold,
/// Const preflight for warm-start friction impulse restore (B4.4 deepen follow-up pass).
struct WarmStartFrictionPreflight {
    bool hasWarmImpulse = false;
    bool hasValidBasis = false;

    bool can_warm_start() const { return !skipped && hasWarmImpulse && hasValidBasis; }

/// Populate warm-start friction preflight without mutating the manifold (B4.4 deepen follow-up pass).
WarmStartFrictionPreflight preflight_warm_start_friction(
    f32 impulseEpsilon = 1e-8f,
    f32 basisEpsilon = 1e-4f);

/// Returns true when warm-start friction restore should be skipped (B4.4 deepen follow-up pass).
bool should_skip_warm_start_friction(

} // namespace fuse::physics::narrowphase
