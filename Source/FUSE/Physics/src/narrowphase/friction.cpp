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

bool isValidFrictionBasisForNormal(vec3 normal, const TangentBasis& basis, f32 epsilon) {
    return isOrthonormalTangentBasis(normal, basis, epsilon);
}

bool friction_basis_matches_normal(const ContactManifold& manifold, f32 epsilon) {
    if (!manifold.hasValidNormal()) {
        return false;
    return isOrthonormalTangentBasis(manifold.contactNormal, manifold.frictionBasis, epsilon);

bool isValidFrictionBasisForNormal(vec3 normal, const TangentBasis& basis, f32 epsilon) {
    return isOrthonormalTangentBasis(normal, basis, epsilon);
}

bool needs_friction_basis_rebuild(const ContactManifold& manifold) {
    return needs_friction_basis_rebuild(manifold, 1e-4f);

    return isValidFrictionBasisForNormal(manifold.contactNormal, manifold.frictionBasis, epsilon);


bool needs_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon) {
    if (should_skip_friction_tangents(manifold)) {
    return !has_cached_friction_basis(manifold);
        return false;
    }
    if (!has_cached_friction_basis(manifold)) {
        return true;
    return !isValidFrictionBasisForNormal(manifold.contactNormal, manifold.frictionBasis, epsilon);
    return !isValidFrictionBasisForNormal(manifold.contactNormal, manifold.frictionBasis);
}

bool should_rebuild_friction_basis(const ContactManifold& manifold, f32 epsilon) {
    return needs_friction_basis_rebuild(manifold, epsilon);
}

bool should_rebuild_friction_basis(const ContactManifold& manifold, f32 epsilon) {
    return needs_friction_basis_rebuild(manifold, epsilon);
}

bool should_rebuild_friction_basis(const ContactManifold& manifold, f32 epsilon) {
    if (should_skip_friction_tangents(manifold)) {
        return false;

void invalidate_friction_basis(ContactManifold& manifold) {
    manifold.frictionBasis = {};

bool ensure_friction_basis(ContactManifold& manifold) {
    return ensure_friction_basis(manifold, 1e-4f);
}

bool ensure_friction_basis(ContactManifold& manifold, f32 epsilon) {
    if (should_skip_friction_tangents(manifold)) {
        invalidate_friction_basis(manifold);

    if (!needs_friction_basis_rebuild(manifold, epsilon)) {
    if (!needs_friction_basis_rebuild(manifold)) {
        return true;

    if (!manifold.normalizeContactNormal()) {
        return false;

    if (has_cached_friction_basis(manifold) && !friction_basis_is_stale(manifold)) {

    manifold.buildFrictionBasis();
bool hasCachedFrictionBasis(const ContactManifold& manifold) {

bool should_rebuild_friction_tangents(const ContactManifold& manifold, f32 normalEpsilon) {
    if (!hasCachedFrictionBasis(manifold)) {
    return !isOrthonormalTangentBasis(manifold.contactNormal, manifold.frictionBasis, normalEpsilon);

void ensureFrictionBasis(ContactManifold& manifold) {
        return;
    if (!should_rebuild_friction_tangents(manifold)) {

    const f32 normalLength = manifold.contactNormal.length();
    if (std::fabs(normalLength - 1.f) > 1e-4f && normalLength > 1e-8f) {
        manifold.contactNormal = manifold.contactNormal * (1.f / normalLength);

bool isValidFrictionBasisForNormal(vec3 normal, const TangentBasis& basis, f32 epsilon) {
    return isOrthonormalTangentBasis(normal, basis, epsilon);

bool should_rebuild_friction_basis(const ContactManifold& manifold, f32 epsilon) {
    return needs_friction_basis_rebuild(manifold, epsilon);

bool friction_basis_is_stale(const ContactManifold& manifold, f32 epsilon) {
    if (!manifold.hasValidNormal()) {

    const f32 tangent1Length = manifold.frictionBasis.tangent1.length();
    const f32 tangent2Length = manifold.frictionBasis.tangent2.length();
    if (tangent1Length <= epsilon && tangent2Length <= epsilon) {

    return !friction_basis_matches_normal(manifold, epsilon);

bool rebuild_friction_basis_if_needed(ContactManifold& manifold) {
    if (should_skip_friction_tangents(manifold)) {
        invalidate_friction_basis(manifold);
        return false;
    }

    if (has_cached_friction_basis(manifold) && !friction_basis_is_stale(manifold)) {
        return true;
    }

    invalidate_friction_basis(manifold);
    if (!needs_friction_basis_rebuild(manifold, epsilon)) {

    if (!manifold.normalizeContactNormal()) {
        return false;


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

    return !friction_basis_matches_normal(manifold, epsilon);

bool needs_friction_basis_refresh(const ContactManifold& manifold, f32 epsilon) {
    return !has_cached_friction_basis(manifold) || friction_basis_is_stale(manifold, epsilon);

bool can_skip_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon) {
        return true;
    return has_cached_friction_basis(manifold) && !friction_basis_is_stale(manifold, epsilon);

bool rebuild_friction_basis_if_needed(ContactManifold& manifold, f32 epsilon) {
        invalidate_friction_basis(manifold);

    if (can_skip_friction_basis_rebuild(manifold, epsilon)) {

    return ensure_friction_basis(manifold);

bool contact_normal_needs_normalize(const ContactManifold& manifold, f32 lengthEpsilon) {
    return manifold.needsNormalNormalization(lengthEpsilon);

bool should_normalize_contact_normal_before_friction(
    const ContactManifold& manifold,
    f32 lengthEpsilon) {
    return contact_normal_needs_normalize(manifold, lengthEpsilon);

void compute_friction_tangents_if_needed(ContactManifold& manifold, f32 epsilon) {
        return;


    const f32 normalLength = manifold.contactNormal.length();
    if (std::fabs(normalLength - 1.f) > 1e-4f) {
        manifold.contactNormal = manifold.contactNormal * (1.f / normalLength);
    manifold.buildFrictionBasis();

const char* friction_basis_reject_reason_name(FrictionBasisRejectReason reason) {
    switch (reason) {
    case FrictionBasisRejectReason::None:
        return "None";
    case FrictionBasisRejectReason::EmptyManifold:
        return "EmptyManifold";
    case FrictionBasisRejectReason::InvalidNormal:
        return "InvalidNormal";
    return "Unknown";

FrictionBasisRejectReason friction_basis_reject_reason(const ContactManifold& manifold) {
    if (manifold.empty()) {
        return FrictionBasisRejectReason::EmptyManifold;
    if (!manifold.hasValidNormal()) {
        return FrictionBasisRejectReason::InvalidNormal;
    return FrictionBasisRejectReason::None;

bool friction_basis_rejects_for_reason(
    FrictionBasisRejectReason expected) {
    return friction_basis_reject_reason(manifold) == expected;

FrictionBasisPreflight preflight_friction_basis_rebuild(
    f32 epsilon) {
    FrictionBasisPreflight preflight{};
    preflight.reason = friction_basis_reject_reason(manifold);
    if (preflight.reason != FrictionBasisRejectReason::None) {
        preflight.skipped = true;
        return preflight;

    preflight.stale = friction_basis_is_stale(manifold, epsilon);
    preflight.needsNormalNormalize = contact_normal_needs_normalize(manifold, epsilon);
    preflight.canReuse = can_skip_friction_basis_rebuild(manifold, epsilon);
    preflight.needsRebuild = needs_friction_basis_refresh(manifold, epsilon);

bool should_skip_friction_basis_preflight(
    return preflight_friction_basis_rebuild(manifold, epsilon).can_skip_rebuild();

bool rebuild_friction_basis_with_preflight(ContactManifold& manifold, f32 epsilon) {
    const FrictionBasisPreflight preflight = preflight_friction_basis_rebuild(manifold, epsilon);
    if (preflight.can_skip_rebuild()) {
        return manifold.hasFrictionBasis();
    if (preflight.needsNormalNormalize) {
        if (normalLength > 1e-8f) {
    return rebuild_friction_basis_if_needed(manifold, epsilon);

bool should_run_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon) {
    return !should_skip_friction_basis_preflight(manifold, epsilon);

bool should_skip_friction_for_manifold(
    f32 staticFriction,
    f32 dynamicFriction,
    f32 normalImpulse,
    f32 impulseEpsilon) {
    return should_skip_friction_solve(staticFriction, dynamicFriction, normalImpulse, impulseEpsilon);

vec2 combine_body_friction_coefficients(
    const RigidBodySoA& bodies,
    u32 bodyA,
    u32 bodyB) {
    if (bodyA >= bodies.count() || bodyB >= bodies.count()) {
        return {};

    const f32 staticA = bodies.frictionStatic[bodyA];
    const f32 staticB = bodies.frictionStatic[bodyB];
    const f32 dynamicA = bodies.frictionDynamic[bodyA];
    const f32 dynamicB = bodies.frictionDynamic[bodyB];
    return {
        std::sqrt(staticA * staticB),
        std::sqrt(dynamicA * dynamicB),
    };

bool should_rebuild_friction_basis(
    vec3 previousNormal,
    vec3 currentNormal,
    f32 angleThresholdRadians) {
    const f32 previousLength = previousNormal.length();
    const f32 currentLength = currentNormal.length();
    if (previousLength <= 1e-8f || currentLength <= 1e-8f) {

    const vec3 unitPrevious = previousNormal * (1.f / previousLength);
    const vec3 unitCurrent = currentNormal * (1.f / currentLength);
    const f32 cosine = std::max(-1.f, std::min(1.f, unitPrevious.dot(unitCurrent)));
    return std::acos(cosine) > angleThresholdRadians;
bool isValidFrictionBasisForNormal(vec3 normal, const TangentBasis& basis, f32 epsilon) {
    return isOrthonormalTangentBasis(normal, basis, epsilon);

bool needs_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon) {
    return !isValidFrictionBasisForNormal(manifold.contactNormal, manifold.frictionBasis, epsilon);

bool should_rebuild_friction_basis(const ContactManifold& manifold, f32 epsilon) {
    return needs_friction_basis_rebuild(manifold, epsilon);

bool ensure_friction_basis(ContactManifold& manifold, f32 epsilon) {
        manifold.frictionBasis = {};

    if (!needs_friction_basis_rebuild(manifold, epsilon)) {

    if (!manifold.normalizeContactNormal()) {

}

FrictionBasisPreflight preflight_friction_basis(const ContactManifold& manifold, f32 epsilon) {
    FrictionBasisPreflight preflight{};
    if (should_skip_friction_tangents(manifold)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.stale = friction_basis_is_stale(manifold, epsilon);
    preflight.needsRefresh = needs_friction_basis_refresh(manifold, epsilon);
    preflight.canSkipRebuild = can_skip_friction_basis_rebuild(manifold, epsilon);
    return preflight;
}

bool should_skip_friction_basis_compute(const ContactManifold& manifold, f32 epsilon) {
    return preflight_friction_basis(manifold, epsilon).can_skip_compute();
}

void invalidate_friction_basis_if_stale(ContactManifold& manifold, f32 epsilon) {
    if (friction_basis_is_stale(manifold, epsilon)) {
        invalidate_friction_basis(manifold);
    }
}

bool rebuild_friction_basis_from_preflight(
    ContactManifold& manifold,
    const FrictionBasisPreflight& preflight) {
    if (preflight.skipped) {
        invalidate_friction_basis(manifold);
        return false;
    }

    if (preflight.canSkipRebuild) {
        return manifold.hasFrictionBasis();
    }

    if (preflight.stale) {
        invalidate_friction_basis(manifold);
    }

    return ensure_friction_basis(manifold);
}

FrictionBasisPreflight preflight_friction_basis_rebuild(
    const ContactManifold& manifold,
    f32 epsilon) {
    FrictionBasisPreflight preflight{};
    if (should_skip_friction_tangents(manifold)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.missing = !has_cached_friction_basis(manifold);
    preflight.stale = friction_basis_is_stale(manifold, epsilon);
    return preflight;
}

bool can_skip_friction_tangents_rebuild(const ContactManifold& manifold, f32 epsilon) {
    return can_skip_friction_basis_rebuild(manifold, epsilon);
}

bool ensure_friction_basis_if_needed(ContactManifold& manifold, f32 epsilon) {
    return rebuild_friction_basis_if_needed(manifold, epsilon);
}

FrictionBasisPreflight preflight_friction_basis_rebuild(
    const ContactManifold& manifold,
    f32 epsilon) {
    FrictionBasisPreflight preflight{};
    if (should_skip_friction_tangents(manifold)) {
        preflight.skipped = true;
        return preflight;
    }

    if (friction_basis_is_stale(manifold, epsilon)) {
        preflight.staleBasis = true;
        return preflight;
    }

    if (!has_cached_friction_basis(manifold)) {
        preflight.missingBasis = true;
    }
    return preflight;
}

bool should_skip_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon) {
    const FrictionBasisPreflight preflight = preflight_friction_basis_rebuild(manifold, epsilon);
    return preflight.skipped || preflight.can_reuse_cached();
}

bool rebuild_friction_basis_from_preflight(ContactManifold& manifold, f32 epsilon) {
    const FrictionBasisPreflight preflight = preflight_friction_basis_rebuild(manifold, epsilon);
    if (preflight.skipped) {
        invalidate_friction_basis(manifold);
        return false;
    }

    if (preflight.can_reuse_cached()) {
        return true;
    }

    return rebuild_friction_basis_if_needed(manifold, epsilon);
}

FrictionBasisPreflight preflight_friction_basis_rebuild(
    const ContactManifold& manifold,
    f32 epsilon) {
    FrictionBasisPreflight preflight{};
    if (should_skip_friction_tangents(manifold)) {
        preflight.shouldSkipTangents = true;
        preflight.skipped = true;
        return preflight;
    }

    preflight.missingBasis = !has_cached_friction_basis(manifold);
    preflight.staleBasis = friction_basis_is_stale(manifold, epsilon);
    return preflight;
}

bool should_skip_friction_basis_rebuild_preflight(
    const ContactManifold& manifold,
    f32 epsilon) {
    return preflight_friction_basis_rebuild(manifold, epsilon).can_skip_rebuild();
}

void rebuild_friction_basis_guarded(ContactManifold& manifold, f32 epsilon) {
    const FrictionBasisPreflight preflight = preflight_friction_basis_rebuild(manifold, epsilon);
    if (preflight.skipped) {
        invalidate_friction_basis(manifold);
        return;
    }
    if (preflight.can_skip_rebuild()) {
        return;
    }
    compute_friction_tangents_if_needed(manifold, epsilon);
}

FrictionBasisPreflight preflight_friction_basis_rebuild(
    const ContactManifold& manifold,
    f32 epsilon) {
    FrictionBasisPreflight preflight{};
    if (should_skip_friction_tangents(manifold)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.stale = friction_basis_is_stale(manifold, epsilon);
    preflight.missing = !has_cached_friction_basis(manifold);
    preflight.needsNormalization = contact_normal_needs_normalization(manifold, epsilon);
    return preflight;
}

bool should_skip_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon) {
    return !preflight_friction_basis_rebuild(manifold, epsilon).needs_rebuild();
}

bool contact_normal_needs_normalization(const ContactManifold& manifold, f32 epsilon) {
    if (!manifold.hasValidNormal()) {
        return false;
    }
    const f32 normalLength = manifold.contactNormal.length();
    return std::fabs(normalLength - 1.f) > epsilon;
}

FrictionBasisRebuildResult rebuild_friction_basis_guarded(ContactManifold& manifold, f32 epsilon) {
    FrictionBasisRebuildResult result{};
    const FrictionBasisPreflight preflight = preflight_friction_basis_rebuild(manifold, epsilon);
    if (preflight.skipped) {
        invalidate_friction_basis(manifold);
        result.skipped = true;
        return result;
    }

    if (preflight.needsNormalization) {
        const f32 normalLength = manifold.contactNormal.length();
        if (normalLength > epsilon) {
            manifold.contactNormal = manifold.contactNormal * (1.f / normalLength);
        }
    }

    if (!preflight.needs_rebuild()) {
        result.skipped = true;
        return result;
    }

    result.rebuilt = rebuild_friction_basis_if_needed(manifold, epsilon);
    return result;
}

} // namespace fuse::physics::narrowphase
