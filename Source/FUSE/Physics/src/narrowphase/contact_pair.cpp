#include <fuse/physics/narrowphase/contact_pair.hpp>

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/friction.hpp>

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

void compute_friction_tangents(ContactManifold& manifold) {
    if (should_skip_friction_tangents(manifold)) {
        manifold.frictionBasis = {};
        return;
    }

    if (has_cached_friction_basis(manifold)) {
        return;
    }

    const f32 normalLength = manifold.contactNormal.length();
    if (std::fabs(normalLength - 1.f) > 1e-4f) {
        manifold.contactNormal = manifold.contactNormal * (1.f / normalLength);
    }
    manifold.buildFrictionBasis();
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

ContactPairRejectPreflight preflight_contact_pair_reject(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
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
}

bool should_reject_contact_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    return is_invalid_contact_pair(pair, bodies, shapes);
}

bool contact_pair_has_valid_indices(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies) {
    return !is_self_contact_pair(pair) && !is_out_of_range_contact_pair(pair, bodies);
}

bool contact_pair_has_shapes(
    const broadphase::CandidatePair& pair,
    const CollisionShapeSoA& shapes) {
    return !is_missing_shape_contact_pair(pair, shapes);
}

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
    }
    return "Unknown";
}

ManifoldFinalizeRejectReason manifold_finalize_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    if (manifold.empty()) {
        return ManifoldFinalizeRejectReason::Empty;
    }
    if (!manifold.hasValidNormal()) {
        return ManifoldFinalizeRejectReason::InvalidNormal;
    }
    if (!manifold.hasPenetratingPoints()) {
        return ManifoldFinalizeRejectReason::NoPenetratingPoints;
    }
    if (manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {
        return ManifoldFinalizeRejectReason::PruneWouldEmpty;
    }
    return ManifoldFinalizeRejectReason::None;
}

ManifoldFinalizePreflight preflight_finalize_contact_manifold(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    ManifoldFinalizePreflight preflight{};
    preflight.empty = manifold.empty();
    preflight.invalidNormal = !preflight.empty && !manifold.hasValidNormal();
    preflight.noPenetratingPoints = !preflight.empty && !manifold.hasPenetratingPoints();
    preflight.pruneWouldEmpty =
        !preflight.empty && manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon);
    preflight.reason = manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon);
    preflight.rejected = preflight.reason != ManifoldFinalizeRejectReason::None;
    return preflight;
}

bool should_skip_finalize_contact_manifold(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    return !preflight_finalize_contact_manifold(manifold, separationEpsilon, duplicateEpsilon).can_finalize();
}

bool generate_contact_manifold_if_needed(ContactManifold& manifold) {
    if (!can_finalize_contact_manifold(manifold)) {
        return false;
    }
    return generate_contact_manifold(manifold);
}

ManifoldPruneFinalizePreflight preflight_manifold_prune_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    ManifoldPruneFinalizePreflight preflight{};
    if (manifold.empty()) {
        preflight.skipped = true;
        preflight.prune.skipped = true;
        preflight.prune.wouldBeEmpty = true;
        preflight.finalize.empty = true;
        preflight.finalize.reason = ManifoldFinalizeRejectReason::Empty;
        preflight.finalize.rejected = true;
        return preflight;
    }

    preflight.prune = preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);
    preflight.finalize = preflight_finalize_contact_manifold(manifold, separationEpsilon, duplicateEpsilon);
    return preflight;
}

bool should_skip_prune_contact_manifold(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    if (manifold.empty()) {
        return true;
    }
    return manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon);
}

} // namespace fuse::physics::narrowphase
