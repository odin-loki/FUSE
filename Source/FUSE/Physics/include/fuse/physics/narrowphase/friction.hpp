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

} // namespace fuse::physics::narrowphase
