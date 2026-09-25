#include <fuse/physics/narrowphase/contact_pair.hpp>

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/friction.hpp>
#include <fuse/physics/narrowphase/gjk.hpp>
#include <fuse/physics/rotation.hpp>

#include <cmath>

namespace fuse::physics::narrowphase {

namespace {

CollisionShapeType shapeType(const CollisionShapeSoA& shapes, u32 shapeIndex) {
    if (shapeIndex >= shapes.count()) {
        return CollisionShapeType::Sphere;
    }
    return static_cast<CollisionShapeType>(shapes.types[shapeIndex]);
}

u32 findShapeForBody(const CollisionShapeSoA& shapes, u32 bodyIndex, CollisionShapeType preferred) {
    // Common layout: one shape per body, added in body order. Multi-shape bodies (adjacent
    // shapes of the same body) take the scan below so `preferred` still wins.
    if (bodyIndex < shapes.count() && shapes.bodyIndices[bodyIndex] == bodyIndex) {
        const bool soleShape = (bodyIndex == 0u || shapes.bodyIndices[bodyIndex - 1u] != bodyIndex) &&
                               (bodyIndex + 1u >= shapes.count() || shapes.bodyIndices[bodyIndex + 1u] != bodyIndex);
        if (soleShape || static_cast<CollisionShapeType>(shapes.types[bodyIndex]) == preferred) {
            return bodyIndex;
        }
    }
    for (u32 i = 0; i < shapes.count(); ++i) {
        if (shapes.bodyIndices[i] == bodyIndex &&
            static_cast<CollisionShapeType>(shapes.types[i]) == preferred) {
            return i;
        }
    }
    for (u32 i = 0; i < shapes.count(); ++i) {
        if (shapes.bodyIndices[i] == bodyIndex) {
            return i;
        }
    }
    return shapes.count();
}

ContactManifold collideCapsuleAgainstBox(vec3 capsulePos, vec3 capsuleParams, vec3 boxPos, vec3 boxHalfExtents,
                                        u32 idxCapsule, u32 idxBox) {
    const f32 radius = capsuleParams.x;
    const f32 halfHeight = std::max(0.f, capsuleParams.y);
    const vec3 samples[3] = {
        capsulePos,
        {capsulePos.x, capsulePos.y + halfHeight, capsulePos.z},
        {capsulePos.x, capsulePos.y - halfHeight, capsulePos.z},
    };

    ContactManifold best{};
    for (const vec3& sample : samples) {
        const ContactManifold hit =
            collideBoxSphere(sample, radius, boxPos, boxHalfExtents, idxCapsule, idxBox);
        if (hit.valid && (!best.valid || hit.penetrationDepth > best.penetrationDepth)) {
            best = hit;
        }
    }
    return best;
}

ContactManifold collideHullPlane(vec3 hullPos, vec3 halfExtents, vec3 planeNormal, f32 planeDistance, u32 idxHull,
                                 u32 idxPlane) {
    const vec3 normal = planeNormal.normalized();
    const f32 extent = std::fabs(normal.x) * halfExtents.x + std::fabs(normal.y) * halfExtents.y +
                       std::fabs(normal.z) * halfExtents.z;
    const vec3 closest = hullPos - normal * extent;
    return collideSpherePlane(closest, 0.f, normal, planeDistance, idxHull, idxPlane);
}

void writeBoxCorners(vec3 center, vec3 halfExtents, vec3 out[8]) {
    u32 index = 0;
    for (f32 x = -1.f; x <= 1.f; x += 2.f) {
        for (f32 y = -1.f; y <= 1.f; y += 2.f) {
            for (f32 z = -1.f; z <= 1.f; z += 2.f) {
                out[index++] = {center.x + halfExtents.x * x, center.y + halfExtents.y * y,
                                center.z + halfExtents.z * z};
            }
        }
    }
}

bool hasShapeForBody(const CollisionShapeSoA& shapes, u32 bodyIndex) {
    if (bodyIndex < shapes.count() && shapes.bodyIndices[bodyIndex] == bodyIndex) {
        return true;
    }
    for (u32 i = 0; i < shapes.count(); ++i) {
        if (shapes.bodyIndices[i] == bodyIndex) {
            return true;
        }
    }
    return false;
}

/// Re-labels a manifold computed with the bodies swapped (normal flipped to point B -> A).
ContactManifold flipped(ContactManifold manifold, u32 bodyA, u32 bodyB) {
    if (!manifold.valid) {
        return invalidContactManifold();
    }
    manifold.contactNormal = manifold.contactNormal * -1.f;
    manifold.bodyA = bodyA;
    manifold.bodyB = bodyB;
    return manifold;
}

quat bodyOrientation(const RigidBodySoA& bodies, u32 body) {
    return body < bodies.orientations.size() ? bodies.orientations[body] : quat{};
}

ContactManifold dispatchShapePairRaw(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    f32 margin) {
    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
        return invalidContactManifold();
    }
    const ShapeInstance a{shapeType(shapes, shapeA), shapes.params[shapeA], shapes.scalars[shapeA],
                          bodies.positions[pair.bodyA], bodyOrientation(bodies, pair.bodyA)};
    const ShapeInstance b{shapeType(shapes, shapeB), shapes.params[shapeB], shapes.scalars[shapeB],
                          bodies.positions[pair.bodyB], bodyOrientation(bodies, pair.bodyB)};
    return collideShapes(a, b, pair.bodyA, pair.bodyB, margin);
}

} // namespace

