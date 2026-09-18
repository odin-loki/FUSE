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
    const f32 speedSq = projected.x * projected.x + projected.y * projected.y;
    return speedSq <= speedThreshold * speedThreshold;
}

bool should_skip_friction_for_manifold(
    const ContactManifold& manifold,
    f32 staticFriction,
    f32 dynamicFriction,
    f32 normalImpulse,
    f32 impulseEpsilon) {
    if (should_skip_friction_tangents(manifold)) {
        return true;
    }
    return should_skip_friction_solve(staticFriction, dynamicFriction, normalImpulse, impulseEpsilon);
}

vec2 combine_body_friction_coefficients(
    const RigidBodySoA& bodies,
    u32 bodyA,
    u32 bodyB) {
    if (bodyA >= bodies.count() || bodyB >= bodies.count()) {
        return {};
    }

    const f32 staticA = bodies.frictionStatic[bodyA];
    const f32 staticB = bodies.frictionStatic[bodyB];
    const f32 dynamicA = bodies.frictionDynamic[bodyA];
    const f32 dynamicB = bodies.frictionDynamic[bodyB];
    return {
        std::sqrt(staticA * staticB),
        std::sqrt(dynamicA * dynamicB),
    };
}

bool should_rebuild_friction_basis(
    vec3 previousNormal,
    vec3 currentNormal,
    f32 angleThresholdRadians) {
    const f32 previousLength = previousNormal.length();
    const f32 currentLength = currentNormal.length();
    if (previousLength <= 1e-8f || currentLength <= 1e-8f) {
        return true;
    }

    const vec3 unitPrevious = previousNormal * (1.f / previousLength);
    const vec3 unitCurrent = currentNormal * (1.f / currentLength);
    const f32 cosine = std::max(-1.f, std::min(1.f, unitPrevious.dot(unitCurrent)));
    return std::acos(cosine) > angleThresholdRadians;
}

} // namespace fuse::physics::narrowphase
