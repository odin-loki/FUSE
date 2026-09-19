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

/// Why friction tangent compute would early-out (B4.6 deepen pass).
enum class FrictionTangentComputeRejectReason : u8 {
    None = 0,
    EmptyManifold,
    InvalidNormal,
    CachedBasis,
};

/// Human-readable label for friction tangent compute reject reasons (B4.6 deepen pass).
inline const char* friction_tangent_compute_reject_reason_name(FrictionTangentComputeRejectReason reason) {
    switch (reason) {
    case FrictionTangentComputeRejectReason::None:
        return "None";
    case FrictionTangentComputeRejectReason::EmptyManifold:
        return "EmptyManifold";
    case FrictionTangentComputeRejectReason::InvalidNormal:
        return "InvalidNormal";
    case FrictionTangentComputeRejectReason::CachedBasis:
        return "CachedBasis";
    }
    return "Unknown";
}

/// Diagnose why `compute_friction_tangents` would skip; vacuously succeeds when compute may proceed (B4.6 deepen pass).
inline FrictionTangentComputeRejectReason friction_tangent_compute_reject_reason(
    const ContactManifold& manifold,
    f32 epsilon = 1e-4f) {
    const FrictionBasisRejectReason basisReason = friction_basis_reject_reason(manifold);
    if (basisReason == FrictionBasisRejectReason::EmptyManifold) {
        return FrictionTangentComputeRejectReason::EmptyManifold;
    }
    if (basisReason == FrictionBasisRejectReason::InvalidNormal) {
        return FrictionTangentComputeRejectReason::InvalidNormal;
    }
    if (can_skip_friction_basis_rebuild(manifold, epsilon)) {
        return FrictionTangentComputeRejectReason::CachedBasis;
    }
    return FrictionTangentComputeRejectReason::None;
}

/// Returns true when `friction_tangent_compute_reject_reason` matches `expected` (B4.6 deepen pass).
inline bool friction_tangent_compute_rejects_for_reason(
    const ContactManifold& manifold,
    FrictionTangentComputeRejectReason expected,
    f32 epsilon = 1e-4f) {
    return friction_tangent_compute_reject_reason(manifold, epsilon) == expected;
}

/// Read-only friction tangent compute diagnostics — no mutation (B4.6 deepen pass).
struct FrictionTangentComputePreflight {
    FrictionTangentComputeRejectReason reason = FrictionTangentComputeRejectReason::None;
    bool skipped = false;
    bool needsCompute = false;

    bool can_compute() const {
        return !skipped && reason == FrictionTangentComputeRejectReason::None && needsCompute;
    }

    bool can_skip_compute() const {
        return skipped || reason != FrictionTangentComputeRejectReason::None || !needsCompute;
    }
};

/// Populate friction tangent compute preflight without mutation (B4.6 deepen pass).
inline FrictionTangentComputePreflight preflight_friction_tangent_compute(
    const ContactManifold& manifold,
    f32 epsilon = 1e-4f) {
    FrictionTangentComputePreflight preflight{};
    preflight.reason = friction_tangent_compute_reject_reason(manifold, epsilon);
    if (preflight.reason == FrictionTangentComputeRejectReason::EmptyManifold ||
        preflight.reason == FrictionTangentComputeRejectReason::InvalidNormal) {
        preflight.skipped = true;
        return preflight;
    }
    if (preflight.reason == FrictionTangentComputeRejectReason::CachedBasis) {
        return preflight;
    }
    preflight.needsCompute = needs_friction_basis_refresh(manifold, epsilon);
    return preflight;
}

/// Returns true when friction tangent compute should be skipped (B4.6 deepen pass).
inline bool can_skip_compute_friction_tangents(const ContactManifold& manifold, f32 epsilon = 1e-4f) {
    return preflight_friction_tangent_compute(manifold, epsilon).can_skip_compute();
}

/// Compute friction tangents only when preflight allows (B4.6 deepen pass).
inline void compute_friction_tangents_with_preflight(ContactManifold& manifold, f32 epsilon = 1e-4f) {
    const FrictionTangentComputePreflight preflight = preflight_friction_tangent_compute(manifold, epsilon);
    if (preflight.reason == FrictionTangentComputeRejectReason::EmptyManifold ||
        preflight.reason == FrictionTangentComputeRejectReason::InvalidNormal) {
        invalidate_friction_basis(manifold);
        return;
    }
    if (preflight.reason == FrictionTangentComputeRejectReason::CachedBasis) {
        return;
    }
    compute_friction_tangents_if_needed(manifold, epsilon);
}

} // namespace fuse::physics::narrowphase