ContactManifold collideShapes(const ShapeInstance& shapeA, const ShapeInstance& shapeB, u32 a, u32 b, f32 margin) {
    const CollisionShapeType typeA = shapeA.type;
    const CollisionShapeType typeB = shapeB.type;
    const vec3 posA = shapeA.position;
    const vec3 posB = shapeB.position;
    const quat rotA = shapeA.orientation;
    const quat rotB = shapeB.orientation;
    const vec3 paramsA = shapeA.params;
    const vec3 paramsB = shapeB.params;

    using T = CollisionShapeType;
    const auto is = [&](T first, T second) { return typeA == first && typeB == second; };

    if (is(T::Sphere, T::Sphere)) {
        return collideSphereSphere(posA, paramsA.x, posB, paramsB.x, a, b, margin);
    }
    if (is(T::Sphere, T::Plane)) {
        return collideSpherePlane(posA, paramsA.x, paramsB, shapeB.scalar, a, b, margin);
    }
    if (is(T::Plane, T::Sphere)) {
        return collideSpherePlane(posB, paramsB.x, paramsA, shapeA.scalar, b, a, margin);
    }
    if (is(T::Box, T::Plane)) {
        return collideOrientedBoxPlane(posA, rotA, paramsA, paramsB, shapeB.scalar, a, b, margin);
    }
    if (is(T::Plane, T::Box)) {
        return collideOrientedBoxPlane(posB, rotB, paramsB, paramsA, shapeA.scalar, b, a, margin);
    }
    if (is(T::Sphere, T::Box)) {
        return collideOrientedBoxSphere(posA, paramsA.x, posB, rotB, paramsB, a, b, margin);
    }
    if (is(T::Box, T::Sphere)) {
        return flipped(collideOrientedBoxSphere(posB, paramsB.x, posA, rotA, paramsA, b, a, margin), a, b);
    }
    if (is(T::Box, T::Box)) {
        if (isIdentity(rotA) && isIdentity(rotB)) {
            return collideBoxBox(posA, paramsA, posB, paramsB, a, b, margin);
        }
        return collideOrientedBoxBox(posA, rotA, paramsA, posB, rotB, paramsB, a, b, margin);
    }

    // Capsules: segment along the body's local Y axis.
    if (is(T::Sphere, T::Capsule)) {
        if (isIdentity(rotB)) {
            return collideCapsuleSphere(posA, paramsA.x, posB, paramsB, a, b, margin);
        }
        const vec3 half = capsuleHalfAxis(rotB, paramsB.y);
        return collideCapsuleSegments(posA, posA, paramsA.x, posA, posB - half, posB + half, paramsB.x, posB, a, b,
                                      margin);
    }
    if (is(T::Capsule, T::Sphere)) {
        if (isIdentity(rotA)) {
            return flipped(collideCapsuleSphere(posB, paramsB.x, posA, paramsA, b, a, margin), a, b);
        }
        const vec3 half = capsuleHalfAxis(rotA, paramsA.y);
        return collideCapsuleSegments(posA - half, posA + half, paramsA.x, posA, posB, posB, paramsB.x, posB, a, b,
                                      margin);
    }
    if (is(T::Capsule, T::Capsule)) {
        const vec3 halfA = capsuleHalfAxis(rotA, paramsA.y);
        const vec3 halfB = capsuleHalfAxis(rotB, paramsB.y);
        return collideCapsuleSegments(posA - halfA, posA + halfA, paramsA.x, posA, posB - halfB, posB + halfB,
                                      paramsB.x, posB, a, b, margin);
    }
    if (is(T::Capsule, T::Plane)) {
        return collideCapsulePlane(posA, rotA, paramsA, paramsB, shapeB.scalar, a, b, margin);
    }
    if (is(T::Plane, T::Capsule)) {
        return flipped(collideCapsulePlane(posB, rotB, paramsB, paramsA, shapeA.scalar, b, a, margin), a, b);
    }
    if (is(T::Capsule, T::Box)) {
        return collideCapsuleBox(posA, rotA, paramsA, posB, rotB, paramsB, a, b, margin);
    }
    if (is(T::Box, T::Capsule)) {
        return flipped(collideCapsuleBox(posB, rotB, paramsB, posA, rotA, paramsA, b, a, margin), a, b);
    }

    if (is(T::Sphere, T::ConvexHull)) {
        return collideBoxSphere(posA, paramsA.x, posB, paramsB, a, b, margin);
    }
    if (is(T::ConvexHull, T::Sphere)) {
        return flipped(collideBoxSphere(posB, paramsB.x, posA, paramsA, b, a, margin), a, b);
    }
    if (is(T::Capsule, T::ConvexHull)) {
        return collideCapsuleAgainstBox(posA, paramsA, posB, paramsB, a, b);
    }
    if (is(T::ConvexHull, T::Capsule)) {
        return flipped(collideCapsuleAgainstBox(posB, paramsB, posA, paramsA, b, a), a, b);
    }
    if (is(T::ConvexHull, T::Plane)) {
        return collideHullPlane(posA, paramsA, paramsB, shapeB.scalar, a, b);
    }
    if (is(T::Plane, T::ConvexHull)) {
        return flipped(collideHullPlane(posB, paramsB, paramsA, shapeA.scalar, b, a), a, b);
    }

    const bool convexA = typeA == T::ConvexHull || typeA == T::Box;
    const bool convexB = typeB == T::ConvexHull || typeB == T::Box;
    if (convexA && convexB && (typeA == T::ConvexHull || typeB == T::ConvexHull)) {
        if (paramsA.x <= 0.f || paramsA.y <= 0.f || paramsA.z <= 0.f || paramsB.x <= 0.f || paramsB.y <= 0.f ||
            paramsB.z <= 0.f) {
            return invalidContactManifold();
        }

        vec3 cornersA[8];
        vec3 cornersB[8];
        writeBoxCorners(posA, paramsA, cornersA);
        writeBoxCorners(posB, paramsB, cornersB);
        return epa(cornersA, 8u, cornersB, 8u, a, b);
    }

    return invalidContactManifold();
}

