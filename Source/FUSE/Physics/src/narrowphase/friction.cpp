#include <fuse/physics/narrowphase/friction.hpp>

#include <fuse/physics/narrowphase/contact_manifold.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics::narrowphase {

namespace {

vec3 cross(vec3 a, vec3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

} // namespace

TangentBasis buildTangentBasis(vec3 normal) {
    const vec3 unitNormal = normal.normalized();
    const vec3 reference = std::fabs(unitNormal.y) < 0.99f ? vec3{0.f, 1.f, 0.f} : vec3{1.f, 0.f, 0.f};
    const vec3 tangent1 = cross(reference, unitNormal).normalized();
    const vec3 tangent2 = cross(unitNormal, tangent1).normalized();
    return {tangent1, tangent2};
}

TangentBasis buildTangentBasisForManifold(const ContactManifold& manifold) {
    if (manifold.hasFrictionBasis()) {
        return manifold.frictionBasis;
    }
    return buildTangentBasis(manifold.contactNormal);
}

bool isOrthonormalTangentBasis(vec3 normal, const TangentBasis& basis, f32 epsilon) {
    const vec3 unitNormal = normal.normalized();
    const f32 tangent1Length = basis.tangent1.length();
    const f32 tangent2Length = basis.tangent2.length();
    if (std::fabs(tangent1Length - 1.f) > epsilon || std::fabs(tangent2Length - 1.f) > epsilon) {
        return false;
    }

    return std::fabs(basis.tangent1.dot(unitNormal)) <= epsilon &&
           std::fabs(basis.tangent2.dot(unitNormal)) <= epsilon &&
           std::fabs(basis.tangent1.dot(basis.tangent2)) <= epsilon;
}

FrictionImpulse clampFrictionImpulse(
    FrictionImpulse accumulated,
    f32 normalImpulse,
    f32 staticFriction,
    f32 dynamicFriction) {
    FrictionImpulse clamped = accumulated;
    clamped.normal = std::max(0.f, normalImpulse);

    const f32 tangentLength = std::sqrt(
        accumulated.tangent1 * accumulated.tangent1 + accumulated.tangent2 * accumulated.tangent2);
    const f32 maxStatic = staticFriction * clamped.normal;
    const f32 maxDynamic = dynamicFriction * clamped.normal;
    const f32 maxTangent = tangentLength <= maxStatic ? maxStatic : maxDynamic;

    if (tangentLength > maxTangent && tangentLength > 1e-8f) {
        const f32 scale = maxTangent / tangentLength;
        clamped.tangent1 *= scale;
        clamped.tangent2 *= scale;
    }

    return clamped;
}

vec2 projectTangentialVelocity(vec3 relativeVelocity, const TangentBasis& basis) {
    return {
        relativeVelocity.dot(basis.tangent1),
        relativeVelocity.dot(basis.tangent2),
    };
}

bool should_skip_friction_tangents(const ContactManifold& manifold) {
    if (manifold.empty() || !manifold.hasValidNormal()) {
        return true;
    }
    return false;
}

bool has_cached_friction_basis(const ContactManifold& manifold) {
    return manifold.hasFrictionBasis();
}

bool friction_basis_matches_normal(const ContactManifold& manifold, f32 epsilon) {
    if (!manifold.hasValidNormal()) {
        return false;
    }
    return isOrthonormalTangentBasis(manifold.contactNormal, manifold.frictionBasis, epsilon);
}

bool needs_friction_basis_rebuild(const ContactManifold& manifold) {
    if (should_skip_friction_tangents(manifold)) {
        return false;
    }
    return !has_cached_friction_basis(manifold);
}

void invalidate_friction_basis(ContactManifold& manifold) {
    manifold.frictionBasis = {};
}

bool ensure_friction_basis(ContactManifold& manifold) {
    if (should_skip_friction_tangents(manifold)) {
        invalidate_friction_basis(manifold);
        return false;
    }

    if (has_cached_friction_basis(manifold)) {
        return true;
    }

    manifold.buildFrictionBasis();
    return manifold.hasFrictionBasis();
}

bool should_skip_friction_solve(
    f32 staticFriction,
    f32 dynamicFriction,
    f32 normalImpulse,
    f32 impulseEpsilon) {
    if (staticFriction <= 0.f && dynamicFriction <= 0.f) {
        return true;
    }
    if (normalImpulse <= impulseEpsilon) {
        return true;
    }
    return false;
}

bool hasNegligibleTangentialVelocity(vec2 projected, f32 speedThreshold) {
    return tangentialSpeed(projected) <= speedThreshold;
}

f32 tangentialSpeed(vec2 projected) {
    return std::sqrt(projected.x * projected.x + projected.y * projected.y);
}

bool should_skip_tangential_velocity_solve(
    vec2 projectedVelocity,
    f32 staticFriction,
    f32 dynamicFriction,
    f32 normalImpulse,
    f32 speedThreshold,
    f32 impulseEpsilon) {
    if (should_skip_friction_solve(staticFriction, dynamicFriction, normalImpulse, impulseEpsilon)) {
        return true;
    }
    return hasNegligibleTangentialVelocity(projectedVelocity, speedThreshold);
}

bool friction_basis_is_stale(const ContactManifold& manifold, f32 epsilon) {
    if (should_skip_friction_tangents(manifold)) {
        return false;
    }

    const bool hasPartialBasis =
        manifold.frictionBasis.tangent1.length() > epsilon ||
        manifold.frictionBasis.tangent2.length() > epsilon;
    if (!hasPartialBasis) {
        return false;
    }

    return !friction_basis_matches_normal(manifold, epsilon);
}

bool needs_friction_basis_refresh(const ContactManifold& manifold, f32 epsilon) {
    if (should_skip_friction_tangents(manifold)) {
        return false;
    }
    return !has_cached_friction_basis(manifold) || friction_basis_is_stale(manifold, epsilon);
}

bool can_skip_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon) {
    if (should_skip_friction_tangents(manifold)) {
        return true;
    }
    return has_cached_friction_basis(manifold) && !friction_basis_is_stale(manifold, epsilon);
}

