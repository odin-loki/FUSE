#include <fuse/physics/narrowphase/contact_pair.hpp>

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/friction.hpp>
#include <fuse/physics/narrowphase/gjk.hpp>
#include <fuse/physics/narrowphase/primitive_contacts.hpp>
#include <fuse/physics/rotation.hpp>
#include <fuse/physics/shapes/shape_pool.hpp>

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

/// Shape pairs with a narrowphase: the primitives with each other (not plane-plane); a convex hull
/// with every primitive, another hull and a triangle mesh; a triangle mesh, SDF or voxel volume with
/// spheres, boxes and capsules (and meshes with hulls). Concave shapes never meet each other or planes
/// (they are static level geometry).
bool shapePairSupported(CollisionShapeType typeA, CollisionShapeType typeB) {
    using T = CollisionShapeType;
    const auto rounded = [](T t) { return t == T::Sphere || t == T::Box || t == T::Capsule; };
    const auto one = [&](T x, T y) {
        switch (x) {
        case T::Sphere:
        case T::Box:
        case T::Capsule:
            return true; // every shape has a sphere / box / capsule pair
        case T::Plane:
            return rounded(y) || y == T::ConvexHull;
        case T::ConvexHull:
            return rounded(y) || y == T::Plane || y == T::ConvexHull || y == T::TriMesh;
        case T::TriMesh:
            return rounded(y) || y == T::ConvexHull;
        case T::SdfMesh:
        case T::Voxel:
            return rounded(y);
        }
        return false;
    };
    return one(typeA, typeB) && one(typeB, typeA);
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
                          bodies.positions[pair.bodyA], bodyOrientation(bodies, pair.bodyA), shapes.shapeRef(shapeA)};
    const ShapeInstance b{shapeType(shapes, shapeB), shapes.params[shapeB], shapes.scalars[shapeB],
                          bodies.positions[pair.bodyB], bodyOrientation(bodies, pair.bodyB), shapes.shapeRef(shapeB)};
    return collideShapes(a, b, pair.bodyA, pair.bodyB, margin);
}

u32 dispatchShapePairMultiRaw(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    f32 margin,
    ContactManifold* out,
    u32 maxOut) {
    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
        return 0u;
    }
    const ShapeInstance a{shapeType(shapes, shapeA), shapes.params[shapeA], shapes.scalars[shapeA],
                          bodies.positions[pair.bodyA], bodyOrientation(bodies, pair.bodyA), shapes.shapeRef(shapeA)};
    const ShapeInstance b{shapeType(shapes, shapeB), shapes.params[shapeB], shapes.scalars[shapeB],
                          bodies.positions[pair.bodyB], bodyOrientation(bodies, pair.bodyB), shapes.shapeRef(shapeB)};
    return collideShapesMulti(a, b, pair.bodyA, pair.bodyB, margin, out, maxOut);
}

} // namespace

u32 contact_shape_for_body(const CollisionShapeSoA& shapes, u32 bodyIndex) {
    return findShapeForBody(shapes, bodyIndex, CollisionShapeType::Sphere);
}

ContactManifold collideShapes(const ShapeInstance& shapeA, const ShapeInstance& shapeB, u32 a, u32 b, f32 margin) {
    // Sphere / box / capsule / plane pairs: the shared FUSE_HOST_DEVICE dispatch (the CUDA resident
    // narrowphase runs the same function).
    if (isPrimitiveShape(shapeA.type) && isPrimitiveShape(shapeB.type)) {
        return collidePrimitiveShapes(shapeA, shapeB, a, b, margin);
    }
    // Convex hulls, triangle meshes, SDFs and voxels: CPU only (narrowphase/pooled_dispatch.cpp).
    return collidePooledShapes(shapeA, shapeB, a, b, margin);
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

bool isShapeDegenerate(CollisionShapeType type, const vec3& params, u32 ref) {
    switch (type) {
    case CollisionShapeType::Sphere:
    case CollisionShapeType::Capsule:
        return params.x <= 0.f;
    case CollisionShapeType::Box:
        return params.x <= 0.f || params.y <= 0.f || params.z <= 0.f;
    case CollisionShapeType::Plane:
        return params.length() < 1e-8f;
    case CollisionShapeType::ConvexHull:
    case CollisionShapeType::TriMesh:
    case CollisionShapeType::SdfMesh:
    case CollisionShapeType::Voxel:
        // Pooled geometry (a flat mesh floor has zero y extent): degenerate only without a live shape.
        return ShapePool::global().type(ref) != type || !ShapePool::global().valid(ref);
    }
    return false;
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
    if (isShapeDegenerate(typeA, shapes.params[shapeA], shapes.shapeRef(shapeA)) ||
        isShapeDegenerate(typeB, shapes.params[shapeB], shapes.shapeRef(shapeB))) {
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
    return isShapeDegenerate(typeA, shapes.params[shapeA], shapes.shapeRef(shapeA)) ||
           isShapeDegenerate(typeB, shapes.params[shapeB], shapes.shapeRef(shapeB));
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

    return !shapePairSupported(typeA, typeB);
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

u32 detect_contacts_pair_multi(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    f32 margin,
    ContactManifold* out,
    u32 maxOut) {
    if (out == nullptr || maxOut == 0u || is_invalid_contact_pair(pair, bodies, shapes)) {
        return 0u;
    }
    const u32 count = dispatchShapePairMultiRaw(pair, bodies, shapes, margin, out, maxOut);
    for (u32 i = 0; i < count; ++i) {
        ContactManifold& manifold = out[i];
        const vec3 centres = bodies.positions[manifold.bodyA] - bodies.positions[manifold.bodyB];
        manifold.minSeparation = centres.dot(manifold.contactNormal) + manifold.maxPenetration();
    }
    return count;
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