namespace {

/// minSeparation is stated against the body centres, (pA - pB) . n >= minSeparation, so its
/// violation equals the deepest penetration whatever the shapes. The solver constrains the
/// individual contact points (with rotational terms); this centre form remains a diagnostic.
ContactManifold dispatchShapePair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    f32 margin) {
    ContactManifold manifold = dispatchShapePairRaw(pair, bodies, shapes, margin);
    if (manifold.valid) {
        const vec3 centres = bodies.positions[manifold.bodyA] - bodies.positions[manifold.bodyB];
        manifold.minSeparation = centres.dot(manifold.contactNormal) + manifold.maxPenetration();
    }
    return manifold;
}

bool isShapeDegenerate(CollisionShapeType type, const vec3& params) {
    switch (type) {
    case CollisionShapeType::Sphere:
    case CollisionShapeType::Capsule:
        return params.x <= 0.f;
    case CollisionShapeType::Box:
    case CollisionShapeType::ConvexHull:
        return params.x <= 0.f || params.y <= 0.f || params.z <= 0.f;
    case CollisionShapeType::Plane:
        return params.length() < 1e-8f;
    default:
        return false;
    }
}

bool isDeepenDegenerateShapePair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes) {
    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
        return false;
    }

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);
    if (isShapeDegenerate(typeA, shapes.params[shapeA]) ||
        isShapeDegenerate(typeB, shapes.params[shapeB])) {
        return true;
    }

    return (typeA == CollisionShapeType::Capsule && shapes.params[shapeA].y <= 0.f) ||
           (typeB == CollisionShapeType::Capsule && shapes.params[shapeB].y <= 0.f);
}

} // namespace

