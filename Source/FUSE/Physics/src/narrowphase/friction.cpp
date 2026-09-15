#include <fuse/physics/narrowphase/friction.hpp>

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

} // namespace fuse::physics::narrowphase
