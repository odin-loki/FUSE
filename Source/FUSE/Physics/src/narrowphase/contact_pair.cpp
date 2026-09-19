#include <fuse/physics/narrowphase/contact_pair.hpp>

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/friction.hpp>

#include <cmath>
#include <vector>

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

    return invalidContactManifold();
}

bool isShapeDegenerate(CollisionShapeType type, const vec3& params) {
    switch (type) {
    case CollisionShapeType::Sphere:
    case CollisionShapeType::Capsule:
        return params.x <= 0.f;
    case CollisionShapeType::Box:
        return params.x <= 0.f || params.y <= 0.f || params.z <= 0.f;
    case CollisionShapeType::Plane:
        return params.length() < 1e-8f;
    default:
        return false;
    }
}

bool isShapeNearDegenerate(CollisionShapeType type, const vec3& params, f32 extentEpsilon) {
    switch (type) {
    case CollisionShapeType::Sphere:
    case CollisionShapeType::Capsule:
        return params.x > 0.f && params.x < extentEpsilon;
    case CollisionShapeType::Box:
        return (params.x > 0.f && params.x < extentEpsilon) ||
               (params.y > 0.f && params.y < extentEpsilon) ||
               (params.z > 0.f && params.z < extentEpsilon);
    default:
        return false;
    }
}

bool isPlaneNormalUnnormalized(const vec3& planeNormal, f32 lengthEpsilon) {
    const f32 length = planeNormal.length();
    if (length < 1e-8f) {
        return false;
    }
    return std::fabs(length - 1.f) > lengthEpsilon;
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
    case ContactPairRejectReason::BothSleeping:
        return "BothSleeping";
    case ContactPairRejectReason::BothKinematic:
        return "BothKinematic";
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
    case ContactPairRejectReason::BothZeroInvMass:
        return "BothZeroInvMass";
    case ContactPairRejectReason::NoColliderDispatch:
        return "NoColliderDispatch";
    case ContactPairRejectReason::ZeroInvMass:
        return "ZeroInvMass";
    case ContactPairRejectReason::NegativeInverseMass:
        return "NegativeInverseMass";
    case ContactPairRejectReason::BothZeroMass:
        return "BothZeroMass";
    case ContactPairRejectReason::SleepingKinematicMix:
        return "SleepingKinematicMix";
    case ContactPairRejectReason::PlanePlane:
        return "PlanePlane";
    case ContactPairRejectReason::InvalidPlaneNormal:
        return "InvalidPlaneNormal";
    case ContactPairRejectReason::ShapeBodyMismatch:
        return "ShapeBodyMismatch";
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
bool is_static_static_pair(
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
bool is_both_sleeping_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies) {
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count()) {
        return false;
    }
    const bool sleepingA = (bodies.flags[pair.bodyA] & RB_SLEEPING) != 0u;
    const bool sleepingB = (bodies.flags[pair.bodyB] & RB_SLEEPING) != 0u;
    return sleepingA && sleepingB;

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

bool is_sleeping_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies) {
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count()) {
        return false;
    }
    const bool sleepingA = (bodies.flags[pair.bodyA] & RB_SLEEPING) != 0u;
    const bool sleepingB = (bodies.flags[pair.bodyB] & RB_SLEEPING) != 0u;
    return sleepingA && sleepingB;

bool is_degenerate_shape_pair(
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

bool is_any_trigger_contact_pair(
    const bool triggerA = (bodies.flags[pair.bodyA] & RB_TRIGGER) != 0u;
    const bool triggerB = (bodies.flags[pair.bodyB] & RB_TRIGGER) != 0u;
    return triggerA || triggerB;

bool is_massless_contact_pair(
    const RigidBodySoA& bodies,
    f32 invMassEpsilon) {
    return bodies.invMasses[pair.bodyA] <= invMassEpsilon &&
           bodies.invMasses[pair.bodyB] <= invMassEpsilon;

bool is_zero_inv_mass_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    f32 invMassEpsilon) {
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count()) {
        return false;
    }
    return bodies.invMasses[pair.bodyA] <= invMassEpsilon &&
           bodies.invMasses[pair.bodyB] <= invMassEpsilon;
}

bool is_negative_inverse_mass_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies) {
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count()) {
        return false;
    }
    return bodies.invMasses[pair.bodyA] < 0.f || bodies.invMasses[pair.bodyB] < 0.f;
}

bool is_zero_mass_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies) {
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count()) {
        return false;
    }
    return bodies.invMasses[pair.bodyA] <= 0.f && bodies.invMasses[pair.bodyB] <= 0.f;
}

bool is_sleeping_kinematic_mix_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies) {
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count()) {
        return false;
    }
    const bool sleepingA = (bodies.flags[pair.bodyA] & RB_SLEEPING) != 0u;
    const bool sleepingB = (bodies.flags[pair.bodyB] & RB_SLEEPING) != 0u;
    const bool kinematicA = (bodies.flags[pair.bodyA] & RB_KINEMATIC) != 0u;
    const bool kinematicB = (bodies.flags[pair.bodyB] & RB_KINEMATIC) != 0u;
    return (sleepingA && kinematicB) || (kinematicA && sleepingB);
}

bool is_degenerate_shape_pair(
    const CollisionShapeSoA& shapes) {
    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);
    return isShapeDegenerate(typeA, shapes.params[shapeA]) ||
           isShapeDegenerate(typeB, shapes.params[shapeB]);

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

bool has_contact_pair_dispatch_path(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes) {
    if (is_missing_shape_contact_pair(pair, shapes)) {
        return false;
    }
    if (is_unsupported_shape_pair(pair, shapes)) {
        return false;
    }
    if (is_degenerate_shape_pair(pair, shapes)) {
        return false;
    }
    return true;
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

    if (!isSupported(typeA) || !isSupported(typeB)) {
        return true;
    }

    if (typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::Plane) {
        return true;
    }

    return false;
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
    default:
        return "Unknown";
    }
}

bool is_empty_contact_manifold(const ContactManifold& manifold) {
    return manifold.empty();
}