bool is_self_contact_pair(const broadphase::CandidatePair& pair) {
    return pair.bodyA == pair.bodyB;
}

bool is_out_of_range_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies) {
    return pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count();
}

bool is_missing_shape_contact_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes) {
    return !hasShapeForBody(shapes, pair.bodyA) || !hasShapeForBody(shapes, pair.bodyB);
}

const char* contact_pair_reject_reason_name(ContactPairRejectReason reason) {
    switch (reason) {
    case ContactPairRejectReason::None:
        return "None";
    case ContactPairRejectReason::SelfPair:
        return "SelfPair";
    case ContactPairRejectReason::OutOfRangeBody:
        return "OutOfRangeBody";
    case ContactPairRejectReason::MissingShape:
        return "MissingShape";
    case ContactPairRejectReason::BothTriggers:
        return "BothTriggers";
    case ContactPairRejectReason::UnsupportedShapePair:
        return "UnsupportedShapePair";
    case ContactPairRejectReason::BothStatic:
        return "BothStatic";
    case ContactPairRejectReason::DegenerateShape:
        return "DegenerateShape";
    case ContactPairRejectReason::BothSleeping:
        return "BothSleeping";
    case ContactPairRejectReason::BothKinematic:
        return "BothKinematic";
    case ContactPairRejectReason::AnyTrigger:
        return "AnyTrigger";
    case ContactPairRejectReason::BothMassless:
        return "BothMassless";
    }
    return "Unknown";
}

bool is_trigger_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies) {
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count()) {
        return false;
    }
    const bool triggerA = (bodies.flags[pair.bodyA] & RB_TRIGGER) != 0u;
    const bool triggerB = (bodies.flags[pair.bodyB] & RB_TRIGGER) != 0u;
    return triggerA && triggerB;
}

bool is_static_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies) {
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count()) {
        return false;
    }
    const bool staticA = (bodies.flags[pair.bodyA] & RB_STATIC) != 0u;
    const bool staticB = (bodies.flags[pair.bodyB] & RB_STATIC) != 0u;
    return staticA && staticB;
}

bool is_sleeping_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies) {
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count()) {
        return false;
    }
    const bool sleepingA = (bodies.flags[pair.bodyA] & RB_SLEEPING) != 0u;
    const bool sleepingB = (bodies.flags[pair.bodyB] & RB_SLEEPING) != 0u;
    return sleepingA && sleepingB;
}

bool is_kinematic_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies) {
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count()) {
        return false;
    }
    const bool kinematicA = (bodies.flags[pair.bodyA] & RB_KINEMATIC) != 0u;
    const bool kinematicB = (bodies.flags[pair.bodyB] & RB_KINEMATIC) != 0u;
    return kinematicA && kinematicB;
}

