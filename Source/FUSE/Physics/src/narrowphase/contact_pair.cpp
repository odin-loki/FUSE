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

// --- deepen additive from deepen-b4-narrowphase-manifold-prune-4828 ---
    return contact_pair_reject_reason(pair, bodies, shapes) == ContactPairRejectReason::None;
const char* contact_pair_reject_reason_label(ContactPairRejectReason reason) {

// --- deepen additive from deepen-b4-narrowphase-guards-56bb ---
    if (should_skip_contact_pair_dispatch(pair, bodies, shapes)) {
const char* contact_pair_preflight_reason_name(const ContactPairPreflight& preflight) {

// --- deepen additive from deepen-b4-narrowphase-guards-d8a9 ---
    const ContactPairPreflight& preflight,
ManifoldFinalizePreflight preflight_finalize_contact_manifold(const ContactManifold& manifold) {
    ManifoldFinalizePreflight preflight{};
    const ManifoldFinalizePreflight preflight = preflight_finalize_contact_manifold(manifold);

// --- deepen additive from b4-narrowphase-deepen-guards-e063 ---
        breakdown.reason = ContactPairRejectReason::SelfPair;
        breakdown.reason = ContactPairRejectReason::OutOfRangeBody;
        breakdown.reason = ContactPairRejectReason::MissingShape;
        breakdown.reason = ContactPairRejectReason::BothTriggers;
        breakdown.reason = ContactPairRejectReason::UnsupportedShapePair;
        breakdown.reason = ContactPairRejectReason::BothStatic;
        breakdown.reason = ContactPairRejectReason::DegenerateShape;

// --- deepen additive from b4-narrowphase-guards-deepen-2074 ---
    const ContactManifoldFinalizePreflight preflight = preflight_contact_manifold_finalize(manifold);
ContactManifoldFinalizePreflight preflight_contact_manifold_finalize(
    ContactManifoldFinalizePreflight preflight{};
    const ManifoldPrunePreflight prunePreflight =
    preflight.pruneWouldEmpty = prunePreflight.wouldBeEmpty;
bool should_skip_contact_manifold_finalize(

// --- deepen additive from deepen-b4-narrowphase-guards-d11b ---
bool contact_pair_was_rejected(const ContactPairPreflight& preflight) {
bool should_skip_finalize_contact_manifold(const ContactManifold& manifold) {
    const ContactPairPreflight preflight = preflight_contact_pair(pair, bodies, shapes);

// --- deepen additive from b4-narrowphase-deepen-guards-c64f ---
    return should_skip_contact_pair_dispatch(pair, bodies, shapes);

// --- deepen additive from deepen-b4-narrowphase-guards-d755 ---
ContactPairRejectPreflight preflight_contact_pair_reject(
    ContactPairRejectPreflight preflight{};
const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason) {
    case ManifoldFinalizeRejectReason::None:
    case ManifoldFinalizeRejectReason::Empty:
    case ManifoldFinalizeRejectReason::InvalidNormal:
    case ManifoldFinalizeRejectReason::NoPenetratingPoints:
    case ManifoldFinalizeRejectReason::PruneWouldEmpty:
ManifoldFinalizeRejectReason manifold_finalize_reject_reason(
        return ManifoldFinalizeRejectReason::Empty;
        return ManifoldFinalizeRejectReason::InvalidNormal;
        return ManifoldFinalizeRejectReason::NoPenetratingPoints;
        return ManifoldFinalizeRejectReason::PruneWouldEmpty;
    return ManifoldFinalizeRejectReason::None;
    preflight.rejected = preflight.reason != ManifoldFinalizeRejectReason::None;
ManifoldPruneFinalizePreflight preflight_manifold_prune_finalize(
    ManifoldPruneFinalizePreflight preflight{};
        preflight.finalize.reason = ManifoldFinalizeRejectReason::Empty;
bool should_skip_prune_contact_manifold(

// --- deepen additive from b4-narrowphase-guards-deepen-ea96 ---
bool can_dispatch_contact_pair(const ContactPairPreflight& preflight) {

// --- deepen additive from deepen-b4-narrowphase-guards-5111 ---
ContactPairDispatchPreflight preflight_contact_pair_dispatch(
    ContactPairDispatchPreflight preflight{};

// --- deepen additive from b4-narrowphase-deepen-guards-f32b ---
    preflight.selfPair = preflight.reason == ContactPairRejectReason::SelfPair;
    preflight.outOfRangeBody = preflight.reason == ContactPairRejectReason::OutOfRangeBody;
    preflight.missingShape = preflight.reason == ContactPairRejectReason::MissingShape;
    preflight.bothTriggers = preflight.reason == ContactPairRejectReason::BothTriggers;
    preflight.unsupportedShapePair = preflight.reason == ContactPairRejectReason::UnsupportedShapePair;
    preflight.bothStatic = preflight.reason == ContactPairRejectReason::BothStatic;
    preflight.degenerateShape = preflight.reason == ContactPairRejectReason::DegenerateShape;

// --- deepen additive from deepen-b4-narrowphase-guards-7360 ---
    const ManifoldFinalizePreflight finalizePreflight = preflight_manifold_finalize(manifold);
    if (finalizePreflight.wouldBeEmptyAfterPrune) {

// --- deepen additive from b4-narrowphase-deepen-guards-a773 ---
const char* narrowphase_reject_reason_name(NarrowphaseRejectReason reason) {
    case NarrowphaseRejectReason::None:
    case NarrowphaseRejectReason::EmptyPairList:
    case NarrowphaseRejectReason::AllPairsRejected:
NarrowphaseRejectReason narrowphase_reject_reason(
        return NarrowphaseRejectReason::EmptyPairList;
            return NarrowphaseRejectReason::None;
    return NarrowphaseRejectReason::AllPairsRejected;
    NarrowphaseRejectReason expected) {
NarrowphasePairListPreflight preflight_narrowphase_pair_list(
    NarrowphasePairListPreflight preflight{};
    preflight.emptyPairList = preflight.reason == NarrowphaseRejectReason::EmptyPairList;
    preflight.allPairsRejected = preflight.reason == NarrowphaseRejectReason::AllPairsRejected;
    if (preflight.reason == NarrowphaseRejectReason::None) {
            if (should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes)) {
    return narrowphase_reject_reason(pairs, bodies, shapes) != NarrowphaseRejectReason::None;

// --- deepen additive from deepen-b4-narrowphase-guards-72f5 ---
NarrowphasePreflight preflight_narrowphase(
    NarrowphasePreflight preflight{};

// --- deepen additive from b4-narrowphase-deepen-guards-699f ---
NarrowphasePairBatchPreflight preflight_narrowphase_pairs(
    NarrowphasePairBatchPreflight preflight{};

// --- deepen additive from deepen-b4-narrowphase-guards-1468 ---
    case ContactPairRejectReason::BothZeroInvMass:
    case ContactPairRejectReason::NoColliderDispatch:
ContactPairRejectReason contact_pair_deepen2_reject_reason(
    const ContactPairRejectReason deepenReason =
    if (deepenReason != ContactPairRejectReason::None) {
        return ContactPairRejectReason::BothZeroInvMass;
        return ContactPairRejectReason::NoColliderDispatch;
ContactPairDeepen2Preflight preflight_contact_pair_deepen2(
    ContactPairDeepen2Preflight preflight{};
bool should_skip_contact_pair_deepen2_dispatch(
    return contact_pair_deepen2_reject_reason(pair, bodies, shapes) != ContactPairRejectReason::None;
        if (!should_skip_contact_pair_deepen2_dispatch(pair, bodies, shapes)) {

// --- deepen additive from deepen-b4-narrowphase-guards-b463 ---
ContactPairBatchPreflight preflight_contact_pair_batch(
    ContactPairBatchPreflight preflight{};

// --- deepen additive from b4-narrowphase-deepen-guards-6e88 ---
    case ContactPairRejectReason::ZeroInvMass:
        return ContactPairRejectReason::ZeroInvMass;

// --- deepen additive from deepen-b4-narrowphase-guards-ddb5 ---
    case ContactPairRejectReason::NegativeInverseMass:
    case ContactPairRejectReason::BothZeroMass:
    case ContactPairRejectReason::SleepingKinematicMix:
        return ContactPairRejectReason::NegativeInverseMass;
        return ContactPairRejectReason::BothZeroMass;
        return ContactPairRejectReason::SleepingKinematicMix;

// --- deepen additive from deepen-b4-narrowphase-guards-f4c2 ---
    if (preflight.reason != NarrowphaseRejectReason::None) {

// --- deepen additive from b4-narrowphase-deepen-guards-6242 ---
    case ContactPairRejectReason::PlanePlane:
    case ContactPairRejectReason::InvalidPlaneNormal:
    case ContactPairRejectReason::ShapeBodyMismatch:
ContactPairRejectReason contact_pair_deepen_pass_reject_reason(
        if (deepenReason == ContactPairRejectReason::UnsupportedShapePair &&
            return ContactPairRejectReason::PlanePlane;
        if (deepenReason == ContactPairRejectReason::DegenerateShape &&
            return ContactPairRejectReason::InvalidPlaneNormal;
        return ContactPairRejectReason::ShapeBodyMismatch;
ContactPairDeepenPassPreflight preflight_contact_pair_deepen_pass(
    ContactPairDeepenPassPreflight preflight{};
bool should_skip_contact_pair_deepen_pass_dispatch(
    return contact_pair_deepen_pass_reject_reason(pair, bodies, shapes) != ContactPairRejectReason::None;
        if (should_skip_contact_pair_deepen_pass_dispatch(pair, bodies, shapes)) {
bool should_skip_contact_pair_batch(

// --- deepen additive from deepen-b4-narrowphase-guards-fbc2 ---
    preflight.isSleeping = preflight.reason == ContactPairRejectReason::BothSleeping;
    preflight.isKinematic = preflight.reason == ContactPairRejectReason::BothKinematic;
    preflight.isAnyTrigger = preflight.reason == ContactPairRejectReason::AnyTrigger;
    preflight.isMassless = preflight.reason == ContactPairRejectReason::BothMassless;
    preflight.isDeepenDegenerate = preflight.reason == ContactPairRejectReason::DegenerateShape &&

// --- deepen additive from deepen-b4-narrowphase-guards-bc5b ---
    return should_skip_contact_pair_deepen_dispatch(pair, bodies, shapes);

// --- deepen additive from deepen-b4-narrowphase-guards-b130 ---
    if (should_skip_friction_basis_preflight(manifold)) {

// --- deepen additive from b4-narrowphase-deepen-guards-4d64 ---
NarrowphaseDispatchPreflight preflight_narrowphase_dispatch(
    NarrowphaseDispatchPreflight preflight{};

// --- deepen additive from narrowphase-guard-pass-4c08 ---
    preflight.bothSleeping = preflight.reason == ContactPairRejectReason::BothSleeping;
    preflight.bothKinematic = preflight.reason == ContactPairRejectReason::BothKinematic;
    preflight.anyTrigger = preflight.reason == ContactPairRejectReason::AnyTrigger;
    preflight.bothMassless = preflight.reason == ContactPairRejectReason::BothMassless;

// --- deepen additive from deepen-b4-narrowphase-guards-754b ---
ContactPairBatchDeepenPreflight preflight_contact_pair_batch_deepen(
    ContactPairBatchDeepenPreflight preflight{};

// --- deepen additive from deepen-b4-narrowphase-guards-7d67 ---
    const ContactPairDeepenPreflight preflight = preflight_contact_pair_deepen(pair, bodies, shapes);
        if (!should_skip_contact_pair_deepen_dispatch(pairs[i], bodies, shapes)) {

// --- deepen additive from b4-narrowphase-deepen-guards-5d1f ---
ContactPairRejectReason first_contact_pair_deepen_reject_in_batch(
        const ContactPairRejectReason reason = contact_pair_deepen_reject_reason(pair, bodies, shapes);
        if (reason == ContactPairRejectReason::None) {

// --- deepen additive from b4-narrowphase-deepen-guards-8a17 ---
const char* narrowphase_batch_reject_reason_name(NarrowphaseBatchRejectReason reason) {
    case NarrowphaseBatchRejectReason::None:
    case NarrowphaseBatchRejectReason::EmptyPairList:
    case NarrowphaseBatchRejectReason::AllRejected:
NarrowphaseBatchRejectReason narrowphase_batch_reject_reason(
        return NarrowphaseBatchRejectReason::EmptyPairList;
        return NarrowphaseBatchRejectReason::AllRejected;
    return NarrowphaseBatchRejectReason::None;
    NarrowphaseBatchRejectReason expected) {

// --- deepen additive from b4-narrowphase-deepen-guards-68c9 ---
NarrowphaseRunPreflight preflight_run_narrowphase(
    NarrowphaseRunPreflight preflight{};

// --- deepen additive from deepen-b4-narrowphase-b46-8196 ---
ContactPairRejectReason first_contact_pair_deepen_reject_reason(
        if (reason != ContactPairRejectReason::None) {
NarrowphaseRunPreflight preflight_narrowphase_run(
bool should_skip_narrowphase_run(

// --- deepen additive from deepen-b4-narrowphase-6c66 ---
    if (!preflightContactBufferWrite(buffer, slot, manifold).canWrite()) {

// --- deepen additive from deepen-b4-narrowphase-9067 ---
NarrowphasePairSlotPreflight preflight_narrowphase_pair_slot(
    NarrowphasePairSlotPreflight preflight{};
bool should_skip_narrowphase_pair_slot(

// --- deepen additive from deepen-b4-narrowphase-guards-1644 ---
bool should_skip_contact_pair_dispatch_preflight(