bool rebuild_friction_basis_if_needed(ContactManifold& manifold, f32 epsilon) {
    if (should_skip_friction_tangents(manifold)) {
        invalidate_friction_basis(manifold);
        return false;
    }

    if (can_skip_friction_basis_rebuild(manifold, epsilon)) {
        return true;
    }

    invalidate_friction_basis(manifold);
    return ensure_friction_basis(manifold);
}

void compute_friction_tangents_if_needed(ContactManifold& manifold, f32 epsilon) {
    if (should_skip_friction_tangents(manifold)) {
        invalidate_friction_basis(manifold);
        return;
    }

    if (can_skip_friction_basis_rebuild(manifold, epsilon)) {
        return;
    }

    invalidate_friction_basis(manifold);
    const f32 normalLength = manifold.contactNormal.length();
    if (std::fabs(normalLength - 1.f) > 1e-4f) {
        manifold.contactNormal = manifold.contactNormal * (1.f / normalLength);
    }
    manifold.buildFrictionBasis();
}

FrictionBasisPreflight preflight_friction_basis_rebuild(
    const ContactManifold& manifold,
    f32 epsilon) {
    FrictionBasisPreflight preflight{};
    preflight.reason = friction_basis_reject_reason(manifold, epsilon);
    if (should_skip_friction_tangents(manifold)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.stale = friction_basis_is_stale(manifold, epsilon);
    preflight.canReuse = can_skip_friction_basis_rebuild(manifold, epsilon);
    preflight.needsRebuild = needs_friction_basis_refresh(manifold, epsilon);
    return preflight;
}

const char* friction_basis_reject_reason_name(FrictionBasisRejectReason reason) {
    switch (reason) {
    case FrictionBasisRejectReason::None:
        return "None";
    case FrictionBasisRejectReason::SkippedManifold:
        return "SkippedManifold";
    case FrictionBasisRejectReason::BasisCurrent:
        return "BasisCurrent";
    }
    return "Unknown";
}

FrictionBasisRejectReason friction_basis_reject_reason(
    const ContactManifold& manifold,
    f32 epsilon) {
    if (should_skip_friction_tangents(manifold)) {
        return FrictionBasisRejectReason::SkippedManifold;
    }
    if (can_skip_friction_basis_rebuild(manifold, epsilon)) {
        return FrictionBasisRejectReason::BasisCurrent;
    }
    return FrictionBasisRejectReason::None;
}

bool friction_basis_rejects_for_reason(
    const ContactManifold& manifold,
    FrictionBasisRejectReason expected,
    f32 epsilon) {
    return friction_basis_reject_reason(manifold, epsilon) == expected;
}

bool should_skip_friction_basis_preflight(
    const ContactManifold& manifold,
    f32 epsilon) {
    return preflight_friction_basis_rebuild(manifold, epsilon).can_skip_rebuild();
}

bool rebuild_friction_basis_with_preflight(ContactManifold& manifold, f32 epsilon) {
    const FrictionBasisPreflight preflight = preflight_friction_basis_rebuild(manifold, epsilon);
    if (preflight.can_skip_rebuild()) {
        if (preflight.reason == FrictionBasisRejectReason::SkippedManifold) {
            invalidate_friction_basis(manifold);
            return false;
        }
        return preflight.canReuse;
    }
    return rebuild_friction_basis_if_needed(manifold, epsilon);
}

void compute_friction_tangents_with_preflight(ContactManifold& manifold, f32 epsilon) {
    const FrictionBasisPreflight preflight = preflight_friction_basis_rebuild(manifold, epsilon);
    if (preflight.can_skip_rebuild()) {
        if (preflight.reason == FrictionBasisRejectReason::SkippedManifold) {
            invalidate_friction_basis(manifold);
        }
        return;
    }
    compute_friction_tangents_if_needed(manifold, epsilon);
}

} // namespace fuse::physics::narrowphase