bool is_any_trigger_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies) {
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count()) {
        return false;
    }
    const bool triggerA = (bodies.flags[pair.bodyA] & RB_TRIGGER) != 0u;
    const bool triggerB = (bodies.flags[pair.bodyB] & RB_TRIGGER) != 0u;
    return triggerA || triggerB;
}

bool is_massless_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    f32 invMassEpsilon) {
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count()) {
        return false;
    }
    return bodies.invMasses[pair.bodyA] <= invMassEpsilon &&
           bodies.invMasses[pair.bodyB] <= invMassEpsilon;
}

bool is_degenerate_shape_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes) {
    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
        return false;
    }

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);
    return isShapeDegenerate(typeA, shapes.params[shapeA]) ||
           isShapeDegenerate(typeB, shapes.params[shapeB]);
}

bool is_unsupported_shape_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes) {
    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
        return true;
    }

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);

    const auto isSupported = [](CollisionShapeType type) {
        switch (type) {
        case CollisionShapeType::Sphere:
        case CollisionShapeType::Box:
        case CollisionShapeType::Capsule:
        case CollisionShapeType::Plane:
            return true;
        default:
            return false;
        }
    };

    const auto otherOfHull = [](CollisionShapeType typeA, CollisionShapeType typeB) -> CollisionShapeType {
        if (typeA == CollisionShapeType::ConvexHull) {
            return typeB;
        }
        if (typeB == CollisionShapeType::ConvexHull) {
            return typeA;
        }
        return typeA;
    };
    if (typeA == CollisionShapeType::ConvexHull || typeB == CollisionShapeType::ConvexHull) {
        switch (otherOfHull(typeA, typeB)) {
        case CollisionShapeType::Sphere:
        case CollisionShapeType::Box:
        case CollisionShapeType::Capsule:
        case CollisionShapeType::Plane:
        case CollisionShapeType::ConvexHull:
            return false;
        default:
            break;
        }
    }

    if (!isSupported(typeA) || !isSupported(typeB)) {
        return true;
    }

    if (typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::Plane) {
        return true;
    }

    return false;
}

ContactPairRejectReason contact_pair_reject_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (pair.bodyA == pair.bodyB) {
        return ContactPairRejectReason::SelfPair;
    }
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count()) {
        return ContactPairRejectReason::OutOfRangeBody;
    }
    if (!hasShapeForBody(shapes, pair.bodyA) || !hasShapeForBody(shapes, pair.bodyB)) {
        return ContactPairRejectReason::MissingShape;
    }
    if (is_trigger_contact_pair(pair, bodies)) {
        return ContactPairRejectReason::BothTriggers;
    }
    if (is_unsupported_shape_pair(pair, shapes)) {
        return ContactPairRejectReason::UnsupportedShapePair;
    }
    if (is_static_contact_pair(pair, bodies)) {
        return ContactPairRejectReason::BothStatic;
    }
    if (is_degenerate_shape_pair(pair, shapes)) {
        return ContactPairRejectReason::DegenerateShape;
    }
    return ContactPairRejectReason::None;
}

bool contact_pair_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_reject_reason(pair, bodies, shapes) == expected;
}

bool is_invalid_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return contact_pair_reject_reason(pair, bodies, shapes) != ContactPairRejectReason::None;
}

bool is_valid_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !is_invalid_contact_pair(pair, bodies, shapes);
}

ContactManifold detect_contacts_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return detect_contacts_pair(pair, bodies, shapes, 0.f);
}

ContactManifold detect_contacts_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    f32 margin) {
    if (is_invalid_contact_pair(pair, bodies, shapes)) {
        return invalidContactManifold();
    }
    return dispatchShapePair(pair, bodies, shapes, margin);
}

bool can_finalize_contact_manifold(const ContactManifold& manifold) {
    if (manifold.empty()) {
        return false;
    }
    if (!manifold.hasValidNormal()) {
        return false;
    }
    return manifold.hasPenetratingPoints();
}

