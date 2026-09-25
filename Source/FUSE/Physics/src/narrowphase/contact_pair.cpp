#include <fuse/physics/narrowphase/contact_pair.hpp>

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/friction.hpp>
#include <fuse/physics/narrowphase/gjk.hpp>

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

ContactManifold flipContactBodies(ContactManifold manifold, u32 bodyA, u32 bodyB) {
    if (!manifold.valid) {
        return invalidContactManifold();
    }
    manifold.contactNormal = manifold.contactNormal * -1.f;
    manifold.bodyA = bodyA;
    manifold.bodyB = bodyB;
    return manifold;
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

ContactManifold collideCapsuleCapsule(vec3 posA, vec3 paramsA, vec3 posB, vec3 paramsB, u32 idxA, u32 idxB) {
    const f32 halfA = std::max(0.f, paramsA.y);
    const f32 halfB = std::max(0.f, paramsB.y);
    const f32 yA0 = posA.y - halfA;
    const f32 yA1 = posA.y + halfA;
    const f32 yB0 = posB.y - halfB;
    const f32 yB1 = posB.y + halfB;
    const f32 overlapLo = std::max(yA0, yB0);
    const f32 overlapHi = std::min(yA1, yB1);

    vec3 pointA{};
    vec3 pointB{};
    if (overlapLo <= overlapHi) {
        const f32 y = 0.5f * (overlapLo + overlapHi);
        pointA = {posA.x, y, posA.z};
        pointB = {posB.x, y, posB.z};
    } else if (yA1 < yB0) {
        pointA = {posA.x, yA1, posA.z};
        pointB = {posB.x, yB0, posB.z};
    } else {
        pointA = {posA.x, yA0, posA.z};
        pointB = {posB.x, yB1, posB.z};
    }

    return collideSphereSphere(pointA, paramsA.x, pointB, paramsB.x, idxA, idxB);
}

ContactManifold collideCapsulePlane(vec3 capsulePos, vec3 capsuleParams, vec3 planeNormal, f32 planeDistance,
                                    u32 idxCapsule, u32 idxPlane) {
    const vec3 normal = planeNormal.normalized();
    const f32 halfHeight = std::max(0.f, capsuleParams.y);
    const vec3 endA = {capsulePos.x, capsulePos.y - halfHeight, capsulePos.z};
    const vec3 endB = {capsulePos.x, capsulePos.y + halfHeight, capsulePos.z};
    const vec3 closest = endA.dot(normal) <= endB.dot(normal) ? endA : endB;
    return collideSpherePlane(closest, capsuleParams.x, normal, planeDistance, idxCapsule, idxPlane);
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
    for (u32 i = 0; i < shapes.count(); ++i) {
        if (shapes.bodyIndices[i] == bodyIndex) {
            return true;
        }
    }
    return false;
}

ContactManifold dispatchShapePair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
        return invalidContactManifold();
    }

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);
    const vec3 posA = bodies.positions[pair.bodyA];
    const vec3 posB = bodies.positions[pair.bodyB];

    if (typeA == CollisionShapeType::Sphere && typeB == CollisionShapeType::Sphere) {
        return collideSphereSphere(
            posA,
            shapes.params[shapeA].x,
            posB,
            shapes.params[shapeB].x,
            pair.bodyA,
            pair.bodyB);
    }

    if (typeA == CollisionShapeType::Sphere && typeB == CollisionShapeType::Plane) {
        return collideSpherePlane(
            posA,
            shapes.params[shapeA].x,
            shapes.params[shapeB],
            shapes.scalars[shapeB],
            pair.bodyA,
            pair.bodyB);
    }

    if (typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::Sphere) {
        return collideSpherePlane(
            posB,
            shapes.params[shapeB].x,
            shapes.params[shapeA],
            shapes.scalars[shapeA],
            pair.bodyB,
            pair.bodyA);
    }

    if (typeA == CollisionShapeType::Sphere && typeB == CollisionShapeType::Box) {
        return collideBoxSphere(
            posA,
            shapes.params[shapeA].x,
            posB,
            shapes.params[shapeB],
            pair.bodyA,
            pair.bodyB);
    }

    if (typeA == CollisionShapeType::Box && typeB == CollisionShapeType::Sphere) {
        const ContactManifold swapped = collideBoxSphere(
            posB,
            shapes.params[shapeB].x,
            posA,
            shapes.params[shapeA],
            pair.bodyB,
            pair.bodyA);
        if (!swapped.valid) {
            return invalidContactManifold();
        }

        ContactManifold manifold = swapped;
        manifold.contactNormal = swapped.contactNormal * -1.f;
        manifold.bodyA = pair.bodyA;
        manifold.bodyB = pair.bodyB;
        return manifold;
    }

    if (typeA == CollisionShapeType::Sphere && typeB == CollisionShapeType::Capsule) {
        return collideCapsuleSphere(
            posA,
            shapes.params[shapeA].x,
            posB,
            shapes.params[shapeB],
            pair.bodyA,
            pair.bodyB);
    }

    if (typeA == CollisionShapeType::Capsule && typeB == CollisionShapeType::Sphere) {
        const ContactManifold swapped = collideCapsuleSphere(
            posB,
            shapes.params[shapeB].x,
            posA,
            shapes.params[shapeA],
            pair.bodyB,
            pair.bodyA);
        if (!swapped.valid) {
            return invalidContactManifold();
        }

        ContactManifold manifold = swapped;
        manifold.contactNormal = swapped.contactNormal * -1.f;
        manifold.bodyA = pair.bodyA;
        manifold.bodyB = pair.bodyB;
        return manifold;
    }

    if (typeA == CollisionShapeType::Box && typeB == CollisionShapeType::Box) {
        return collideBoxBox(
            posA,
            shapes.params[shapeA],
            posB,
            shapes.params[shapeB],
            pair.bodyA,
            pair.bodyB);
    }

    if (typeA == CollisionShapeType::Capsule && typeB == CollisionShapeType::Capsule) {
        return collideCapsuleCapsule(posA, shapes.params[shapeA], posB, shapes.params[shapeB], pair.bodyA,
                                     pair.bodyB);
    }

    if (typeA == CollisionShapeType::Capsule && typeB == CollisionShapeType::Box) {
        return collideCapsuleAgainstBox(posA, shapes.params[shapeA], posB, shapes.params[shapeB], pair.bodyA,
                                        pair.bodyB);
    }

    if (typeA == CollisionShapeType::Box && typeB == CollisionShapeType::Capsule) {
        const ContactManifold swapped = collideCapsuleAgainstBox(
            posB, shapes.params[shapeB], posA, shapes.params[shapeA], pair.bodyB, pair.bodyA);
        return flipContactBodies(swapped, pair.bodyA, pair.bodyB);
    }

    if (typeA == CollisionShapeType::Capsule && typeB == CollisionShapeType::Plane) {
        return collideCapsulePlane(posA, shapes.params[shapeA], shapes.params[shapeB], shapes.scalars[shapeB],
                                   pair.bodyA, pair.bodyB);
    }

    if (typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::Capsule) {
        const ContactManifold swapped =
            collideCapsulePlane(posB, shapes.params[shapeB], shapes.params[shapeA], shapes.scalars[shapeA],
                                pair.bodyB, pair.bodyA);
        return flipContactBodies(swapped, pair.bodyA, pair.bodyB);
    }

    if (typeA == CollisionShapeType::Sphere && typeB == CollisionShapeType::ConvexHull) {
        return collideBoxSphere(posA, shapes.params[shapeA].x, posB, shapes.params[shapeB], pair.bodyA,
                                pair.bodyB);
    }

    if (typeA == CollisionShapeType::ConvexHull && typeB == CollisionShapeType::Sphere) {
        const ContactManifold swapped = collideBoxSphere(
            posB, shapes.params[shapeB].x, posA, shapes.params[shapeA], pair.bodyB, pair.bodyA);
        return flipContactBodies(swapped, pair.bodyA, pair.bodyB);
    }

    if (typeA == CollisionShapeType::Capsule && typeB == CollisionShapeType::ConvexHull) {
        return collideCapsuleAgainstBox(posA, shapes.params[shapeA], posB, shapes.params[shapeB], pair.bodyA,
                                        pair.bodyB);
    }

    if (typeA == CollisionShapeType::ConvexHull && typeB == CollisionShapeType::Capsule) {
        const ContactManifold swapped = collideCapsuleAgainstBox(
            posB, shapes.params[shapeB], posA, shapes.params[shapeA], pair.bodyB, pair.bodyA);
        return flipContactBodies(swapped, pair.bodyA, pair.bodyB);
    }

    if (typeA == CollisionShapeType::ConvexHull && typeB == CollisionShapeType::Plane) {
        return collideHullPlane(posA, shapes.params[shapeA], shapes.params[shapeB], shapes.scalars[shapeB],
                                pair.bodyA, pair.bodyB);
    }

    if (typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::ConvexHull) {
        const ContactManifold swapped =
            collideHullPlane(posB, shapes.params[shapeB], shapes.params[shapeA], shapes.scalars[shapeA],
                             pair.bodyB, pair.bodyA);
        return flipContactBodies(swapped, pair.bodyA, pair.bodyB);
    }

    const bool convexA = typeA == CollisionShapeType::ConvexHull || typeA == CollisionShapeType::Box;
    const bool convexB = typeB == CollisionShapeType::ConvexHull || typeB == CollisionShapeType::Box;
    if (convexA && convexB &&
        (typeA == CollisionShapeType::ConvexHull || typeB == CollisionShapeType::ConvexHull)) {
        const vec3& halfA = shapes.params[shapeA];
        const vec3& halfB = shapes.params[shapeB];
        if (halfA.x <= 0.f || halfA.y <= 0.f || halfA.z <= 0.f || halfB.x <= 0.f || halfB.y <= 0.f ||
            halfB.z <= 0.f) {
            return invalidContactManifold();
        }

        vec3 cornersA[8];
        vec3 cornersB[8];
        writeBoxCorners(posA, shapes.params[shapeA], cornersA);
        writeBoxCorners(posB, shapes.params[shapeB], cornersB);
        return epa(cornersA, 8u, cornersB, 8u, pair.bodyA, pair.bodyB);
    }

    return invalidContactManifold();
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
    if (is_invalid_contact_pair(pair, bodies, shapes)) {
        return invalidContactManifold();
    }
    return dispatchShapePair(pair, bodies, shapes);
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
