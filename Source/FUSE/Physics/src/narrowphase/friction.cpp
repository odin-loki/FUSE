#include <fuse/physics/narrowphase/friction.hpp>

#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/narrowphase/contact_pair.hpp>

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

    if (should_skip_friction_basis_preflight(manifold, epsilon)) {
        if (should_skip_friction_tangents(manifold)) {
            invalidate_friction_basis(manifold);
        }
    if (!should_run_friction_basis_rebuild(manifold, epsilon)) {

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

const char* friction_basis_reject_reason_name(FrictionBasisRejectReason reason) {
    switch (reason) {
    case FrictionBasisRejectReason::None:
        return "None";
    case FrictionBasisRejectReason::Skipped:
        return "Skipped";
    case FrictionBasisRejectReason::MissingNormal:
        return "MissingNormal";
    case FrictionBasisRejectReason::StaleBasis:
        return "StaleBasis";
    }
    return "Unknown";
}

FrictionBasisRejectReason friction_basis_reject_reason(
    const ContactManifold& manifold,
    f32 epsilon) {
    if (manifold.empty()) {
        return FrictionBasisRejectReason::Skipped;
    }
    if (!manifold.hasValidNormal()) {
        return FrictionBasisRejectReason::MissingNormal;
    }
    if (friction_basis_is_stale(manifold, epsilon)) {
        return FrictionBasisRejectReason::StaleBasis;
    }
    return FrictionBasisRejectReason::None;
}

bool friction_basis_rejects_for_reason(
    const ContactManifold& manifold,
    FrictionBasisRejectReason expected,
    f32 epsilon) {
    return friction_basis_reject_reason(manifold, epsilon) == expected;
}

const char* friction_basis_reject_reason_name(FrictionBasisRejectReason reason) {
    switch (reason) {
    case FrictionBasisRejectReason::None:
        return "None";
    case FrictionBasisRejectReason::EmptyManifold:
        return "EmptyManifold";
    case FrictionBasisRejectReason::InvalidNormal:
        return "InvalidNormal";
    case FrictionBasisRejectReason::StaleBasis:
        return "StaleBasis";
    }
    return "Unknown";
}

FrictionBasisRejectReason friction_basis_reject_reason(const ContactManifold& manifold) {
    if (manifold.empty()) {
        return FrictionBasisRejectReason::EmptyManifold;
    }
    if (!manifold.hasValidNormal()) {
        return FrictionBasisRejectReason::InvalidNormal;
    }
    if (friction_basis_is_stale(manifold)) {
        return FrictionBasisRejectReason::StaleBasis;
    }
    return FrictionBasisRejectReason::None;
}

bool friction_basis_rejects_for_reason(
    const ContactManifold& manifold,
    FrictionBasisRejectReason expected) {
    return friction_basis_reject_reason(manifold) == expected;
}

const char* friction_basis_reject_reason_name(FrictionBasisRejectReason reason) {
    switch (reason) {
    case FrictionBasisRejectReason::None:
        return "None";
    case FrictionBasisRejectReason::SkippedManifold:
        return "SkippedManifold";
    case FrictionBasisRejectReason::ValidCachedBasis:
        return "ValidCachedBasis";
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
        return FrictionBasisRejectReason::ValidCachedBasis;
    }
    return FrictionBasisRejectReason::None;
}

bool friction_basis_rejects_for_reason(
    const ContactManifold& manifold,
    FrictionBasisRejectReason expected,
    f32 epsilon) {
    return friction_basis_reject_reason(manifold, epsilon) == expected;
}

const char* friction_basis_reject_reason_name(FrictionBasisRejectReason reason) {
    switch (reason) {
    case FrictionBasisRejectReason::None:
        return "None";
    case FrictionBasisRejectReason::EmptyOrInvalid:
        return "EmptyOrInvalid";
    case FrictionBasisRejectReason::BasisReusable:
        return "BasisReusable";
    }
    return "Unknown";
}

FrictionBasisRejectReason friction_basis_reject_reason(
    const ContactManifold& manifold,
    f32 epsilon) {
    if (should_skip_friction_tangents(manifold)) {
        return FrictionBasisRejectReason::EmptyOrInvalid;
    }

    if (can_skip_friction_basis_rebuild(manifold, epsilon)) {
        return FrictionBasisRejectReason::BasisReusable;
    }

    return FrictionBasisRejectReason::None;
}

bool friction_basis_rejects_for_reason(
    const ContactManifold& manifold,
    FrictionBasisRejectReason expected,
    f32 epsilon) {
    return friction_basis_reject_reason(manifold, epsilon) == expected;
}

const char* friction_basis_reject_reason_name(FrictionBasisRejectReason reason) {
    switch (reason) {
    case FrictionBasisRejectReason::None:
        return "None";
    case FrictionBasisRejectReason::EmptyManifold:
        return "EmptyManifold";
    case FrictionBasisRejectReason::InvalidNormal:
        return "InvalidNormal";
    case FrictionBasisRejectReason::CanReuseBasis:
        return "CanReuseBasis";
    }
    return "Unknown";
}

FrictionBasisRejectReason friction_basis_reject_reason(
    const ContactManifold& manifold,
    f32 epsilon) {
    if (manifold.empty()) {
        return FrictionBasisRejectReason::EmptyManifold;
    }
    if (!manifold.hasValidNormal()) {
        return FrictionBasisRejectReason::InvalidNormal;
    }
    if (can_skip_friction_basis_rebuild(manifold, epsilon)) {
        return FrictionBasisRejectReason::CanReuseBasis;
    }
    return FrictionBasisRejectReason::None;
}

bool friction_basis_rejects_for_reason(
    const ContactManifold& manifold,
    FrictionBasisRejectReason expected,
    f32 epsilon) {
    return friction_basis_reject_reason(manifold, epsilon) == expected;
}

const char* friction_basis_reject_reason_name(FrictionBasisRejectReason reason) {
    switch (reason) {
    case FrictionBasisRejectReason::None:
        return "None";
    case FrictionBasisRejectReason::EmptyManifold:
        return "EmptyManifold";
    case FrictionBasisRejectReason::InvalidNormal:
        return "InvalidNormal";
    case FrictionBasisRejectReason::CanReuse:
        return "CanReuse";
    }
    return "Unknown";
}

FrictionBasisRejectReason friction_basis_reject_reason(
    const ContactManifold& manifold,
    f32 epsilon) {
    if (manifold.empty()) {
        return FrictionBasisRejectReason::EmptyManifold;
    }
    if (!manifold.hasValidNormal()) {
        return FrictionBasisRejectReason::InvalidNormal;
    }
    if (can_skip_friction_basis_rebuild(manifold, epsilon)) {
        return FrictionBasisRejectReason::CanReuse;
    }
    return FrictionBasisRejectReason::None;
}

bool friction_basis_rejects_for_reason(
    const ContactManifold& manifold,
    FrictionBasisRejectReason expected,
    f32 epsilon) {
    return friction_basis_reject_reason(manifold, epsilon) == expected;
}

const char* friction_basis_rebuild_reject_reason_name(FrictionBasisRebuildRejectReason reason) {
    switch (reason) {
    case FrictionBasisRebuildRejectReason::None:
        return "None";
    case FrictionBasisRebuildRejectReason::Skipped:
        return "Skipped";
    case FrictionBasisRebuildRejectReason::CanReuse:
        return "CanReuse";
    }
    return "Unknown";
}

FrictionBasisRebuildRejectReason friction_basis_rebuild_reject_reason(
    const ContactManifold& manifold,
    f32 epsilon) {
    if (should_skip_friction_tangents(manifold)) {
        return FrictionBasisRebuildRejectReason::Skipped;
    }
    if (can_skip_friction_basis_rebuild(manifold, epsilon)) {
        return FrictionBasisRebuildRejectReason::CanReuse;
    }
    return FrictionBasisRebuildRejectReason::None;
}

bool friction_basis_rebuild_rejects_for_reason(
    const ContactManifold& manifold,
    FrictionBasisRebuildRejectReason expected,
    f32 epsilon) {
    return friction_basis_rebuild_reject_reason(manifold, epsilon) == expected;
}

const char* friction_basis_rebuild_reject_reason_name(FrictionBasisRebuildRejectReason reason) {
    switch (reason) {
    case FrictionBasisRebuildRejectReason::None:
        return "None";
    case FrictionBasisRebuildRejectReason::EmptyManifold:
        return "EmptyManifold";
    case FrictionBasisRebuildRejectReason::NoValidNormal:
        return "NoValidNormal";
    case FrictionBasisRebuildRejectReason::CanReuseBasis:
        return "CanReuseBasis";
    }
    return "Unknown";
}

FrictionBasisRebuildRejectReason friction_basis_rebuild_reject_reason(
    const ContactManifold& manifold,
    f32 epsilon) {
    if (should_skip_friction_tangents(manifold)) {
        if (manifold.empty()) {
            return FrictionBasisRebuildRejectReason::EmptyManifold;
        }
        return FrictionBasisRebuildRejectReason::NoValidNormal;
    }
    if (can_skip_friction_basis_rebuild(manifold, epsilon)) {
        return FrictionBasisRebuildRejectReason::CanReuseBasis;
    }
    return FrictionBasisRebuildRejectReason::None;
}

bool friction_basis_rebuild_rejects_for_reason(
    const ContactManifold& manifold,
    FrictionBasisRebuildRejectReason expected,
    f32 epsilon) {
    return friction_basis_rebuild_reject_reason(manifold, epsilon) == expected;
}

bool should_run_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon) {
    return friction_basis_rebuild_reject_reason(manifold, epsilon) == FrictionBasisRebuildRejectReason::None;
}

const char* friction_basis_rebuild_reject_reason_name(FrictionBasisRebuildRejectReason reason) {
    switch (reason) {
    case FrictionBasisRebuildRejectReason::None:
        return "None";
    case FrictionBasisRebuildRejectReason::EmptyManifold:
        return "EmptyManifold";
    case FrictionBasisRebuildRejectReason::NoValidNormal:
        return "NoValidNormal";
    case FrictionBasisRebuildRejectReason::CanReuseCached:
        return "CanReuseCached";
    }
    return "Unknown";
}

FrictionBasisRebuildRejectReason friction_basis_rebuild_reject_reason(
    const ContactManifold& manifold,
    f32 epsilon) {
    if (manifold.empty()) {
        return FrictionBasisRebuildRejectReason::EmptyManifold;
    }
    if (!manifold.hasValidNormal()) {
        return FrictionBasisRebuildRejectReason::NoValidNormal;
    }
    if (can_skip_friction_basis_rebuild(manifold, epsilon)) {
        return FrictionBasisRebuildRejectReason::CanReuseCached;
    }
    return FrictionBasisRebuildRejectReason::None;
}

bool friction_basis_rebuild_rejects_for_reason(
    const ContactManifold& manifold,
    FrictionBasisRebuildRejectReason expected,
    f32 epsilon) {
    return friction_basis_rebuild_reject_reason(manifold, epsilon) == expected;
}

bool should_run_friction_basis_rebuild(
    const ContactManifold& manifold,
    f32 epsilon) {
    return !should_skip_friction_basis_preflight(manifold, epsilon);
}

FrictionBasisPreflight preflight_friction_basis_rebuild(
    f32 epsilon) {
    FrictionBasisPreflight preflight{};
    preflight.reason = friction_basis_reject_reason(manifold);
    if (preflight.reason != FrictionBasisRejectReason::None) {
    preflight.reason = friction_basis_reject_reason(manifold, epsilon);
    if (preflight.reason == FrictionBasisRejectReason::EmptyOrInvalid) {
        preflight.skipped = true;
        preflight.skipped = preflight.reason == FrictionBasisRejectReason::EmptyManifold ||
                            preflight.reason == FrictionBasisRejectReason::InvalidNormal;
        if (preflight.reason == FrictionBasisRejectReason::CanReuseBasis) {
            preflight.canReuse = true;
        }
        return preflight;

    preflight.stale = friction_basis_is_stale(manifold, epsilon);
    preflight.needsNormalNormalize = contact_normal_needs_normalize(manifold, epsilon);
    preflight.canReuse = false;
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

    preflight.canReuse = preflight.reason == FrictionBasisRejectReason::BasisReusable;
    preflight.needsRebuild = preflight.reason == FrictionBasisRejectReason::None;

FrictionBasisPreflight preflight_friction_basis(const ContactManifold& manifold, f32 epsilon) {
    FrictionBasisPreflight preflight{};
    preflight.rejectReason = friction_basis_reject_reason(manifold, epsilon);
    preflight.reason = friction_basis_rebuild_reject_reason(manifold, epsilon);
    if (should_skip_friction_tangents(manifold)) {
    if (preflight.reason == FrictionBasisRejectReason::SkippedManifold) {
    if (manifold.empty()) {
        preflight.reason = FrictionBasisRejectReason::EmptyManifold;
    if (!manifold.hasValidNormal()) {
        preflight.reason = FrictionBasisRejectReason::InvalidNormal;
        preflight.skipped = true;
        return preflight;
    }

    if (preflight.reason == FrictionBasisRejectReason::ValidCachedBasis) {
        preflight.canReuse = true;
        return preflight;
    }

    preflight.reason = FrictionBasisRejectReason::None;
    preflight.stale = friction_basis_is_stale(manifold, epsilon);
    preflight.needsRefresh = needs_friction_basis_refresh(manifold, epsilon);
    preflight.canSkipRebuild = can_skip_friction_basis_rebuild(manifold, epsilon);

bool should_skip_friction_basis_compute(const ContactManifold& manifold, f32 epsilon) {
    return preflight_friction_basis(manifold, epsilon).can_skip_compute();

void invalidate_friction_basis_if_stale(ContactManifold& manifold, f32 epsilon) {
    if (friction_basis_is_stale(manifold, epsilon)) {
        invalidate_friction_basis(manifold);

bool rebuild_friction_basis_from_preflight(
    ContactManifold& manifold,
    const FrictionBasisPreflight& preflight) {
    if (preflight.skipped) {
        return false;

    if (preflight.canSkipRebuild) {
        return manifold.hasFrictionBasis();

    if (preflight.stale) {

    return ensure_friction_basis(manifold);

FrictionBasisPreflight preflight_friction_basis_rebuild(
    const ContactManifold& manifold,
    f32 epsilon) {
    preflight.reason = friction_basis_reject_reason(manifold, epsilon);

    preflight.missing = !has_cached_friction_basis(manifold);

bool can_skip_friction_tangents_rebuild(const ContactManifold& manifold, f32 epsilon) {
    return can_skip_friction_basis_rebuild(manifold, epsilon);

bool ensure_friction_basis_if_needed(ContactManifold& manifold, f32 epsilon) {
    return rebuild_friction_basis_if_needed(manifold, epsilon);

const char* friction_basis_reject_reason_name(FrictionBasisRejectReason reason) {
    switch (reason) {
    case FrictionBasisRejectReason::None:
        return "None";
    case FrictionBasisRejectReason::EmptyManifold:
        return "EmptyManifold";
    case FrictionBasisRejectReason::InvalidNormal:
        return "InvalidNormal";
    case FrictionBasisRejectReason::MissingBasis:
        return "MissingBasis";
    case FrictionBasisRejectReason::StaleBasis:
        return "StaleBasis";
    return "Unknown";

FrictionBasisRejectReason friction_basis_reject_reason(
    if (manifold.empty()) {
        return FrictionBasisRejectReason::EmptyManifold;
    if (!manifold.hasValidNormal()) {
        return FrictionBasisRejectReason::InvalidNormal;
        return FrictionBasisRejectReason::StaleBasis;
    if (!has_cached_friction_basis(manifold)) {
        return FrictionBasisRejectReason::MissingBasis;
    return FrictionBasisRejectReason::None;

bool friction_basis_rejects_for_reason(
    FrictionBasisRejectReason expected,
    return friction_basis_reject_reason(manifold, epsilon) == expected;

    FrictionBasisPreflight preflight{};
    if (should_skip_friction_tangents(manifold)) {
        preflight.skipped = true;
        return preflight;

        preflight.staleBasis = true;

        preflight.missingBasis = true;

bool should_skip_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon) {
    const FrictionBasisPreflight preflight = preflight_friction_basis_rebuild(manifold, epsilon);
    return preflight.skipped || preflight.can_reuse_cached();

bool rebuild_friction_basis_from_preflight(ContactManifold& manifold, f32 epsilon) {

    if (preflight.can_reuse_cached()) {
        return true;


        preflight.shouldSkipTangents = true;

    preflight.missingBasis = !has_cached_friction_basis(manifold);
    preflight.staleBasis = friction_basis_is_stale(manifold, epsilon);

bool should_skip_friction_basis_rebuild_preflight(
    case FrictionBasisRejectReason::SkippedManifold:
        return "SkippedManifold";
    case FrictionBasisRejectReason::BasisCurrent:
        return "BasisCurrent";

        return FrictionBasisRejectReason::SkippedManifold;
    if (can_skip_friction_basis_rebuild(manifold, epsilon)) {
        return FrictionBasisRejectReason::BasisCurrent;

    preflight.canReuse = can_skip_friction_basis_rebuild(manifold, epsilon);
    preflight.needsRebuild = needs_friction_basis_refresh(manifold, epsilon);
    if (preflight.canReuse) {
        preflight.rejectReason = FrictionBasisRejectReason::None;
    }

const char* friction_basis_rebuild_reject_reason_name(FrictionBasisRebuildRejectReason reason) {
    switch (reason) {
    case FrictionBasisRebuildRejectReason::None:
        return "None";
    case FrictionBasisRebuildRejectReason::EmptyManifold:
        return "EmptyManifold";
    case FrictionBasisRebuildRejectReason::InvalidNormal:
        return "InvalidNormal";
    case FrictionBasisRebuildRejectReason::CanReuseBasis:
        return "CanReuseBasis";
    }
    return "Unknown";
}

FrictionBasisRebuildRejectReason friction_basis_rebuild_reject_reason(
    const ContactManifold& manifold,
    f32 epsilon) {
    if (should_skip_friction_tangents(manifold)) {
        if (manifold.empty()) {
            return FrictionBasisRebuildRejectReason::EmptyManifold;
        }
        return FrictionBasisRebuildRejectReason::InvalidNormal;
    }
    if (can_skip_friction_basis_rebuild(manifold, epsilon)) {
        return FrictionBasisRebuildRejectReason::CanReuseBasis;
    }
    return FrictionBasisRebuildRejectReason::None;
}

bool friction_basis_rebuild_rejects_for_reason(
    const ContactManifold& manifold,
    FrictionBasisRebuildRejectReason expected,
    f32 epsilon) {
    return friction_basis_rebuild_reject_reason(manifold, epsilon) == expected;
}

bool should_skip_friction_basis_preflight(
    const ContactManifold& manifold,
    f32 epsilon) {
    return friction_basis_reject_reason(manifold, epsilon) != FrictionBasisRejectReason::None;
}

void rebuild_friction_basis_guarded(ContactManifold& manifold, f32 epsilon) {
    const FrictionBasisPreflight preflight = preflight_friction_basis_rebuild(manifold, epsilon);
    if (preflight.skipped) {
        invalidate_friction_basis(manifold);
        return;
    }
    if (preflight.can_skip_rebuild()) {
    compute_friction_tangents_if_needed(manifold, epsilon);

const char* friction_basis_reject_reason_name(FrictionBasisRejectReason reason) {
    switch (reason) {
    case FrictionBasisRejectReason::None:
        return "None";
    case FrictionBasisRejectReason::SkippedManifold:
        return "SkippedManifold";
    case FrictionBasisRejectReason::ValidCachedBasis:
        return "ValidCachedBasis";
    return "Unknown";
    case FrictionBasisRejectReason::EmptyManifold:
        return "EmptyManifold";
    case FrictionBasisRejectReason::InvalidNormal:
        return "InvalidNormal";
    case FrictionBasisRejectReason::StaleBasis:
        return "StaleBasis";

FrictionBasisRejectReason friction_basis_reject_reason(
    const ContactManifold& manifold,
    f32 epsilon) {
    if (should_skip_friction_tangents(manifold)) {
        return FrictionBasisRejectReason::SkippedManifold;
    if (can_skip_friction_basis_rebuild(manifold, epsilon)) {
        return FrictionBasisRejectReason::ValidCachedBasis;
    return FrictionBasisRejectReason::None;

bool friction_basis_rejects_for_reason(
    FrictionBasisRejectReason expected,
    return friction_basis_reject_reason(manifold, epsilon) == expected;

FrictionBasisPreflight preflight_friction_basis_rebuild(
    FrictionBasisPreflight preflight{};
    preflight.reason = friction_basis_reject_reason(manifold, epsilon);
    if (preflight.reason == FrictionBasisRejectReason::SkippedManifold) {
        preflight.skipped = true;
        return preflight;

    if (preflight.reason == FrictionBasisRejectReason::ValidCachedBasis) {
        preflight.canReuse = true;

    preflight.stale = friction_basis_is_stale(manifold, epsilon);
    preflight.missing = !has_cached_friction_basis(manifold);
    preflight.needsNormalization = contact_normal_needs_normalization(manifold, epsilon);
    preflight.needsRebuild = needs_friction_basis_refresh(manifold, epsilon);

bool should_skip_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon) {
    return !preflight_friction_basis_rebuild(manifold, epsilon).needs_rebuild();

bool contact_normal_needs_normalization(const ContactManifold& manifold, f32 epsilon) {
    if (!manifold.hasValidNormal()) {
        return false;
    const f32 normalLength = manifold.contactNormal.length();
    return std::fabs(normalLength - 1.f) > epsilon;

FrictionBasisRebuildResult rebuild_friction_basis_guarded(ContactManifold& manifold, f32 epsilon) {
    FrictionBasisRebuildResult result{};
        result.skipped = true;
        return result;

    if (preflight.needsNormalization) {
        if (normalLength > epsilon) {
            manifold.contactNormal = manifold.contactNormal * (1.f / normalLength);

    if (!preflight.needs_rebuild()) {

    result.rebuilt = rebuild_friction_basis_if_needed(manifold, epsilon);


    preflight.canReuse = can_skip_friction_basis_rebuild(manifold, epsilon);

bool ensure_friction_basis_if_needed(ContactManifold& manifold, f32 epsilon) {
    return rebuild_friction_basis_if_needed(manifold, epsilon);


    preflight.hasCachedBasis = has_cached_friction_basis(manifold);
    preflight.wouldRebuild = needs_friction_basis_refresh(manifold, epsilon);

bool should_skip_friction_basis_rebuild_preflight(const FrictionBasisPreflight& preflight) {
    return preflight.skipped || preflight.can_reuse();

bool can_reuse_friction_basis(const FrictionBasisPreflight& preflight) {
    return preflight.can_reuse();

FrictionBasisRebuildPreflight preflight_friction_basis_rebuild(
    FrictionBasisRebuildPreflight preflight{};
        preflight.skipTangents = true;

    preflight.isStale = friction_basis_is_stale(manifold, epsilon);

bool compute_friction_tangents_guarded(ContactManifold& manifold, f32 epsilon) {
    const FrictionBasisRebuildPreflight preflight = preflight_friction_basis_rebuild(manifold, epsilon);
    if (preflight.skipTangents) {

        return manifold.hasFrictionBasis();


bool ensure_friction_basis_guarded(ContactManifold& manifold, f32 epsilon) {






bool rebuild_friction_basis_from_preflight(ContactManifold& manifold, f32 epsilon) {
        return preflight.canReuse;

FrictionBasisPreflight preflight_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon) {
        preflight.shouldSkip = true;


bool should_skip_friction_basis_preflight(const ContactManifold& manifold) {
    return preflight_friction_basis_rebuild(manifold).skipped;
    return friction_basis_reject_reason(manifold, epsilon) != FrictionBasisRejectReason::None;
bool rebuild_friction_basis_with_preflight(ContactManifold& manifold, f32 epsilon) {

void compute_friction_tangents_with_preflight(ContactManifold& manifold, f32 epsilon) {
        return !preflight.skipped && has_cached_friction_basis(manifold);

    return ensure_friction_basis(manifold);

bool ensure_friction_basis_from_preflight(ContactManifold& manifold, f32 epsilon) {
    if (preflight.canReuse) {
        return true;
    return rebuild_friction_basis_from_preflight(manifold, epsilon);
bool should_rebuild_friction_basis(
    return !should_skip_friction_basis_preflight(manifold, epsilon);



    if (!preflight.needsRebuild) {

bool can_run_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon) {
    return preflight.can_rebuild();

bool has_partial_friction_basis(const ContactManifold& manifold, f32 epsilon) {

    const f32 tangent1Length = manifold.frictionBasis.tangent1.length();
    const f32 tangent2Length = manifold.frictionBasis.tangent2.length();
    const bool hasTangent1 = tangent1Length > epsilon;
    const bool hasTangent2 = tangent2Length > epsilon;
    if (hasTangent1 != hasTangent2) {
    if (!hasTangent1) {

    return !isOrthonormalTangentBasis(manifold.contactNormal, manifold.frictionBasis, epsilon);

bool friction_basis_needs_completion(const ContactManifold& manifold, f32 epsilon) {
    return !has_cached_friction_basis(manifold) || has_partial_friction_basis(manifold, epsilon);

FrictionBasisDeepenPreflight preflight_friction_basis_rebuild_deepen(
    f32 epsilon,
    f32 normalEpsilon) {
    FrictionBasisDeepenPreflight preflight{};

    const FrictionBasisPreflight basePreflight = preflight_friction_basis_rebuild(manifold, epsilon);
    preflight.stale = basePreflight.stale;
    preflight.canReuse = basePreflight.canReuse;
    preflight.needsRebuild = basePreflight.needsRebuild;
    preflight.partial = has_partial_friction_basis(manifold, epsilon);
    preflight.needsNormalNormalization = manifold.hasUnnormalizedNormal(normalEpsilon);
    if (preflight.partial) {
        preflight.needsRebuild = true;
        preflight.canReuse = false;

bool should_skip_friction_basis_deepen_preflight(
    return preflight_friction_basis_rebuild_deepen(manifold, epsilon, normalEpsilon).can_skip_rebuild();

void compute_friction_tangents_deepen_if_needed(
    ContactManifold& manifold,

    manifold.normalizeContactNormalIfNeeded(normalEpsilon);

    const FrictionBasisDeepenPreflight preflight =
        preflight_friction_basis_rebuild_deepen(manifold, epsilon, normalEpsilon);

    manifold.buildFrictionBasis();
WarmStartFrictionPreflight preflight_warm_start_friction(
    f32 impulseEpsilon,
    f32 basisEpsilon) {
    WarmStartFrictionPreflight preflight{};
    if (manifold.empty() || !manifold.hasValidNormal()) {

    preflight.hasWarmImpulse =
        std::fabs(manifold.warmNormalImpulse) > impulseEpsilon ||
        tangentialSpeed(manifold.warmTangentImpulse) > impulseEpsilon;
    preflight.hasValidBasis = can_skip_friction_basis_rebuild(manifold, basisEpsilon);

bool should_skip_warm_start_friction(
    return !preflight_warm_start_friction(manifold, impulseEpsilon, basisEpsilon).can_warm_start();
bool rebuild_friction_basis_preflight_dispatch(ContactManifold& manifold, f32 epsilon) {

    if (manifold.empty()) {
        return FrictionBasisRejectReason::EmptyManifold;
        return FrictionBasisRejectReason::InvalidNormal;
    if (friction_basis_is_stale(manifold, epsilon)) {
        return FrictionBasisRejectReason::StaleBasis;


bool can_skip_friction_basis_preflight(
    return should_skip_friction_basis_preflight(manifold, epsilon);
FrictionBasisEnsurePreflight preflight_friction_basis_ensure(
    FrictionBasisEnsurePreflight preflight{};

    preflight.needsEnsure = !preflight.canReuse;

bool can_skip_friction_basis_ensure(
    return preflight_friction_basis_ensure(manifold, epsilon).can_skip_ensure();

    if (can_skip_friction_basis_ensure(manifold, epsilon)) {
        return !should_skip_friction_tangents(manifold);

bool can_dispatch_friction_basis_rebuild(
    const FrictionBasisRejectReason reason = friction_basis_reject_reason(manifold, epsilon);
    return reason == FrictionBasisRejectReason::None ||
           reason == FrictionBasisRejectReason::StaleBasis;
        return preflight.reason == FrictionBasisRejectReason::None && preflight.canReuse;

        if (preflight.reason != FrictionBasisRejectReason::None) {

bool should_run_friction_basis_rebuild(

    const FrictionBasisPreflight preflight = preflight_friction_basis_rebuild(manifold, epsilon);

    if (preflight.reason == FrictionBasisRejectReason::EmptyManifold ||
        preflight.reason == FrictionBasisRejectReason::InvalidNormal) {
        invalidate_friction_basis(manifold);
        return false;
    }
    if (preflight.can_skip_rebuild()) {
        return true;
    }
    return rebuild_friction_basis_with_normalize_preflight(manifold, epsilon);
}

void normalize_contact_normal_for_friction(ContactManifold& manifold, f32 lengthEpsilon) {
    if (!should_normalize_contact_normal_before_friction(manifold, lengthEpsilon)) {
        return;
    }

    const f32 normalLength = manifold.contactNormal.length();
    if (normalLength > 1e-8f) {
        manifold.contactNormal = manifold.contactNormal * (1.f / normalLength);
    }
}

bool rebuild_friction_basis_with_normalize_preflight(ContactManifold& manifold, f32 epsilon) {
    const FrictionBasisPreflight preflight = preflight_friction_basis_rebuild(manifold, epsilon);
    if (preflight.reason == FrictionBasisRejectReason::EmptyManifold ||
        preflight.reason == FrictionBasisRejectReason::InvalidNormal) {
        invalidate_friction_basis(manifold);
        return false;
    }
    if (preflight.can_skip_rebuild()) {
        return manifold.hasFrictionBasis();
    }

    normalize_contact_normal_for_friction(manifold, epsilon);
    invalidate_friction_basis(manifold);
    return ensure_friction_basis(manifold);
}

bool should_rebuild_friction_basis(
    const ContactManifold& manifold,
    f32 epsilon) {
    return !should_skip_friction_basis_preflight(manifold, epsilon);
}

bool rebuild_friction_basis_from_preflight(ContactManifold& manifold, f32 epsilon) {
    const FrictionBasisPreflight preflight = preflight_friction_basis_rebuild(manifold, epsilon);
    if (preflight.skipped) {
        invalidate_friction_basis(manifold);
        return false;

    if (preflight.canReuse) {
        return true;

    if (!preflight.needsRebuild) {

    return rebuild_friction_basis_if_needed(manifold, epsilon);

bool can_run_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon) {
    return preflight.can_rebuild();

bool ensure_friction_basis_if_needed(ContactManifold& manifold, f32 epsilon) {
    if (preflight.can_skip_rebuild()) {

const char* friction_basis_rebuild_reject_reason_name(FrictionBasisRebuildRejectReason reason) {
    switch (reason) {
    case FrictionBasisRebuildRejectReason::None:
        return "None";
    case FrictionBasisRebuildRejectReason::SkippedEmpty:
        return "SkippedEmpty";
    case FrictionBasisRebuildRejectReason::SkippedNoNormal:
        return "SkippedNoNormal";
    case FrictionBasisRebuildRejectReason::CanReuse:
        return "CanReuse";
    case FrictionBasisRebuildRejectReason::NeedsRebuild:
        return "NeedsRebuild";
    return "Unknown";

FrictionBasisRebuildRejectReason friction_basis_rebuild_reject_reason(
    if (manifold.empty()) {
        return FrictionBasisRebuildRejectReason::SkippedEmpty;
    if (!manifold.hasValidNormal(epsilon)) {
        return FrictionBasisRebuildRejectReason::SkippedNoNormal;
    if (can_skip_friction_basis_rebuild(manifold, epsilon)) {
        return FrictionBasisRebuildRejectReason::CanReuse;
    if (needs_friction_basis_refresh(manifold, epsilon)) {
        return FrictionBasisRebuildRejectReason::NeedsRebuild;
    return FrictionBasisRebuildRejectReason::None;

bool friction_basis_rebuild_rejects_for_reason(
    FrictionBasisRebuildRejectReason expected,
    return friction_basis_rebuild_reject_reason(manifold, epsilon) == expected;

bool should_run_friction_basis_rebuild(
bool friction_basis_preflight_skips(const ContactManifold& manifold, f32 epsilon) {
    return should_skip_friction_basis_preflight(manifold, epsilon);

bool normalize_contact_normal_if_needed(ContactManifold& manifold, f32 lengthEpsilon) {
    if (!should_normalize_contact_normal_before_friction(manifold, lengthEpsilon)) {

    const f32 normalLength = manifold.contactNormal.length();
    if (normalLength <= lengthEpsilon) {

    if (normalLength < 1e-8f) {
    manifold.contactNormal = manifold.contactNormal * (1.f / normalLength);

bool ensure_friction_basis_after_preflight(ContactManifold& manifold, f32 epsilon) {
    if (should_skip_friction_basis_preflight(manifold, epsilon)) {
        if (should_skip_friction_tangents(manifold)) {
        return has_cached_friction_basis(manifold);

    normalize_contact_normal_if_needed(manifold, epsilon);



    return friction_basis_rebuild_reject_reason(manifold, epsilon) == FrictionBasisRebuildRejectReason::None;

bool can_dispatch_friction_basis_rebuild(
    return should_run_friction_basis_rebuild(manifold, epsilon);

FrictionBasisNormalizePreflight preflight_friction_basis_normalize_rebuild(
    FrictionBasisNormalizePreflight preflight{};
    preflight.rebuild = preflight_friction_basis_rebuild(manifold, epsilon);
    preflight.needsNormalize = should_normalize_contact_normal_before_friction(manifold, epsilon);
    return preflight;

bool should_skip_friction_basis_normalize_rebuild(
    return preflight_friction_basis_normalize_rebuild(manifold, epsilon).can_skip_all();

    case FrictionBasisRebuildRejectReason::EmptyManifold:
        return "EmptyManifold";
    case FrictionBasisRebuildRejectReason::InvalidNormal:
        return "InvalidNormal";

        return manifold.empty() ? FrictionBasisRebuildRejectReason::EmptyManifold
                                : FrictionBasisRebuildRejectReason::InvalidNormal;


bool should_run_friction_basis_rebuild(const ContactManifold& manifold, f32 epsilon) {

bool can_skip_friction_basis_rebuild_dispatch(const ContactManifold& manifold, f32 epsilon) {

const char* friction_basis_reject_reason_name(FrictionBasisRejectReason reason) {
    case FrictionBasisRejectReason::None:
    case FrictionBasisRejectReason::EmptyManifold:
    case FrictionBasisRejectReason::InvalidNormal:
    case FrictionBasisRejectReason::CanReuse:

FrictionBasisRejectReason friction_basis_reject_reason(
        return FrictionBasisRejectReason::EmptyManifold;
    if (!manifold.hasValidNormal()) {
        return FrictionBasisRejectReason::InvalidNormal;
        return FrictionBasisRejectReason::CanReuse;
    return FrictionBasisRejectReason::None;

bool friction_basis_rejects_for_reason(
    FrictionBasisRejectReason expected,
    return friction_basis_reject_reason(manifold, epsilon) == expected;



            return FrictionBasisRebuildRejectReason::EmptyManifold;
        return FrictionBasisRebuildRejectReason::InvalidNormal;


bool can_skip_friction_basis_preflight(

    return !can_skip_friction_basis_preflight(manifold, epsilon);


    case FrictionBasisRejectReason::Skipped:
        return "Skipped";

        return FrictionBasisRejectReason::Skipped;




    if (!contact_normal_needs_normalize(manifold, lengthEpsilon)) {

    if (normalLength <= 1e-8f) {






bool rebuild_friction_basis_using_preflight(ContactManifold& manifold, f32 epsilon) {

        return manifold.hasFrictionBasis();

    manifold.buildFrictionBasis();

bool rebuild_friction_basis_with_preflight(ContactManifold& manifold, f32 epsilon) {
        return !preflight.skipped && manifold.hasFrictionBasis();

    if (preflight.needsNormalNormalize) {
        if (std::fabs(normalLength - 1.f) > epsilon && normalLength > 1e-8f) {

    return ensure_friction_basis(manifold);

void compute_friction_tangents_with_preflight(ContactManifold& manifold, f32 epsilon) {
        return;


    rebuild_friction_basis_with_preflight(manifold, epsilon);

bool compute_friction_tangents_with_preflight(ContactManifold& manifold, f32 epsilon) {
    if (preflight.reason != FrictionBasisRejectReason::None) {
    compute_friction_tangents_if_needed(manifold, epsilon);

    if (!should_run_friction_basis_rebuild(manifold, epsilon)) {

    if (should_normalize_contact_normal_before_friction(manifold, epsilon)) {
        if (normalLength > 1e-8f) {




    compute_friction_tangents(manifold);

bool ensure_friction_basis_with_preflight(ContactManifold& manifold, f32 epsilon) {



bool rebuild_contact_buffer_friction_bases_with_preflight(ContactBufferSoA& buffer) {
    if (!preflightContactBufferFrictionBuild(buffer).canBuild()) {
    buildContactBufferFrictionTangentBasesWithPreflight(buffer);
bool friction_basis_is_stale_but_rebuildable(const ContactManifold& manifold, f32 epsilon) {
    return friction_basis_is_stale(manifold, epsilon);


bool rebuild_friction_basis_after_normalize_with_preflight(ContactManifold& manifold, f32 epsilon) {
    return rebuild_friction_basis_with_preflight(manifold, epsilon);
const char* contact_normal_normalize_reject_reason_name(ContactNormalNormalizeRejectReason reason) {
    case ContactNormalNormalizeRejectReason::None:
    case ContactNormalNormalizeRejectReason::EmptyManifold:
    case ContactNormalNormalizeRejectReason::InvalidNormal:
    case ContactNormalNormalizeRejectReason::AlreadyUnit:
        return "AlreadyUnit";

ContactNormalNormalizeRejectReason contact_normal_normalize_reject_reason(
    f32 lengthEpsilon) {
        return ContactNormalNormalizeRejectReason::EmptyManifold;
        return ContactNormalNormalizeRejectReason::InvalidNormal;
        return ContactNormalNormalizeRejectReason::AlreadyUnit;
    return ContactNormalNormalizeRejectReason::None;

bool contact_normal_normalize_rejects_for_reason(
    ContactNormalNormalizeRejectReason expected,
    return contact_normal_normalize_reject_reason(manifold, lengthEpsilon) == expected;

ContactNormalNormalizePreflight preflight_contact_normal_normalize(
    ContactNormalNormalizePreflight preflight{};
    preflight.reason = contact_normal_normalize_reject_reason(manifold, lengthEpsilon);
    if (preflight.reason != ContactNormalNormalizeRejectReason::None) {
        preflight.skipped = true;
    preflight.needsNormalize = contact_normal_needs_normalize(manifold, lengthEpsilon);

bool normalize_contact_normal_with_preflight(ContactManifold& manifold, f32 lengthEpsilon) {
    if (!preflight_contact_normal_normalize(manifold, lengthEpsilon).can_normalize()) {

} // namespace fuse::physics::narrowphase