bool generate_contact_manifold(ContactManifold& manifold) {
    if (manifold.empty()) {
        manifold.clear();
        return false;
    }

    manifold.pruneContactPoints();
    if (manifold.empty()) {
        manifold.clear();
        return false;
    }

    if (!manifold.hasValidNormal()) {
        manifold.clear();
        return false;
    }

    const f32 normalLength = manifold.contactNormal.length();
    manifold.contactNormal = manifold.contactNormal * (1.f / normalLength);

    manifold.syncLegacyFields();
    compute_friction_tangents(manifold);
    if (!manifold.hasFrictionBasis()) {
        manifold.clear();
        return false;
    }

    manifold.valid = true;
    return true;
}

bool generate_contact_manifold_if_needed(ContactManifold& manifold) {
    if (!can_finalize_contact_manifold(manifold)) {
        return false;
    }
    return generate_contact_manifold(manifold);
}

void compute_friction_tangents(ContactManifold& manifold) {
    if (should_skip_friction_tangents(manifold)) {
        manifold.frictionBasis = {};
        return;
    }

    rebuild_friction_basis_with_preflight(manifold);
}

ContactPairPreflight preflight_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    ContactPairPreflight preflight{};
    preflight.reason = contact_pair_reject_reason(pair, bodies, shapes);
    preflight.rejected = preflight.reason != ContactPairRejectReason::None;
    return preflight;
}

bool should_skip_contact_pair_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return is_invalid_contact_pair(pair, bodies, shapes);
}

ContactPairRejectReason contact_pair_deepen_reject_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    const ContactPairRejectReason baseReason = contact_pair_reject_reason(pair, bodies, shapes);
    if (baseReason != ContactPairRejectReason::None) {
        return baseReason;
    }
    if (isDeepenDegenerateShapePair(pair, shapes)) {
        return ContactPairRejectReason::DegenerateShape;
    }
    if (is_sleeping_contact_pair(pair, bodies)) {
        return ContactPairRejectReason::BothSleeping;
    }
    if (is_kinematic_contact_pair(pair, bodies)) {
        return ContactPairRejectReason::BothKinematic;
    }
    if (is_any_trigger_contact_pair(pair, bodies)) {
        return ContactPairRejectReason::AnyTrigger;
    }
    if (is_massless_contact_pair(pair, bodies)) {
        return ContactPairRejectReason::BothMassless;
    }
    return ContactPairRejectReason::None;
}

ContactPairDeepenPreflight preflight_contact_pair_deepen(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    ContactPairDeepenPreflight preflight{};
    preflight.reason = contact_pair_deepen_reject_reason(pair, bodies, shapes);
    preflight.rejected = preflight.reason != ContactPairRejectReason::None;
    return preflight;
}

bool should_skip_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) != ContactPairRejectReason::None;
}

bool can_skip_narrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !has_dispatchable_contact_pair(pairs, bodies, shapes);
}

u32 count_dispatchable_contact_pairs(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    u32 dispatchable = 0u;
    for (const broadphase::CandidatePair& pair : pairs) {
        if (!should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {
            ++dispatchable;
        }
    }
    return dispatchable;
}

bool has_dispatchable_contact_pair(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (pairs.empty()) {
        return false;
    }
    return count_dispatchable_contact_pairs(pairs, bodies, shapes) > 0u;
}

bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;
}

bool is_plane_plane_contact_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes) {
    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
        return false;
    }

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);
    return typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::Plane;
}

NarrowphaseBatchPreflight preflight_narrowphase_batch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    NarrowphaseBatchPreflight preflight{};
    preflight.pairCount = static_cast<u32>(pairs.size());
    preflight.dispatchableCount = count_dispatchable_contact_pairs(pairs, bodies, shapes);
    preflight.rejectedCount = preflight.pairCount - preflight.dispatchableCount;
    return preflight;
}

bool narrowphase_batch_rejects_all(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return preflight_narrowphase_batch(pairs, bodies, shapes).can_skip();
}

bool should_run_contact_pair_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_dispatch(pair, bodies, shapes);
}

bool should_run_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

bool should_run_narrowphase_batch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !narrowphase_batch_rejects_all(pairs, bodies, shapes);
}

} // namespace fuse::physics::narrowphase
