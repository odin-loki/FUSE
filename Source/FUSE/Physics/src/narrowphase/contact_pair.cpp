#include <fuse/physics/narrowphase/contact_pair.hpp>

#include <fuse/physics/narrowphase/collision_dispatch.hpp>

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

bool isEmptyPair(const broadphase::CandidatePair& pair, const RigidBodySoA& bodies) {
    if (pair.bodyA == pair.bodyB) {
        return true;
    }
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count()) {
        return true;
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

} // namespace

ContactManifold detect_contacts_pair(
    const broadphase::CandidatePair& pair,
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes) {
    if (isEmptyPair(pair, bodies)) {
        return invalidContactManifold();
    }
    return dispatchShapePair(pair, bodies, shapes);
}

bool generate_contact_manifold(ContactManifold& manifold) {
    if (manifold.empty()) {
        manifold.clear();
        return false;
    }

    manifold.syncLegacyFields();
    compute_friction_tangents(manifold);
    manifold.valid = true;
    return true;
}

void compute_friction_tangents(ContactManifold& manifold) {
    manifold.buildFrictionBasis();
}

} // namespace fuse::physics::narrowphase