bool is_valid_contact_manifold(const ContactManifold& manifold) {
    return manifold.valid && !manifold.empty() && manifold.hasValidNormal();
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
    if (is_static_static_pair(pair, bodies)) {
        return ContactPairRejectReason::BothStatic;
    }
    if (is_both_sleeping_pair(pair, bodies)) {
        return ContactPairRejectReason::BothSleeping;
    }
    if (is_unsupported_shape_pair(pair, shapes)) {
        return ContactPairRejectReason::UnsupportedShapePair;
    }
    if (is_static_contact_pair(pair, bodies)) {
        return ContactPairRejectReason::BothStatic;
    }
    if (is_sleeping_contact_pair(pair, bodies)) {
        return ContactPairRejectReason::BothSleeping;
    }
    if (is_kinematic_contact_pair(pair, bodies)) {
        return ContactPairRejectReason::BothKinematic;
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
bool contact_pair_should_dispatch(
    const CollisionShapeSoA& shapes) {
    return contact_pair_reject_reason(pair, bodies, shapes) == ContactPairRejectReason::None;
}

const char* contact_pair_reject_reason_label(ContactPairRejectReason reason) {
    switch (reason) {
    case ContactPairRejectReason::None:
        return "none";
    case ContactPairRejectReason::SelfPair:
        return "self_pair";
    case ContactPairRejectReason::OutOfRangeBody:
        return "out_of_range_body";
    case ContactPairRejectReason::MissingShape:
        return "missing_shape";
    case ContactPairRejectReason::BothTriggers:
        return "both_triggers";
    case ContactPairRejectReason::BothStatic:
        return "both_static";
    case ContactPairRejectReason::BothSleeping:
        return "both_sleeping";
    case ContactPairRejectReason::UnsupportedShapePair:
        return "unsupported_shape_pair";
    default:
        return "unknown";
}

bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;
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

bool should_skip_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return is_invalid_contact_pair(pair, bodies, shapes);
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
    const ContactManifoldFinalizePreflight preflight = preflight_contact_manifold_finalize(manifold);
    if (!preflight.can_finalize()) {
        manifold.clear();
        return false;
    }

    manifold.pruneAndRetainPenetrating();
    if (should_run_manifold_prune(manifold)) {
        manifold.pruneContactPoints();
    }
    if (manifold.empty()) {
    if (!manifold.pruneContactPointsIfNeeded()) {
    const ManifoldFinalizePreflight finalizePreflight = preflight_manifold_finalize(manifold);
    if (finalizePreflight.wouldBeEmptyAfterPrune) {
        manifold.clear();
        return false;
    }

        manifold.clear();
        return false;
    }


    if (!manifold.hasValidNormal()) {

    const f32 normalLength = manifold.contactNormal.length();
    manifold.contactNormal = manifold.contactNormal * (1.f / normalLength);

    manifold.syncLegacyFields();
    compute_friction_tangents_if_needed(manifold);
    if (!manifold.hasFrictionBasis()) {

    manifold.valid = true;
    return true;
    return generate_contact_manifold_guarded(manifold).finalized;
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
    if (should_skip_friction_basis_preflight(manifold)) {
        return;
    }

    const f32 normalLength = manifold.contactNormal.length();
    if (std::fabs(normalLength - 1.f) > 1e-4f) {
        manifold.contactNormal = manifold.contactNormal * (1.f / normalLength);
    manifold.buildFrictionBasis();

ContactPairPreflight preflight_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    ContactPairPreflight preflight{};
    preflight.isSelfPair = is_self_contact_pair(pair);
    preflight.isOutOfRange = is_out_of_range_contact_pair(pair, bodies);
    preflight.isMissingShape = is_missing_shape_contact_pair(pair, shapes);
    preflight.isBothTriggers = is_trigger_contact_pair(pair, bodies);
    preflight.isUnsupportedShape = is_unsupported_shape_pair(pair, shapes);
    preflight.isBothStatic = is_static_contact_pair(pair, bodies);
    preflight.isDegenerateShape = is_degenerate_shape_pair(pair, shapes);
    preflight.reason = contact_pair_reject_reason(pair, bodies, shapes);
    preflight.rejected = preflight.reason != ContactPairRejectReason::None;
    return preflight;

bool should_skip_contact_pair_dispatch(
    return is_invalid_contact_pair(pair, bodies, shapes);

bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;
}

bool should_run_contact_pair_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_dispatch(pair, bodies, shapes);
}

bool should_run_contact_pair_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_dispatch(pair, bodies, shapes);
}

ContactPairRejectReason contact_pair_deepen_reject_reason(
    const ContactPairRejectReason baseReason = contact_pair_reject_reason(pair, bodies, shapes);
    if (baseReason != ContactPairRejectReason::None) {
        return baseReason;
    if (isDeepenDegenerateShapePair(pair, shapes)) {
        return ContactPairRejectReason::DegenerateShape;
    if (is_sleeping_contact_pair(pair, bodies)) {
        return ContactPairRejectReason::BothSleeping;
    if (is_kinematic_contact_pair(pair, bodies)) {
        return ContactPairRejectReason::BothKinematic;
    if (is_any_trigger_contact_pair(pair, bodies)) {
        return ContactPairRejectReason::AnyTrigger;
    if (is_massless_contact_pair(pair, bodies)) {
        return ContactPairRejectReason::BothMassless;
    return ContactPairRejectReason::None;

bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;
}

ContactPairDeepenPreflight preflight_contact_pair_deepen(
    ContactPairDeepenPreflight preflight{};
    preflight.reason = contact_pair_deepen_reject_reason(pair, bodies, shapes);
    preflight.rejected = preflight.reason != ContactPairRejectReason::None;
    preflight.isSleeping = preflight.reason == ContactPairRejectReason::BothSleeping;
    preflight.isKinematic = preflight.reason == ContactPairRejectReason::BothKinematic;
    preflight.isAnyTrigger = preflight.reason == ContactPairRejectReason::AnyTrigger;
    preflight.isMassless = preflight.reason == ContactPairRejectReason::BothMassless;
    preflight.isDeepenDegenerate = preflight.reason == ContactPairRejectReason::DegenerateShape &&
                                   contact_pair_reject_reason(pair, bodies, shapes) ==
                                       ContactPairRejectReason::None;
    preflight.bothSleeping = preflight.reason == ContactPairRejectReason::BothSleeping;
    preflight.bothKinematic = preflight.reason == ContactPairRejectReason::BothKinematic;
    preflight.anyTrigger = preflight.reason == ContactPairRejectReason::AnyTrigger;
    preflight.bothMassless = preflight.reason == ContactPairRejectReason::BothMassless;
    preflight.degenerateShape = preflight.reason == ContactPairRejectReason::DegenerateShape;
    return preflight;
}

bool should_skip_contact_pair_deepen_dispatch(
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) != ContactPairRejectReason::None;

bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;
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

bool should_run_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

bool should_run_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;
}

bool can_skip_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

bool should_run_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !can_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;
}

bool should_run_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

bool can_skip_narrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,
    return !has_dispatchable_contact_pair(pairs, bodies, shapes);

u32 count_dispatchable_contact_pairs(
    u32 dispatchable = 0u;
    for (const broadphase::CandidatePair& pair : pairs) {
        if (!should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {
            ++dispatchable;
    return dispatchable;

bool has_dispatchable_contact_pair(
    if (pairs.empty()) {
        return false;
    return count_dispatchable_contact_pairs(pairs, bodies, shapes) > 0u;

bool contact_pair_deepen_rejects_for_reason(
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;

bool is_plane_plane_contact_pair(
    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);
    return typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::Plane;

NarrowphaseBatchPreflight preflight_narrowphase_batch(
    NarrowphaseBatchPreflight preflight{};
    preflight.pairCount = static_cast<u32>(pairs.size());
    preflight.dispatchableCount = count_dispatchable_contact_pairs(pairs, bodies, shapes);
    preflight.rejectedCount = preflight.pairCount - preflight.dispatchableCount;

bool narrowphase_batch_rejects_all(
    return preflight_narrowphase_batch(pairs, bodies, shapes).can_skip();

bool should_run_contact_pair_dispatch(
    return !should_skip_contact_pair_dispatch(pair, bodies, shapes);

bool should_run_contact_pair_deepen_dispatch(
    return !should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);

bool should_run_narrowphase_batch(
    return !narrowphase_batch_rejects_all(pairs, bodies, shapes);
    ensureFrictionBasis(manifold);
    if (has_cached_friction_basis(manifold) && !friction_basis_is_stale(manifold)) {
        return;
    }

    const f32 normalLength = manifold.contactNormal.length();
    if (std::fabs(normalLength - 1.f) > 1e-4f) {
        manifold.contactNormal = manifold.contactNormal * (1.f / normalLength);
    invalidate_friction_basis(manifold);
    manifold.buildFrictionBasis();
    if (!needs_friction_basis_rebuild(manifold)) {

    (void)ensure_friction_basis(manifold);
}

bool can_dispatch_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_dispatch(pair, bodies, shapes);
}

ContactManifold detect_contacts_pair_if_valid(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (should_skip_contact_pair_dispatch(pair, bodies, shapes)) {
        return invalidContactManifold();
    return dispatchShapePair(pair, bodies, shapes);

ContactPairDispatchResult dispatch_contact_pair_if_valid(
    ContactPairDispatchResult result{};
    result.preflight = preflight_contact_pair(pair, bodies, shapes);
    if (result.preflight.rejected) {
        return result;

    result.manifold = dispatchShapePair(pair, bodies, shapes);
    result.detected = result.manifold.valid;

const char* contact_pair_preflight_reason_name(const ContactPairPreflight& preflight) {
    return contact_pair_reject_reason_name(preflight.reason);

bool generate_contact_manifold_if_valid(ContactManifold& manifold) {
    if (!can_finalize_contact_manifold(manifold)) {
        manifold.clear();
        return false;
    return generate_contact_manifold(manifold);

bool is_plane_plane_contact_pair(
    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);
    return typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::Plane;

bool can_dispatch_contact_pair(

bool contact_pair_preflight_matches(
    const ContactPairPreflight& preflight,
    ContactPairRejectReason expected) {
    return preflight.reason == expected;

ManifoldFinalizePreflight preflight_finalize_contact_manifold(const ContactManifold& manifold) {
    ManifoldFinalizePreflight preflight{};
    if (manifold.empty()) {
        preflight.skipped = true;
        preflight.empty = true;
        preflight.wouldBeEmptyAfterPrune = true;
        return preflight;

    preflight.invalidNormal = !manifold.hasValidNormal();
    preflight.noPenetratingPoints = !manifold.hasPenetratingPoints();
    preflight.wouldBeEmptyAfterPrune = manifold.wouldBeEmptyAfterPrune();

bool can_skip_finalize_contact_manifold(const ContactManifold& manifold) {
    return manifold.valid && manifold.hasFrictionBasis() &&
           std::fabs(manifold.contactNormal.length() - 1.f) <= 1e-4f;

bool generate_contact_manifold_if_needed(ContactManifold& manifold) {
    if (can_skip_finalize_contact_manifold(manifold)) {
        return true;

    const ManifoldFinalizePreflight preflight = preflight_finalize_contact_manifold(manifold);
    if (!preflight.can_finalize()) {


ContactPairRejectBreakdown contact_pair_reject_breakdown(
    ContactPairRejectBreakdown breakdown{};

    breakdown.selfPair = is_self_contact_pair(pair);
    if (breakdown.selfPair) {
        breakdown.reason = ContactPairRejectReason::SelfPair;
        return breakdown;

    breakdown.outOfRangeBody = is_out_of_range_contact_pair(pair, bodies);
    if (breakdown.outOfRangeBody) {
        breakdown.reason = ContactPairRejectReason::OutOfRangeBody;

    breakdown.missingShape = is_missing_shape_contact_pair(pair, shapes);
    if (breakdown.missingShape) {
        breakdown.reason = ContactPairRejectReason::MissingShape;

    breakdown.bothTriggers = is_trigger_contact_pair(pair, bodies);
    if (breakdown.bothTriggers) {
        breakdown.reason = ContactPairRejectReason::BothTriggers;

    breakdown.unsupportedShapePair = is_unsupported_shape_pair(pair, shapes);
    if (breakdown.unsupportedShapePair) {
        breakdown.reason = ContactPairRejectReason::UnsupportedShapePair;

    breakdown.bothStatic = is_static_contact_pair(pair, bodies);
    if (breakdown.bothStatic) {
        breakdown.reason = ContactPairRejectReason::BothStatic;

    breakdown.degenerateShape = is_degenerate_shape_pair(pair, shapes);
    if (breakdown.degenerateShape) {
        breakdown.reason = ContactPairRejectReason::DegenerateShape;


bool contact_pair_rejects_with_breakdown(
    const CollisionShapeSoA& shapes,
    return contact_pair_reject_breakdown(pair, bodies, shapes).reason == expected;
    return preflight_contact_pair(pair, bodies, shapes).rejected;

ContactManifoldFinalizePreflight preflight_contact_manifold_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    ContactManifoldFinalizePreflight preflight{};

    if (!manifold.hasValidNormal()) {
        preflight.invalidNormal = true;

    if (!manifold.hasPenetratingPoints(separationEpsilon)) {
        preflight.noPenetratingPoints = true;
        preflight.pruneWouldEmpty = true;

    const ManifoldPrunePreflight prunePreflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);
    preflight.pruneWouldEmpty = prunePreflight.wouldBeEmpty;

bool should_skip_contact_manifold_finalize(
    return !preflight_contact_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon).can_finalize();

bool is_empty_narrowphase_input(
    return bodies.count() == 0u || shapes.count() == 0u;

bool can_skip_narrowphase_for_empty_input(
    return is_empty_narrowphase_input(bodies, shapes);

bool contact_pair_was_rejected(const ContactPairPreflight& preflight) {
    return preflight.rejected;


    preflight.empty = false;
    preflight.allSeparated = !manifold.hasPenetratingPoints();

bool should_skip_finalize_contact_manifold(const ContactManifold& manifold) {
    return !preflight_finalize_contact_manifold(manifold).can_finalize();

bool generate_contact_manifold_guarded(ContactManifold& manifold) {

ContactManifold detect_contacts_pair_guarded(
    const ContactPairPreflight preflight = preflight_contact_pair(pair, bodies, shapes);
    if (contact_pair_was_rejected(preflight)) {
    return detect_contacts_pair(pair, bodies, shapes);

ContactPairRejectReason contact_pair_reject_reason(
    u32 bodyA,
    u32 bodyB,
    return contact_pair_reject_reason({bodyA, bodyB}, bodies, shapes);

bool is_contact_pair_dispatchable(

bool should_reject_contact_pair(
    return should_skip_contact_pair_dispatch(pair, bodies, shapes);

bool contact_pair_preflight_matches_reason(

u32 count_rejected_contact_pairs(
    const std::vector<broadphase::CandidatePair>& pairs,
    u32 rejected = 0u;
    for (const broadphase::CandidatePair& pair : pairs) {
        if (should_reject_contact_pair(pair, bodies, shapes)) {
            ++rejected;
    return rejected;
    return preflight_narrowphase_pairs(pairs, bodies, shapes).can_skip_batch();
}

u32 count_dispatchable_contact_pairs(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    u32 dispatchable = 0u;
    for (const broadphase::CandidatePair& pair : pairs) {
        if (is_contact_pair_dispatchable(pair, bodies, shapes)) {
            ++dispatchable;
        }
    return dispatchable;

ContactPairRejectPreflight preflight_contact_pair_reject(
    const broadphase::CandidatePair& pair,
    ContactPairRejectPreflight preflight{};
    preflight.selfPair = is_self_contact_pair(pair);
    preflight.outOfRange = is_out_of_range_contact_pair(pair, bodies);
    preflight.missingShape = is_missing_shape_contact_pair(pair, shapes);
    preflight.bothTriggers = is_trigger_contact_pair(pair, bodies);
    preflight.unsupportedPair = is_unsupported_shape_pair(pair, shapes);
    preflight.bothStatic = is_static_contact_pair(pair, bodies);
    preflight.degenerateShape = is_degenerate_shape_pair(pair, shapes);
    preflight.reason = contact_pair_reject_reason(pair, bodies, shapes);
    preflight.rejected = preflight.reason != ContactPairRejectReason::None;
    return preflight;

bool should_reject_contact_pair(
    return is_invalid_contact_pair(pair, bodies, shapes);

bool contact_pair_has_valid_indices(
    const RigidBodySoA& bodies) {
    return !is_self_contact_pair(pair) && !is_out_of_range_contact_pair(pair, bodies);

bool contact_pair_has_shapes(
    return !is_missing_shape_contact_pair(pair, shapes);

const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason) {
    switch (reason) {
    case ManifoldFinalizeRejectReason::None:
        return "None";
    case ManifoldFinalizeRejectReason::Empty:
        return "Empty";
    case ManifoldFinalizeRejectReason::InvalidNormal:
        return "InvalidNormal";
    case ManifoldFinalizeRejectReason::NoPenetratingPoints:
        return "NoPenetratingPoints";
    case ManifoldFinalizeRejectReason::PruneWouldEmpty:
        return "PruneWouldEmpty";
    return "Unknown";

ManifoldFinalizeRejectReason manifold_finalize_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    if (manifold.empty()) {
        return ManifoldFinalizeRejectReason::Empty;
    if (!manifold.hasValidNormal()) {
        return ManifoldFinalizeRejectReason::InvalidNormal;
    if (!manifold.hasPenetratingPoints()) {
        return ManifoldFinalizeRejectReason::NoPenetratingPoints;
    if (manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {
        return ManifoldFinalizeRejectReason::PruneWouldEmpty;
    return ManifoldFinalizeRejectReason::None;

ManifoldFinalizePreflight preflight_finalize_contact_manifold(
    ManifoldFinalizePreflight preflight{};
    preflight.empty = manifold.empty();
    preflight.invalidNormal = !preflight.empty && !manifold.hasValidNormal();
    preflight.noPenetratingPoints = !preflight.empty && !manifold.hasPenetratingPoints();
    preflight.pruneWouldEmpty =
        !preflight.empty && manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon);
    preflight.reason = manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon);
    preflight.rejected = preflight.reason != ManifoldFinalizeRejectReason::None;

bool should_skip_finalize_contact_manifold(
    return !preflight_finalize_contact_manifold(manifold, separationEpsilon, duplicateEpsilon).can_finalize();

bool generate_contact_manifold_if_needed(ContactManifold& manifold) {
    if (!can_finalize_contact_manifold(manifold)) {
        return false;
    return generate_contact_manifold(manifold);

ManifoldPruneFinalizePreflight preflight_manifold_prune_finalize(
    ManifoldPruneFinalizePreflight preflight{};
        preflight.skipped = true;
        preflight.prune.skipped = true;
        preflight.prune.wouldBeEmpty = true;
        preflight.finalize.empty = true;
        preflight.finalize.reason = ManifoldFinalizeRejectReason::Empty;
        preflight.finalize.rejected = true;

    preflight.prune = preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);
    preflight.finalize = preflight_finalize_contact_manifold(manifold, separationEpsilon, duplicateEpsilon);

bool should_skip_prune_contact_manifold(
        return true;
    return manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon);
bool contact_pair_preflight_matches_reason(
    const ContactPairPreflight& preflight,
    ContactPairRejectReason expected) {
    return preflight.reason == expected;

bool contact_pair_preflight_rejects_for_reason(
    return preflight.rejected && preflight.reason == expected;

bool can_dispatch_contact_pair(const ContactPairPreflight& preflight) {
    return preflight.can_dispatch();
bool contact_pair_has_reject_reason(
    return contact_pair_reject_reason(pair, bodies, shapes) != ContactPairRejectReason::None;


ContactPairDispatchPreflight preflight_contact_pair_dispatch(
    ContactPairDispatchPreflight preflight{};
    preflight.has_valid_bodies =
        !is_self_contact_pair(pair) && !is_out_of_range_contact_pair(pair, bodies);
    preflight.has_valid_shapes =
        preflight.has_valid_bodies && !is_missing_shape_contact_pair(pair, shapes);

ContactPairDispatchResult dispatch_contact_pair(
    ContactPairDispatchResult result{};
    result.preflight = preflight_contact_pair_dispatch(pair, bodies, shapes);
    if (!result.preflight.can_dispatch()) {
        return result;

    result.manifold = dispatchShapePair(pair, bodies, shapes);
    result.dispatched = true;
    preflight.selfPair = preflight.reason == ContactPairRejectReason::SelfPair;
    preflight.outOfRangeBody = preflight.reason == ContactPairRejectReason::OutOfRangeBody;
    preflight.missingShape = preflight.reason == ContactPairRejectReason::MissingShape;
    preflight.bothTriggers = preflight.reason == ContactPairRejectReason::BothTriggers;
    preflight.unsupportedShapePair = preflight.reason == ContactPairRejectReason::UnsupportedShapePair;
    preflight.bothStatic = preflight.reason == ContactPairRejectReason::BothStatic;
    preflight.degenerateShape = preflight.reason == ContactPairRejectReason::DegenerateShape;

    return should_skip_contact_pair_dispatch(pair, bodies, shapes);

    preflight.reject = preflight_contact_pair_reject(pair, bodies, shapes);
    preflight.skipped = preflight.reject.rejected;
ContactPairDispatchResult detect_contacts_pair_result(
    const ContactPairPreflight preflight = preflight_contact_pair(pair, bodies, shapes);
    result.reason = preflight.reason;
    if (!preflight.can_dispatch()) {
        result.skipped = true;

    result.detected = true;

ContactPairDispatchResult detect_contacts_pair_guarded(
    return detect_contacts_pair_result(pair, bodies, shapes);
bool contact_pair_deepen_rejects_for_reason(
    const CollisionShapeSoA& shapes,
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;

const char* narrowphase_reject_reason_name(NarrowphaseRejectReason reason) {
    case NarrowphaseRejectReason::None:
    case NarrowphaseRejectReason::EmptyPairList:
        return "EmptyPairList";
    case NarrowphaseRejectReason::AllPairsRejected:
        return "AllPairsRejected";

NarrowphaseRejectReason narrowphase_reject_reason(
    if (pairs.empty()) {
        return NarrowphaseRejectReason::EmptyPairList;
    return !should_run_narrowphase(pairs, bodies, shapes);


bool is_plane_plane_contact_pair(
    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {

    return shapeType(shapes, shapeA) == CollisionShapeType::Plane &&
           shapeType(shapes, shapeB) == CollisionShapeType::Plane;

bool is_zero_friction_contact_pair(
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count()) {

    const bool zeroA =
        bodies.frictionStatic[pair.bodyA] <= 0.f && bodies.frictionDynamic[pair.bodyA] <= 0.f;
    const bool zeroB =
        bodies.frictionStatic[pair.bodyB] <= 0.f && bodies.frictionDynamic[pair.bodyB] <= 0.f;
    return zeroA && zeroB;

        if (!should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {
            return NarrowphaseRejectReason::None;
    return NarrowphaseRejectReason::AllPairsRejected;

bool narrowphase_rejects_for_reason(
    NarrowphaseRejectReason expected) {
    return narrowphase_reject_reason(pairs, bodies, shapes) == expected;

NarrowphasePairListPreflight preflight_narrowphase_pair_list(
    NarrowphasePairListPreflight preflight{};
    preflight.reason = narrowphase_reject_reason(pairs, bodies, shapes);
    preflight.emptyPairList = preflight.reason == NarrowphaseRejectReason::EmptyPairList;
    preflight.allPairsRejected = preflight.reason == NarrowphaseRejectReason::AllPairsRejected;

    if (preflight.reason == NarrowphaseRejectReason::None) {
        preflight.dispatchablePairCount = static_cast<u32>(pairs.size());
            if (should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {
                --preflight.dispatchablePairCount;

bool can_skip_narrowphase(
    return narrowphase_reject_reason(pairs, bodies, shapes) != NarrowphaseRejectReason::None;



bool has_dispatchable_contact_pair(
    return count_dispatchable_contact_pairs(pairs, bodies, shapes) > 0u;

NarrowphasePreflight preflight_narrowphase(
    NarrowphasePreflight preflight{};
    preflight.totalPairs = static_cast<u32>(pairs.size());
    preflight.dispatchablePairs = count_dispatchable_contact_pairs(pairs, bodies, shapes);
    preflight.rejectedPairs = preflight.totalPairs - preflight.dispatchablePairs;
NarrowphaseBatchPreflight preflight_narrowphase_batch(
    NarrowphaseBatchPreflight preflight{};
    preflight.dispatchableCount = count_dispatchable_contact_pairs(pairs, bodies, shapes);
    preflight.rejectedCount = preflight.totalPairs - preflight.dispatchableCount;
    preflight.allRejected = preflight.totalPairs == 0u || preflight.dispatchableCount == 0u;

bool should_run_narrowphase(
    return preflight_narrowphase_batch(pairs, bodies, shapes).can_dispatch();


u32 count_dispatchable_contact_pairs_deepen(

    preflight.dispatchableCount = count_dispatchable_contact_pairs_deepen(pairs, bodies, shapes);

bool should_run_narrowphase_dispatch(
    return !can_skip_narrowphase(pairs, bodies, shapes);
    return preflight_narrowphase_pairs(pairs, bodies, shapes).can_skip_batch();


NarrowphasePairBatchStats compute_narrowphase_pair_stats(
    NarrowphasePairBatchStats stats{};
    stats.totalPairs = static_cast<u32>(pairs.size());

            ++stats.rejectedPairs;
        } else {
            ++stats.dispatchablePairs;

    return stats;

    return compute_narrowphase_pair_stats(pairs, bodies, shapes).dispatchablePairs;

NarrowphasePairBatchPreflight preflight_narrowphase_pairs(
    NarrowphasePairBatchPreflight preflight{};
    preflight.stats = compute_narrowphase_pair_stats(pairs, bodies, shapes);
    preflight.allRejected = preflight.stats.totalPairs == 0u || preflight.stats.dispatchablePairs == 0u;
    preflight.hasDispatchable = preflight.stats.dispatchablePairs > 0u;

ContactManifold detect_contacts_pair_if_needed(
        return invalidContactManifold();
    return detect_contacts_pair(pair, bodies, shapes);

namespace {

bool hasColliderDispatchPath(CollisionShapeType typeA, CollisionShapeType typeB) {
    if (typeA == CollisionShapeType::Sphere || typeB == CollisionShapeType::Sphere) {
    if (typeA == CollisionShapeType::Box && typeB == CollisionShapeType::Box) {

} // namespace

bool is_zero_inv_mass_contact_pair(
    return bodies.invMasses[pair.bodyA] <= 0.f && bodies.invMasses[pair.bodyB] <= 0.f;

bool is_undispatched_shape_pair(

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);
    return !hasColliderDispatchPath(typeA, typeB);

ContactPairRejectReason contact_pair_deepen2_reject_reason(
bool should_run_contact_pair_dispatch(
    return !should_skip_contact_pair_dispatch(pair, bodies, shapes);

ContactPairRejectReason contact_pair_deepen_reject_reason(
    const ContactPairRejectReason deepenReason =
        contact_pair_deepen_reject_reason(pair, bodies, shapes);
    if (deepenReason != ContactPairRejectReason::None) {
        return deepenReason;
    if (is_zero_inv_mass_contact_pair(pair, bodies)) {
        return ContactPairRejectReason::BothZeroInvMass;
    if (is_undispatched_shape_pair(pair, shapes)) {
        return ContactPairRejectReason::NoColliderDispatch;
        return ContactPairRejectReason::ZeroInvMass;
    if (is_negative_inverse_mass_pair(pair, bodies)) {
        return ContactPairRejectReason::NegativeInverseMass;
    if (is_zero_mass_contact_pair(pair, bodies)) {
        return ContactPairRejectReason::BothZeroMass;
    if (is_sleeping_kinematic_mix_pair(pair, bodies)) {
        return ContactPairRejectReason::SleepingKinematicMix;
    return ContactPairRejectReason::None;

ContactPairDeepen2Preflight preflight_contact_pair_deepen2(
    ContactPairDeepen2Preflight preflight{};
    preflight.reason = contact_pair_deepen2_reject_reason(pair, bodies, shapes);

bool should_skip_contact_pair_deepen2_dispatch(
    return contact_pair_deepen2_reject_reason(pair, bodies, shapes) != ContactPairRejectReason::None;

bool can_skip_narrowphase_deepen2(

bool should_run_contact_pair_deepen_dispatch(
    return !should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);

    return !has_dispatchable_contact_pairs(pairs, bodies, shapes);




bool is_plane_plane_pair(

    return typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::Plane;

bool has_dispatchable_contact_pairs(

ContactPairBatchPreflight preflight_contact_pair_batch(
    ContactPairBatchPreflight preflight{};

        if (!should_skip_contact_pair_deepen2_dispatch(pair, bodies, shapes)) {
            ++preflight.rejectedCount;
            ++preflight.dispatchableCount;

    return !can_run_narrowphase(pairs, bodies, shapes);


bool can_run_narrowphase(


    preflight.pairCount = static_cast<u32>(pairs.size());


ContactManifold detect_contacts_pair_if_valid(
    if (should_skip_contact_pair_dispatch(pair, bodies, shapes)) {
    return dispatchShapePair(pair, bodies, shapes);

ContactManifold detect_contacts_pair_deepen(








u32 count_rejected_contact_pairs_deepen(
    u32 rejected = 0u;
            ++rejected;
    return rejected;


    if (count_dispatchable_contact_pairs(pairs, bodies, shapes) == 0u) {


    preflight.emptyPairList = pairs.empty();
    preflight.rejectedPairs = count_rejected_contact_pairs_deepen(pairs, bodies, shapes);
    preflight.allPairsRejected = !pairs.empty() && preflight.dispatchablePairs == 0u;

bool can_skip_narrowphase_preflight(
    return !preflight_narrowphase(pairs, bodies, shapes).can_dispatch();


    preflight.stats.totalPairs = static_cast<u32>(pairs.size());

            ++preflight.stats.rejectedCount;
            ++preflight.stats.dispatchableCount;

    if (preflight.stats.dispatchableCount == 0u) {

    return preflight_narrowphase_batch(pairs, bodies, shapes).stats.dispatchableCount;

    return preflight_narrowphase_pairs(pairs, bodies, shapes).hasDispatchable;




bool is_invalid_contact_pair_deepen(
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) != ContactPairRejectReason::None;

bool is_valid_contact_pair_deepen(
    return !is_invalid_contact_pair_deepen(pair, bodies, shapes);







    if (preflight.reason != NarrowphaseRejectReason::None) {
        preflight.rejectedCount = preflight.pairCount;



bool can_skip_narrowphase_dispatch(
    return can_skip_narrowphase(pairs, bodies, shapes);



NarrowphasePairBatchStats compute_narrowphase_pair_stats(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    NarrowphasePairBatchStats stats{};
    stats.totalPairs = static_cast<u32>(pairs.size());

    for (const broadphase::CandidatePair& pair : pairs) {
        if (should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {
            ++stats.rejectedPairs;
        } else {
            ++stats.dispatchablePairs;
        }
    }

    return stats;
}

NarrowphasePairBatchPreflight preflight_narrowphase_pairs(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    NarrowphasePairBatchPreflight preflight{};
    preflight.stats = compute_narrowphase_pair_stats(pairs, bodies, shapes);
    preflight.allRejected =
        preflight.stats.totalPairs == 0u || preflight.stats.dispatchablePairs == 0u;
    preflight.hasDispatchable = preflight.stats.dispatchablePairs > 0u;
    return preflight;
}

bool should_run_narrowphase_dispatch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !can_skip_narrowphase(pairs, bodies, shapes);
}

ContactManifold detect_contacts_pair_if_needed(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {
        return invalidContactManifold();
    }
    return detect_contacts_pair(pair, bodies, shapes);
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

bool is_invalid_plane_normal_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes) {
    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
        return false;
    }

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);
    if (typeA == CollisionShapeType::Plane &&
        shapes.params[shapeA].length() < 1e-8f) {
        return true;
    if (typeB == CollisionShapeType::Plane &&
        shapes.params[shapeB].length() < 1e-8f) {

bool is_shape_body_mismatch_contact_pair(

    return shapes.bodyIndices[shapeA] != pair.bodyA ||
           shapes.bodyIndices[shapeB] != pair.bodyB;

ContactPairRejectReason contact_pair_deepen_pass_reject_reason(
    const RigidBodySoA& bodies,
    const ContactPairRejectReason deepenReason =
        contact_pair_deepen_reject_reason(pair, bodies, shapes);
    if (deepenReason != ContactPairRejectReason::None) {
        if (deepenReason == ContactPairRejectReason::UnsupportedShapePair &&
            is_plane_plane_contact_pair(pair, shapes)) {
            return ContactPairRejectReason::PlanePlane;
        if (deepenReason == ContactPairRejectReason::DegenerateShape &&
            is_invalid_plane_normal_pair(pair, shapes)) {
            return ContactPairRejectReason::InvalidPlaneNormal;
        return deepenReason;
    if (is_shape_body_mismatch_contact_pair(pair, shapes)) {
        return ContactPairRejectReason::ShapeBodyMismatch;
    return ContactPairRejectReason::None;

ContactPairDeepenPassPreflight preflight_contact_pair_deepen_pass(
    ContactPairDeepenPassPreflight preflight{};
    preflight.reason = contact_pair_deepen_pass_reject_reason(pair, bodies, shapes);
    preflight.rejected = preflight.reason != ContactPairRejectReason::None;
    return preflight;

bool should_skip_contact_pair_deepen_pass_dispatch(
    return contact_pair_deepen_pass_reject_reason(pair, bodies, shapes) != ContactPairRejectReason::None;

ContactPairBatchPreflight preflight_contact_pair_batch(
    const std::vector<broadphase::CandidatePair>& pairs,
    ContactPairBatchPreflight preflight{};
    preflight.totalPairs = static_cast<u32>(pairs.size());
    preflight.emptyInput = pairs.empty();
    if (pairs.empty()) {
        preflight.allRejected = true;

    for (const broadphase::CandidatePair& pair : pairs) {
        if (should_skip_contact_pair_deepen_pass_dispatch(pair, bodies, shapes)) {
            ++preflight.rejectedCount;
        } else {
            ++preflight.dispatchableCount;
    preflight.allRejected = preflight.dispatchableCount == 0u;

bool should_skip_contact_pair_batch(
    return !preflight_contact_pair_batch(pairs, bodies, shapes).can_dispatch();

bool should_run_narrowphase(
    return !can_skip_narrowphase(pairs, bodies, shapes);

bool can_dispatch_contact_pair_deepen(
    return !should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);

bool should_run_contact_pair_deepen_dispatch(
    return preflight_contact_pair_deepen(pair, bodies, shapes).can_dispatch();

u32 count_rejected_contact_pairs(
    return static_cast<u32>(pairs.size()) - count_dispatchable_contact_pairs(pairs, bodies, shapes);
const char* narrowphase_batch_reject_reason_name(NarrowphaseBatchRejectReason reason) {
    switch (reason) {
    case NarrowphaseBatchRejectReason::None:
        return "None";
    case NarrowphaseBatchRejectReason::EmptyPairList:
        return "EmptyPairList";
    case NarrowphaseBatchRejectReason::AllRejected:
        return "AllRejected";
    return "Unknown";

NarrowphaseBatchRejectReason narrowphase_batch_reject_reason(
        return NarrowphaseBatchRejectReason::EmptyPairList;
    if (!has_dispatchable_contact_pair(pairs, bodies, shapes)) {
        return NarrowphaseBatchRejectReason::AllRejected;
    return NarrowphaseBatchRejectReason::None;

bool narrowphase_batch_rejects_for_reason(
    const CollisionShapeSoA& shapes,
    NarrowphaseBatchRejectReason expected) {
    return narrowphase_batch_reject_reason(pairs, bodies, shapes) == expected;
}

NarrowphaseBatchPreflight preflight_narrowphase_batch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    NarrowphaseBatchPreflight preflight{};
    preflight.reason = narrowphase_batch_reject_reason(pairs, bodies, shapes);
    preflight.pairCount = static_cast<u32>(pairs.size());
    preflight.dispatchableCount = count_dispatchable_contact_pairs(pairs, bodies, shapes);
    preflight.rejectedCount = preflight.pairCount - preflight.dispatchableCount;
    preflight.canSkip = preflight.dispatchableCount == 0u;
    return preflight;
}

bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;
}

NarrowphaseBatchPreflight preflight_narrowphase_batch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    NarrowphaseBatchPreflight preflight{};
    preflight.totalPairs = static_cast<u32>(pairs.size());
    preflight.dispatchableCount = count_dispatchable_contact_pairs(pairs, bodies, shapes);
    preflight.rejectedCount = preflight.totalPairs - preflight.dispatchableCount;
    return preflight;
}

bool narrowphase_batch_all_rejected(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return preflight_narrowphase_batch(pairs, bodies, shapes).allRejected();
}

bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;
}

bool should_run_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

bool can_skip_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;
}

bool should_run_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;
}

bool should_run_narrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !can_skip_narrowphase(pairs, bodies, shapes);
}

bool should_run_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

NarrowphaseDispatchPreflight preflight_narrowphase_dispatch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    NarrowphaseDispatchPreflight preflight{};
    if (pairs.empty()) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.dispatchableCount = count_dispatchable_contact_pairs(pairs, bodies, shapes);
    return preflight;
}

bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;
}

bool is_unnormalized_plane_shape_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes,
    f32 lengthEpsilon) {
    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
        return false;
    }

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);
    if (typeA == CollisionShapeType::Plane &&
        isPlaneNormalUnnormalized(shapes.params[shapeA], lengthEpsilon)) {
        return true;
    }
    if (typeB == CollisionShapeType::Plane &&
        isPlaneNormalUnnormalized(shapes.params[shapeB], lengthEpsilon)) {
        return true;
    }
    return false;
}

bool is_near_degenerate_shape_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes,
    f32 extentEpsilon) {
    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
        return false;
    }

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);
    if (isShapeNearDegenerate(typeA, shapes.params[shapeA], extentEpsilon) ||
        isShapeNearDegenerate(typeB, shapes.params[shapeB], extentEpsilon)) {
        return true;
    }

    return (typeA == CollisionShapeType::Capsule &&
            shapes.params[shapeA].y > 0.f && shapes.params[shapeA].y < extentEpsilon) ||
           (typeB == CollisionShapeType::Capsule &&
            shapes.params[shapeB].y > 0.f && shapes.params[shapeB].y < extentEpsilon);
}

bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;
}

bool is_dispatchable_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

bool should_run_narrowphase(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !can_skip_narrowphase(pairs, bodies, shapes);
}

NarrowphaseDispatchPreflight preflight_narrowphase_dispatch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    NarrowphaseDispatchPreflight preflight{};
    preflight.pairCount = static_cast<u32>(pairs.size());
    preflight.dispatchableCount = count_dispatchable_contact_pairs(pairs, bodies, shapes);
    return preflight;
}

ContactManifold detect_contacts_pair_deepen(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {
        return invalidContactManifold();
    }
    return dispatchShapePair(pair, bodies, shapes);
}

bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;
}

bool should_run_contact_pair_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_dispatch(pair, bodies, shapes);
}

bool can_skip_contact_pair_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return should_skip_contact_pair_dispatch(pair, bodies, shapes);
}

bool should_run_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

bool can_skip_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;
}

u32 count_rejected_contact_pairs_deepen(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    const u32 totalPairs = static_cast<u32>(pairs.size());
    if (totalPairs == 0u) {
        return 0u;
    }
    return totalPairs - count_dispatchable_contact_pairs(pairs, bodies, shapes);
}

ContactPairBatchDeepenPreflight preflight_contact_pair_batch_deepen(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    ContactPairBatchDeepenPreflight preflight{};
    preflight.totalPairs = static_cast<u32>(pairs.size());
    preflight.dispatchableCount = count_dispatchable_contact_pairs(pairs, bodies, shapes);
    preflight.rejectedCount = preflight.totalPairs - preflight.dispatchableCount;
    preflight.allRejected = preflight.totalPairs > 0u && preflight.dispatchableCount == 0u;
    return preflight;
}

bool contact_pair_deepen_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    return contact_pair_deepen_reject_reason(pair, bodies, shapes) == expected;
}

bool is_dispatchable_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

ContactManifold detect_contacts_pair_deepen(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {
        return invalidContactManifold();
    }
    return dispatchShapePair(pair, bodies, shapes);
}

std::vector<broadphase::CandidatePair> filter_dispatchable_contact_pairs(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    std::vector<broadphase::CandidatePair> dispatchable;
    dispatchable.reserve(pairs.size());
    for (const broadphase::CandidatePair& pair : pairs) {
        if (is_dispatchable_contact_pair(pair, bodies, shapes)) {
            dispatchable.push_back(pair);
        }
    }
    return dispatchable;
}

ContactManifold detect_contacts_pair_deepen(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {
        return invalidContactManifold();
    }
    return dispatchShapePair(pair, bodies, shapes);
}

bool contact_pair_deepen_preflight_rejects_for_reason(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    ContactPairRejectReason expected) {
    const ContactPairDeepenPreflight preflight = preflight_contact_pair_deepen(pair, bodies, shapes);
    return preflight.rejected && preflight.reason == expected;
}

u32 first_dispatchable_contact_pair_index(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    for (u32 i = 0u; i < static_cast<u32>(pairs.size()); ++i) {
        if (!should_skip_contact_pair_deepen_dispatch(pairs[i], bodies, shapes)) {
            return i;
        }
    }
    return static_cast<u32>(pairs.size());
}

bool should_run_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

ContactManifold detect_contacts_pair_with_preflight(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {
        return invalidContactManifold();
    }
    return detect_contacts_pair(pair, bodies, shapes);
}

bool is_capsule_capsule_contact_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes) {
    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
        return false;
    }

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);
    return typeA == CollisionShapeType::Capsule && typeB == CollisionShapeType::Capsule;
}

bool is_box_plane_contact_pair(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes) {
    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
        return false;
    }

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);
    return (typeA == CollisionShapeType::Box && typeB == CollisionShapeType::Plane) ||
           (typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::Box);
}

ContactPairRejectReason first_contact_pair_deepen_reject_in_batch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (pairs.empty()) {
        return ContactPairRejectReason::OutOfRangeBody;
    }

    for (const broadphase::CandidatePair& pair : pairs) {
        const ContactPairRejectReason reason = contact_pair_deepen_reject_reason(pair, bodies, shapes);
        if (reason == ContactPairRejectReason::None) {
            return ContactPairRejectReason::None;
        }
    }

    return contact_pair_deepen_reject_reason(pairs.front(), bodies, shapes);
}

bool can_skip_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

bool should_run_contact_pair_deepen_dispatch(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !can_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);
}

bool can_skip_narrowphase_batch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return can_skip_narrowphase(pairs, bodies, shapes);
}

bool should_run_narrowphase_batch(
    const std::vector<broadphase::CandidatePair>& pairs,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return !can_skip_narrowphase_batch(pairs, bodies, shapes);
}

} // namespace fuse::physics::narrowphase
