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

/// Const preflight for friction-basis rebuild dispatch (B4.4 deepen pass).
struct FrictionBasisPreflight {
    bool shouldSkipTangents = false;
    bool missingBasis = false;
    bool staleBasis = false;
    bool skipped = false;

    bool needs_rebuild() const {
        return !skipped && !shouldSkipTangents && (missingBasis || staleBasis);
    }

    bool can_skip_rebuild() const {
        if (skipped || shouldSkipTangents) {
            return true;
        }
        return !missingBasis && !staleBasis;
    }
};

/// Populate friction-basis preflight without mutating the manifold (B4.4 deepen pass).
FrictionBasisPreflight preflight_friction_basis_rebuild(
    const ContactManifold& manifold,
    f32 epsilon = 1e-4f);

/// Returns true when friction-basis rebuild should be skipped (B4.4 deepen pass).
bool should_skip_friction_basis_rebuild_preflight(
    const ContactManifold& manifold,
    f32 epsilon = 1e-4f);

/// Guarded rebuild: preflight then `compute_friction_tangents_if_needed` (B4.4 deepen pass).
void rebuild_friction_basis_guarded(ContactManifold& manifold, f32 epsilon = 1e-4f);

} // namespace fuse::physics::narrowphase
