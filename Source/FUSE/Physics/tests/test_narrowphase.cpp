#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/contact_pair.hpp>
#include <fuse/physics/narrowphase/friction.hpp>
#include <fuse/physics/narrowphase/gjk.hpp>
#include <fuse/physics/physics_data.hpp>

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(fuse::physics::f32 actual, fuse::physics::f32 expected, fuse::physics::f32 tolerance, const char* message) {
    if (std::fabs(actual - expected) > tolerance) {
        std::fprintf(stderr, "FAIL: %s (got %.6f expected %.6f)\n", message, actual, expected);
        ++g_failures;
    }
}

void testSphereSphereCollision() {
    const auto manifold = fuse::physics::narrowphase::collideSphereSphere(
        {0.f, 0.f, 0.f},
        1.f,
        {1.5f, 0.f, 0.f},
        1.f,
        0u,
        1u);
    expectTrue(manifold.valid, "overlapping spheres produce contact");
    expectTrue(manifold.penetrationDepth > 0.f, "penetration depth positive");
}

void testSpherePlaneCollision() {
    const auto manifold = fuse::physics::narrowphase::collideSpherePlane(
        {0.f, 0.5f, 0.f},
        1.f,
        {0.f, 1.f, 0.f},
        0.f,
        0u,
        1u);
    expectTrue(manifold.valid, "sphere intersecting ground plane produces contact");
}

void testBoxSphereCollision() {
    const auto manifold = fuse::physics::narrowphase::collideBoxSphere(
        {0.f, 1.2f, 0.f},
        0.5f,
        {0.f, 0.f, 0.f},
        {1.f, 1.f, 1.f},
        0u,
        1u);
    expectTrue(manifold.valid, "sphere resting on box top produces contact");
    expectNear(manifold.penetrationDepth, 0.3f, 1e-4f, "box-sphere penetration depth");
    expectNear(manifold.contactNormal.y, 1.f, 1e-4f, "box-sphere normal points up");
}

void testCapsuleSphereCollision() {
    const auto manifold = fuse::physics::narrowphase::collideCapsuleSphere(
        {0.f, 0.f, 0.7f},
        0.5f,
        {0.f, 0.f, 0.f},
        {0.4f, 1.f, 0.f},
        0u,
        1u);
    expectTrue(manifold.valid, "sphere overlapping capsule hemisphere produces contact");
    expectTrue(manifold.penetrationDepth > 0.f, "capsule-sphere penetration depth positive");
}

void testContactBufferClearReuse() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.reserve(8u);
    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold first{};
    first.valid = true;
    first.bodyA = 0u;
    first.bodyB = 1u;
    first.penetrationDepth = 0.25f;
    buffer.writeSlot(0u, first);
    expectTrue(buffer.compact() == 1u, "compact keeps valid slot");

    buffer.clear();
    expectTrue(buffer.activeCount == 0u, "clear resets active count");
    expectTrue(buffer.pairSlotCount == 0u, "clear resets pair slots");

    buffer.preparePairSlots(4u);
    fuse::physics::narrowphase::ContactManifold second = first;
    second.bodyB = 2u;
    buffer.writeSlot(1u, second);
    buffer.writeSlot(3u, first);
    expectTrue(buffer.compact() == 2u, "reuse after clear compacts new contacts");
}

void testBoxBoxCollisionPointCount() {
    const auto manifold = fuse::physics::narrowphase::collideBoxBox(
        {0.f, 1.1f, 0.f},
        {1.f, 1.f, 1.f},
        {0.f, 0.f, 0.f},
        {1.f, 1.f, 1.f},
        0u,
        1u);
    expectTrue(manifold.valid, "overlapping axis-aligned boxes produce contact");
    expectTrue(manifold.pointCount == 4u, "box-box stub emits four face contact points");
    expectTrue(manifold.penetrationDepth > 0.f, "box-box penetration depth positive");
}

void testFrictionBasisOrthogonality() {
    const fuse::physics::vec3 normals[] = {
        {0.f, 1.f, 0.f},
        {1.f, 0.f, 0.f},
        {0.f, 0.f, 1.f},
        {0.577350269f, 0.577350269f, 0.577350269f},
        {0.267261f, 0.534522f, 0.801784f},
    };

    for (const fuse::physics::vec3& normal : normals) {
        const auto basis = fuse::physics::narrowphase::buildTangentBasis(normal);
        expectTrue(
            fuse::physics::narrowphase::isOrthonormalTangentBasis(normal, basis),
            "friction basis is orthonormal for sampled normals");
    }
}

void testFrictionClampStub() {
    const auto basis = fuse::physics::narrowphase::buildTangentBasis({0.f, 1.f, 0.f});
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis({0.f, 1.f, 0.f}, basis),
        "friction clamp stub uses orthonormal tangent basis");

    fuse::physics::narrowphase::FrictionImpulse withinStatic{};
    withinStatic.tangent1 = 0.4f;
    const auto staticClamped =
        fuse::physics::narrowphase::clampFrictionImpulse(withinStatic, 2.f, 0.5f, 0.3f);
    expectNear(staticClamped.tangent1, 0.4f, 1e-4f, "friction clamp keeps tangent inside static cone");

    fuse::physics::narrowphase::FrictionImpulse beyondStatic{};
    beyondStatic.tangent1 = 5.f;
    const auto dynamicClamped =
        fuse::physics::narrowphase::clampFrictionImpulse(beyondStatic, 2.f, 0.5f, 0.3f);
    expectNear(dynamicClamped.normal, 2.f, 1e-4f, "friction clamp preserves normal impulse");
    expectNear(dynamicClamped.tangent1, 0.6f, 1e-4f, "friction clamp limits tangent to dynamic cone");
}

void testEmptyContacts() {
    const auto separatedSpheres = fuse::physics::narrowphase::collideSphereSphere(
        {0.f, 0.f, 0.f},
        1.f,
        {5.f, 0.f, 0.f},
        1.f,
        0u,
        1u);
    expectTrue(!separatedSpheres.valid, "separated spheres produce no contact");
    expectTrue(separatedSpheres.empty(), "separated sphere manifold has no points");

    const auto separatedBoxes = fuse::physics::narrowphase::collideBoxBox(
        {0.f, 0.f, 0.f},
        {1.f, 1.f, 1.f},
        {5.f, 0.f, 0.f},
        {1.f, 1.f, 1.f},
        0u,
        1u);
    expectTrue(!separatedBoxes.valid, "separated boxes produce no contact");

    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(3u);
    expectTrue(buffer.compact() == 0u, "all-invalid pair slots compact to zero contacts");
    expectTrue(buffer.isEmpty(), "empty buffer reports no active contacts");

    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({10.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{bodyA, bodyB}};
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(pairs, bodies, shapes, buffer);
    expectTrue(buffer.isEmpty(), "narrowphase buffer stays empty for separated pair");
}

void testManifoldFillAndPointCap() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.1f);
    manifold.addPoint({1.f, 0.f, 0.f}, 0.4f);
    manifold.addPoint({2.f, 0.f, 0.f}, 0.2f);
    manifold.addPoint({3.f, 0.f, 0.f}, 0.15f);
    manifold.addPoint({4.f, 0.f, 0.f}, 0.9f);
    expectTrue(manifold.pointCount == fuse::physics::narrowphase::kMaxContactPointsPerManifold,
        "manifold fill caps at max contact points");
    expectNear(manifold.maxPenetration(), 0.4f, 1e-4f, "max penetration tracks deepest point");
    expectNear(manifold.penetrationDepth, 0.4f, 1e-4f, "legacy penetration mirrors deepest point");
    expectNear(manifold.contactPoint.x, 1.f, 1e-4f, "legacy contact point mirrors deepest slot");

    manifold.reset();
    expectTrue(!manifold.valid, "reset clears validity");
    expectTrue(manifold.empty(), "reset clears point slots");
    expectNear(manifold.maxPenetration(), 0.f, 1e-4f, "reset clears penetration");
}

void testManifoldPointAccessAndFrictionBasis() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({1.f, 0.f, 0.f}, 0.2f);
    manifold.addPoint({2.f, 0.f, 0.f}, 0.35f);
    expectNear(manifold.pointAt(0u).penetration, 0.2f, 1e-4f, "pointAt returns first slot");
    expectNear(manifold.pointAt(1u).point.x, 2.f, 1e-4f, "pointAt returns second slot position");

    manifold.buildFrictionBasis();
    expectTrue(
        manifold.hasFrictionBasis(),
        "manifold friction basis is orthonormal after buildFrictionBasis");
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis(
            manifold.contactNormal, manifold.frictionBasis),
        "manifold stores orthonormal friction basis");
}

void testTangentialVelocityProjection() {
    const auto basis = fuse::physics::narrowphase::buildTangentBasis({0.f, 1.f, 0.f});
    const fuse::physics::vec3 relativeVelocity{3.f, 0.f, 4.f};
    const fuse::physics::vec2 projected =
        fuse::physics::narrowphase::projectTangentialVelocity(relativeVelocity, basis);
    expectNear(
        projected.x,
        relativeVelocity.dot(basis.tangent1),
        1e-4f,
        "tangential projection along tangent1");
    expectNear(
        projected.y,
        relativeVelocity.dot(basis.tangent2),
        1e-4f,
        "tangential projection along tangent2");

    const fuse::physics::vec3 reconstructed =
        basis.tangent1 * projected.x + basis.tangent2 * projected.y;
    expectNear(reconstructed.x, relativeVelocity.x, 1e-4f, "tangential projection reconstructs X");
    expectNear(reconstructed.y, relativeVelocity.y, 1e-4f, "tangential projection reconstructs Y");
    expectNear(reconstructed.z, relativeVelocity.z, 1e-4f, "tangential projection reconstructs Z");
}

void testContactBufferFrictionTangentSoA() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 0.f, 1.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.1f);
    manifold.buildFrictionBasis();
    buffer.writeSlot(0u, manifold);

    fuse::physics::narrowphase::ContactManifold second{};
    second.valid = true;
    second.bodyA = 2u;
    second.bodyB = 3u;
    second.contactNormal = {1.f, 0.f, 0.f};
    second.addPoint({0.f, 0.f, 0.f}, 0.2f);
    buffer.writeSlot(1u, second);

    expectTrue(buffer.compact() == 2u, "friction tangent SoA compacts two manifolds");
    const auto firstBasis = buffer.tangentBasisAt(0u);
    const auto secondBasis = buffer.tangentBasisAt(1u);
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis({0.f, 0.f, 1.f}, firstBasis),
        "buffer tangent SoA slot zero is orthonormal");
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis({1.f, 0.f, 0.f}, secondBasis),
        "buffer tangent SoA slot one is orthonormal");

    fuse::physics::narrowphase::ContactManifold restored = buffer.manifoldAt(0u);
    expectTrue(restored.hasFrictionBasis(), "manifoldAt restores friction basis");
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis(
            restored.contactNormal, restored.frictionBasis),
        "restored manifold friction basis matches normal");

    buffer.buildFrictionTangentBases();
    const auto rebuilt = buffer.tangentBasisAt(1u);
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis({1.f, 0.f, 0.f}, rebuilt),
        "buildFrictionTangentBases rebuilds orthonormal frames");
}

void testContactBufferWarmStartAndPointSlots() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(1u);

    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.warmNormalImpulse = 3.5f;
    manifold.warmTangentImpulse = {0.25f, -0.1f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    manifold.addPoint({1.f, 0.f, 0.f}, 0.25f);
    buffer.writeSlot(0u, manifold);
    expectTrue(buffer.compact() == 1u, "warm-start buffer compacts one manifold");

    fuse::physics::narrowphase::ContactManifold restored = buffer.manifoldAt(0u);
    expectTrue(restored.pointCount == 2u, "buffer restores multi-point slots");
    expectNear(restored.warmNormalImpulse, 3.5f, 1e-4f, "buffer stores warm normal impulse");
    expectNear(restored.warmTangentImpulse.x, 0.25f, 1e-4f, "buffer stores warm tangent impulse");

    fuse::physics::narrowphase::ContactManifold warmStartTarget{};
    buffer.applyWarmStartStub(0u, warmStartTarget);
    expectNear(warmStartTarget.warmNormalImpulse, 3.5f, 1e-4f, "warm-start stub copies prior impulses");
}

void testRunNarrowphaseIntoBufferJobSafe() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    const fuse::u32 boxBodyA = bodies.addBody({0.f, 1.1f, 0.f}, 1.f);
    const fuse::u32 boxBodyB = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Box, boxBodyA, {1.f, 1.f, 1.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Box, boxBodyB, {1.f, 1.f, 1.f});

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {
        {boxBodyA, boxBodyB},
        {boxBodyB, boxBodyA},
    };

    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.reserve(2u);
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(pairs, bodies, shapes, buffer);

    expectTrue(buffer.activeCount == 2u, "job-safe narrowphase fills one slot per pair index");
    const auto first = buffer.manifoldAt(0u);
    const auto second = buffer.manifoldAt(1u);
    expectTrue(first.valid && second.valid, "both pair slots produce valid manifolds");
    expectTrue(first.pointCount == 4u, "box-box dispatch preserves four contact points");
    expectTrue(first.bodyA == boxBodyA && first.bodyB == boxBodyB, "slot zero preserves pair order");
    expectTrue(second.bodyA == boxBodyB && second.bodyB == boxBodyA, "slot one preserves swapped pair order");
}

void testContactPairRejectReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 triggerA = bodies.addBody({2.f, 0.f, 0.f}, 0.f, fuse::physics::RB_TRIGGER);
    const fuse::u32 triggerB = bodies.addBody({2.5f, 0.f, 0.f}, 0.f, fuse::physics::RB_TRIGGER);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, triggerA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, triggerB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({bodyA, bodyA}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::SelfPair,
        "reject reason flags self pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({bodyA, 99u}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::OutOfRangeBody,
        "reject reason flags out-of-range body");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({triggerA, triggerB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothTriggers,
        "reject reason flags both-trigger pair");
    expectTrue(
        fuse::physics::narrowphase::is_trigger_contact_pair({triggerA, triggerB}, bodies),
        "trigger guard detects both-trigger pair");
    expectTrue(
        !fuse::physics::narrowphase::is_trigger_contact_pair({bodyA, triggerA}, bodies),
        "trigger guard allows mixed trigger/dynamic pair");

    const fuse::u32 hullBody = bodies.addBody({5.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::ConvexHull, hullBody, {1.f, 0.f, 0.f});
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({bodyA, hullBody}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedShapePair,
        "reject reason flags unsupported convex hull pair");
    expectTrue(
        fuse::physics::narrowphase::is_unsupported_shape_pair({bodyA, hullBody}, shapes),
        "unsupported shape guard flags convex hull dispatch gap");

    const auto triggerPair =
        fuse::physics::narrowphase::detect_contacts_pair({triggerA, triggerB}, bodies, shapes);
    expectTrue(!triggerPair.valid, "both-trigger pair returns invalid manifold");
    expectTrue(triggerPair.empty(), "both-trigger pair has no contact points");
}

void testDetectContactsPairEmptyGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyNoShape = bodies.addBody({2.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::is_invalid_contact_pair({bodyA, bodyA}, bodies, shapes),
        "invalid guard flags self pair");
    expectTrue(
        fuse::physics::narrowphase::is_invalid_contact_pair({bodyA, 99u}, bodies, shapes),
        "invalid guard flags out-of-range body");
    expectTrue(
        fuse::physics::narrowphase::is_invalid_contact_pair({bodyA, bodyNoShape}, bodies, shapes),
        "invalid guard flags missing shape");

    const auto selfPair = fuse::physics::narrowphase::detect_contacts_pair({bodyA, bodyA}, bodies, shapes);
    expectTrue(!selfPair.valid, "self pair returns invalid manifold");
    expectTrue(selfPair.empty(), "self pair has no contact points");

    const auto missingPair = fuse::physics::narrowphase::detect_contacts_pair({bodyA, 99u}, bodies, shapes);
    expectTrue(!missingPair.valid, "out-of-range pair returns invalid manifold");

    const auto missingShapePair =
        fuse::physics::narrowphase::detect_contacts_pair({bodyA, bodyNoShape}, bodies, shapes);
    expectTrue(!missingShapePair.valid, "missing shape pair returns invalid manifold");
    expectTrue(missingShapePair.empty(), "missing shape pair has no contact points");

    const fuse::u32 bodySeparated = bodies.addBody({10.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodySeparated, {1.f, 0.f, 0.f});
    const auto separated =
        fuse::physics::narrowphase::detect_contacts_pair({bodyA, bodySeparated}, bodies, shapes);
    expectTrue(!separated.valid, "separated pair returns invalid manifold from detect");

    const auto overlap = fuse::physics::narrowphase::detect_contacts_pair({bodyA, bodyB}, bodies, shapes);
    expectTrue(overlap.valid, "valid pair detects contact");
    expectTrue(overlap.pointCount > 0u, "valid pair populates contact points");
}

void testGenerateContactManifoldAndFrictionTangents() {
    fuse::physics::narrowphase::ContactManifold empty{};
    empty.valid = true;
    expectTrue(!fuse::physics::narrowphase::generate_contact_manifold(empty), "empty manifold generation fails");
    expectTrue(!empty.valid, "failed generation clears validity");
    expectTrue(empty.empty(), "failed generation clears points");

    fuse::physics::narrowphase::ContactManifold degenerateNormal{};
    degenerateNormal.valid = true;
    degenerateNormal.contactNormal = {};
    degenerateNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        !fuse::physics::narrowphase::generate_contact_manifold(degenerateNormal),
        "generate rejects zero-length contact normal");
    expectTrue(!degenerateNormal.valid, "degenerate normal clears validity");

    fuse::physics::narrowphase::ContactManifold nonPenetrating{};
    nonPenetrating.valid = true;
    nonPenetrating.contactNormal = {0.f, 1.f, 0.f};
    nonPenetrating.addPoint({0.f, 0.f, 0.f}, -0.05f);
    nonPenetrating.addPoint({1.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        !fuse::physics::narrowphase::generate_contact_manifold(nonPenetrating),
        "generate rejects all non-penetrating points");
    expectTrue(nonPenetrating.empty(), "non-penetrating prune leaves manifold empty");

    fuse::physics::narrowphase::ContactManifold manifold =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(manifold.valid, "detected sphere pair is valid before finalize");
    expectTrue(!manifold.hasFrictionBasis(), "raw detection omits friction basis");

    expectTrue(fuse::physics::narrowphase::generate_contact_manifold(manifold), "generate finalizes manifold");
    expectTrue(manifold.hasFrictionBasis(), "generate builds friction basis");
    expectNear(manifold.contactNormal.length(), 1.f, 1e-4f, "generate normalizes contact normal");
    fuse::physics::narrowphase::compute_friction_tangents(manifold);
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis(
            manifold.contactNormal, manifold.frictionBasis),
        "compute_friction_tangents stores orthonormal basis");
}

void testManifoldPruneNonPenetratingPoints() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.3f);
    manifold.addPoint({1.f, 0.f, 0.f}, 0.f);
    manifold.addPoint({2.f, 0.f, 0.f}, 0.15f);
    manifold.addPoint({3.f, 0.f, 0.f}, -0.05f);

    manifold.pruneNonPenetratingPoints();
    expectTrue(manifold.pointCount == 3u, "prune keeps touching and penetrating points");
    expectNear(manifold.maxPenetration(), 0.3f, 1e-4f, "prune preserves deepest penetration");
    expectNear(manifold.penetrationDepth, 0.3f, 1e-4f, "prune syncs legacy penetration depth");
}

void testManifoldPruneHelpers() {
    fuse::physics::narrowphase::ContactManifold capped{};
    capped.contactNormal = {0.f, 1.f, 0.f};
    capped.addPoint({0.f, 0.f, 0.f}, 0.1f);
    capped.addPoint({1.f, 0.f, 0.f}, 0.5f);
    capped.addPoint({2.f, 0.f, 0.f}, 0.3f);
    capped.addPoint({3.f, 0.f, 0.f}, 0.9f);
    capped.pruneToMaxPoints(2u);
    expectTrue(capped.pointCount == 2u, "pruneToMaxPoints keeps deepest two slots");
    expectNear(capped.maxPenetration(), 0.9f, 1e-4f, "pruneToMaxPoints retains max penetration");
    expectNear(capped.penetrationDepth, 0.9f, 1e-4f, "pruneToMaxPoints syncs legacy depth");

    fuse::physics::narrowphase::ContactManifold duplicates{};
    duplicates.contactNormal = {0.f, 1.f, 0.f};
    duplicates.addPoint({0.f, 0.f, 0.f}, 0.2f);
    duplicates.addPoint({0.00001f, 0.f, 0.f}, 0.35f);
    duplicates.addPoint({1.f, 0.f, 0.f}, 0.1f);
    duplicates.pruneDuplicatePoints(1e-3f);
    expectTrue(duplicates.pointCount == 2u, "pruneDuplicatePoints merges near-identical points");
    expectNear(duplicates.maxPenetration(), 0.35f, 1e-4f, "pruneDuplicatePoints keeps deeper duplicate");

    fuse::physics::narrowphase::ContactManifold chained{};
    chained.contactNormal = {0.f, 1.f, 0.f};
    chained.addPoint({0.f, 0.f, 0.f}, 0.4f);
    chained.addPoint({0.f, 0.f, 0.f}, 0.5f);
    chained.addPoint({1.f, 0.f, 0.f}, -0.1f);
    chained.addPoint({2.f, 0.f, 0.f}, 0.05f);
    chained.pruneContactPoints();
    expectTrue(chained.pointCount == 2u, "pruneContactPoints chains separation and duplicate pruning");
    expectNear(chained.maxPenetration(), 0.5f, 1e-4f, "pruneContactPoints keeps deepest merged point");
}

void testFrictionTangentEarlyOuts() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_tangents(empty),
        "friction early-out skips empty manifold");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_tangents(noNormal),
        "friction early-out skips zero-length normal");

    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_solve(0.f, 0.f, 1.f),
        "friction solve early-out skips zero coefficients");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_solve(0.5f, 0.3f, 0.f),
        "friction solve early-out skips negligible normal impulse");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_friction_solve(0.5f, 0.3f, 0.25f),
        "friction solve proceeds with coefficients and impulse");

    const fuse::physics::vec2 slow{1e-8f, 1e-8f};
    const fuse::physics::vec2 fast{0.3f, 0.4f};
    expectTrue(
        fuse::physics::narrowphase::hasNegligibleTangentialVelocity(slow),
        "tangential early-out flags negligible speed");
    expectTrue(
        !fuse::physics::narrowphase::hasNegligibleTangentialVelocity(fast),
        "tangential early-out allows meaningful slip");

    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    fuse::physics::narrowphase::compute_friction_tangents(manifold);
    expectTrue(manifold.hasFrictionBasis(), "first friction tangent build succeeds");

    const auto cachedBasis = manifold.frictionBasis;
    fuse::physics::narrowphase::compute_friction_tangents(manifold);
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis(
            manifold.contactNormal, manifold.frictionBasis),
        "second friction tangent build early-outs with valid basis");
    expectNear(manifold.frictionBasis.tangent1.x, cachedBasis.tangent1.x, 1e-4f,
        "friction early-out preserves cached tangent1");
}

void testRunNarrowphaseFinalizesFrictionTangents() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{bodyA, bodyB}};
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(pairs, bodies, shapes, buffer);

    expectTrue(buffer.activeCount == 1u, "narrowphase produces one finalized contact");
    const auto restored = buffer.manifoldAt(0u);
    expectTrue(restored.hasFrictionBasis(), "narrowphase buffer stores finalized friction basis");
    expectNear(restored.contactNormal.length(), 1.f, 1e-4f, "narrowphase buffer stores unit contact normal");
}

void testContactPointTangentBasisAndManifoldClear() {
    fuse::physics::narrowphase::ContactPoint point{};
    point.point = {1.f, 0.f, 0.f};
    point.penetration = 0.25f;

    const fuse::physics::vec3 normal{0.f, 1.f, 0.f};
    const auto basis = point.tangent_basis(normal);
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis(normal, basis),
        "ContactPoint tangent_basis is orthonormal");

    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.contactNormal = normal;
    manifold.addPoint(point.point, point.penetration);
    manifold.clear();
    expectTrue(!manifold.valid, "clear resets validity");
    expectTrue(manifold.empty(), "clear removes contact points");
}

void testContactBufferCapacityClamp() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.setMaxCapacity(2u);
    buffer.preparePairSlots(4u);

    fuse::physics::narrowphase::ContactManifold shallow{};
    shallow.valid = true;
    shallow.bodyA = 0u;
    shallow.bodyB = 1u;
    shallow.contactNormal = {0.f, 1.f, 0.f};
    shallow.penetrationDepth = 0.1f;
    shallow.addPoint({0.f, 0.f, 0.f}, 0.1f);

    fuse::physics::narrowphase::ContactManifold deep = shallow;
    deep.bodyB = 2u;
    deep.penetrationDepth = 0.9f;
    deep.points[0].penetration = 0.9f;

    fuse::physics::narrowphase::ContactManifold medium = shallow;
    medium.bodyB = 3u;
    medium.penetrationDepth = 0.5f;
    medium.points[0].penetration = 0.5f;

    fuse::physics::narrowphase::ContactManifold selfPair = shallow;
    selfPair.bodyB = selfPair.bodyA;

    buffer.writeSlot(0u, shallow);
    buffer.writeSlot(1u, deep);
    buffer.writeSlot(2u, medium);
    buffer.writeSlot(3u, selfPair);

    expectTrue(buffer.compactAndClamp() == 2u, "compactAndClamp ignores self pair and truncates to max capacity");
    expectTrue(buffer.droppedCount == 1u, "dropped count tracks truncated contacts");
    expectNear(buffer.manifoldAt(0u).penetrationDepth, 0.9f, 1e-4f, "clamp keeps deepest penetration");
    expectNear(buffer.manifoldAt(1u).penetrationDepth, 0.5f, 1e-4f, "clamp keeps next deepest penetration");
}

void testContactPairDeepenGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 staticA = bodies.addBody({0.f, 2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 staticB = bodies.addBody({0.f, 3.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, staticA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, staticB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::is_valid_contact_pair({dynamicA, dynamicB}, bodies, shapes),
        "valid pair passes is_valid_contact_pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({staticA, staticB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothStatic,
        "reject reason flags both-static pair");
    expectTrue(
        fuse::physics::narrowphase::is_static_contact_pair({staticA, staticB}, bodies),
        "static guard detects both-static pair");
    expectTrue(
        !fuse::physics::narrowphase::is_static_contact_pair({dynamicA, staticA}, bodies),
        "static guard allows dynamic/static mix");

    const fuse::u32 zeroRadiusBody = bodies.addBody({5.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, zeroRadiusBody, {0.f, 0.f, 0.f});
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({dynamicA, zeroRadiusBody}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::DegenerateShape,
        "reject reason flags zero-radius sphere");
    expectTrue(
        fuse::physics::narrowphase::is_degenerate_shape_pair({dynamicA, zeroRadiusBody}, shapes),
        "degenerate guard flags zero-radius shape");

    const auto staticPair =
        fuse::physics::narrowphase::detect_contacts_pair({staticA, staticB}, bodies, shapes);
    expectTrue(!staticPair.valid, "both-static pair returns invalid manifold");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::BothStatic),
            "BothStatic") == 0,
        "reject reason name resolves BothStatic");
}

void testManifoldPruneDeepenHelpers() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.5f);
    manifold.addPoint({1.f, 0.f, 0.f}, 0.01f);
    manifold.addPoint({2.f, 0.f, 0.f}, -0.1f);

    expectTrue(manifold.hasPenetratingPoints(), "hasPenetratingPoints true with mixed depths");
    expectTrue(manifold.countPenetratingPoints() == 2u, "countPenetratingPoints excludes separated");
    expectTrue(manifold.needsPruning(), "needsPruning true with separated and duplicate candidates");

    manifold.pruneShallowPenetrations(0.05f);
    expectTrue(manifold.pointCount == 1u, "pruneShallowPenetrations keeps deep point");
    expectNear(manifold.maxPenetration(), 0.5f, 1e-4f, "shallow prune retains deepest penetration");

    fuse::physics::narrowphase::ContactManifold emptyAfterPrune{};
    emptyAfterPrune.contactNormal = {0.f, 1.f, 0.f};
    emptyAfterPrune.addPoint({0.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        !emptyAfterPrune.pruneIfEmpty(),
        "pruneIfEmpty returns false when all points separated");
    expectTrue(emptyAfterPrune.empty(), "pruneIfEmpty clears separated manifold");

    fuse::physics::narrowphase::ContactManifold survives{};
    survives.contactNormal = {0.f, 1.f, 0.f};
    survives.addPoint({0.f, 0.f, 0.f}, 0.3f);
    expectTrue(survives.pruneIfEmpty(), "pruneIfEmpty returns true when points remain");
    expectTrue(survives.pointCount == 1u, "pruneIfEmpty keeps penetrating point");
}

void testFrictionTangentDeepenEarlyOuts() {
    fuse::physics::narrowphase::ContactManifold withBasis{};
    withBasis.contactNormal = {0.f, 1.f, 0.f};
    withBasis.addPoint({0.f, 0.f, 0.f}, 0.2f);
    withBasis.buildFrictionBasis();
    expectTrue(
        fuse::physics::narrowphase::has_cached_friction_basis(withBasis),
        "cached basis detected after build");

    const auto cachedTangent1 = withBasis.frictionBasis.tangent1;
    fuse::physics::narrowphase::compute_friction_tangents(withBasis);
    expectNear(
        withBasis.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "cached basis early-out preserves tangent1");

    const fuse::physics::vec2 slip{0.3f, 0.4f};
    expectNear(
        fuse::physics::narrowphase::tangentialSpeed(slip),
        0.5f,
        1e-4f,
        "tangentialSpeed computes magnitude");

    expectTrue(
        fuse::physics::narrowphase::should_skip_tangential_velocity_solve(
            {1e-9f, 1e-9f}, 0.5f, 0.3f, 1.f),
        "combined early-out skips negligible slip with friction");
    expectTrue(
        fuse::physics::narrowphase::should_skip_tangential_velocity_solve(
            slip, 0.f, 0.f, 1.f),
        "combined early-out skips zero friction coefficients");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_tangential_velocity_solve(
            slip, 0.5f, 0.3f, 0.5f),
        "combined early-out proceeds with slip and friction");
}

void testContactPairRejectGuardHelpers() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyNoShape = bodies.addBody({2.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::is_self_contact_pair({bodyA, bodyA}),
        "self guard flags identical body indices");
    expectTrue(
        !fuse::physics::narrowphase::is_self_contact_pair({bodyA, bodyB}),
        "self guard allows distinct bodies");
    expectTrue(
        fuse::physics::narrowphase::is_out_of_range_contact_pair({bodyA, 99u}, bodies),
        "out-of-range guard flags invalid body index");
    expectTrue(
        !fuse::physics::narrowphase::is_out_of_range_contact_pair({bodyA, bodyB}, bodies),
        "out-of-range guard allows in-range pair");
    expectTrue(
        fuse::physics::narrowphase::is_missing_shape_contact_pair({bodyA, bodyNoShape}, shapes),
        "missing-shape guard flags body without shape");
    expectTrue(
        !fuse::physics::narrowphase::is_missing_shape_contact_pair({bodyA, bodyB}, shapes),
        "missing-shape guard allows fully shaped pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_rejects_for_reason(
            {bodyA, bodyA},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::SelfPair),
        "rejects_for_reason matches self pair");
    expectTrue(
        !fuse::physics::narrowphase::contact_pair_rejects_for_reason(
            {bodyA, bodyB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::SelfPair),
        "rejects_for_reason does not false-positive valid pair");
}

void testCanFinalizeContactManifoldGuard() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::can_finalize_contact_manifold(empty),
        "can_finalize rejects empty manifold");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        !fuse::physics::narrowphase::can_finalize_contact_manifold(noNormal),
        "can_finalize rejects zero-length normal");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        !fuse::physics::narrowphase::can_finalize_contact_manifold(separated),
        "can_finalize rejects all-separated points");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    expectTrue(
        fuse::physics::narrowphase::can_finalize_contact_manifold(ready),
        "can_finalize accepts penetrating manifold with valid normal");
}

void testManifoldPrunePreflightGuards() {
    fuse::physics::narrowphase::ContactManifold mixed{};
    mixed.contactNormal = {0.f, 1.f, 0.f};
    mixed.addPoint({0.f, 0.f, 0.f}, 0.4f);
    mixed.addPoint({1.f, 0.f, 0.f}, -0.2f);
    mixed.addPoint({0.00001f, 0.f, 0.f}, 0.35f);

    expectTrue(mixed.hasSeparatedPoints(), "hasSeparatedPoints flags separated slot");
    expectTrue(mixed.hasDuplicatePoints(1e-3f), "hasDuplicatePoints flags near-identical slots");
    expectTrue(!mixed.wouldBeEmptyAfterPrune(), "wouldBeEmptyAfterPrune false when penetrating remain");

    fuse::physics::narrowphase::ContactManifold allSeparated{};
    allSeparated.contactNormal = {0.f, 1.f, 0.f};
    allSeparated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    allSeparated.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        allSeparated.wouldBeEmptyAfterPrune(),
        "wouldBeEmptyAfterPrune true when all points separated");

    fuse::physics::narrowphase::ContactManifold staleValid{};
    staleValid.valid = true;
    staleValid.contactNormal = {0.f, 1.f, 0.f};
    staleValid.invalidateIfEmpty();
    expectTrue(!staleValid.valid, "invalidateIfEmpty clears validity on empty manifold");
}

void testManifoldPruneSkipGuards() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    clean.addPoint({1.f, 0.f, 0.f}, 0.25f);

    expectTrue(clean.canSkipPruneNonPenetrating(), "canSkipPruneNonPenetrating true without separated slots");
    expectTrue(clean.canSkipPruneDuplicates(), "canSkipPruneDuplicates true without duplicate slots");
    expectTrue(clean.canSkipPruneToMaxPoints(4u), "canSkipPruneToMaxPoints true within cap");
    expectTrue(clean.canSkipPruneContactPoints(), "canSkipPruneContactPoints true when prune is no-op");
    expectTrue(clean.pruneContactPointsIfNeeded(), "pruneContactPointsIfNeeded preserves clean manifold");
    expectTrue(clean.pointCount == 2u, "conditional prune leaves clean slots untouched");

    const auto cleanPreflight = fuse::physics::narrowphase::preflight_manifold_prune(clean);
    expectTrue(!cleanPreflight.needs_pruning(), "preflight skips clean manifold");
    expectTrue(!cleanPreflight.can_prune_in_place(), "preflight cannot prune clean manifold");

    fuse::physics::narrowphase::ContactManifold shallow{};
    shallow.contactNormal = {0.f, 1.f, 0.f};
    shallow.addPoint({0.f, 0.f, 0.f}, 0.5f);
    shallow.addPoint({1.f, 0.f, 0.f}, 0.01f);
    expectTrue(shallow.hasShallowPenetrations(0.05f), "hasShallowPenetrations flags shallow slot");
    shallow.pruneShallowPenetrations(0.05f);
    expectTrue(shallow.pointCount == 1u, "shallow prune keeps deep point");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    dirty.addPoint({0.00001f, 0.f, 0.f}, 0.35f);
    expectTrue(!dirty.canSkipPruneContactPoints(), "canSkipPruneContactPoints false when separated/duplicate");
    expectTrue(dirty.pruneContactPointsIfNeeded(), "conditional prune keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "conditional prune removes separated and duplicate slots");
    expectNear(dirty.maxPenetration(), 0.4f, 1e-4f, "conditional prune keeps deepest merged point");

    const auto dirtyPreflight = fuse::physics::narrowphase::preflight_manifold_prune(dirty);
    expectTrue(!dirtyPreflight.needs_pruning(), "preflight marks already-pruned manifold as clean");
}

void testContactPairPreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    const auto validPreflight =
        fuse::physics::narrowphase::preflight_contact_pair({bodyA, bodyB}, bodies, shapes);
    expectTrue(validPreflight.can_dispatch(), "preflight allows valid pair dispatch");
    expectTrue(
        validPreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::None,
        "preflight reports None for valid pair");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_contact_pair_dispatch({bodyA, bodyB}, bodies, shapes),
        "skip guard allows valid pair");

    const auto selfPreflight =
        fuse::physics::narrowphase::preflight_contact_pair({bodyA, bodyA}, bodies, shapes);
    expectTrue(!selfPreflight.can_dispatch(), "preflight rejects self pair");
    expectTrue(selfPreflight.rejected, "preflight marks self pair rejected");
    expectTrue(
        fuse::physics::narrowphase::should_skip_contact_pair_dispatch({bodyA, bodyA}, bodies, shapes),
        "skip guard rejects self pair");
}

void testFrictionBasisGuardHelpers() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::needs_friction_basis_rebuild(empty),
        "needs_friction_basis_rebuild false for empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::needs_friction_basis_rebuild(needsBuild),
        "needs_friction_basis_rebuild true without cached basis");

    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(needsBuild),
        "ensure_friction_basis builds orthonormal frame");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(needsBuild),
        "friction_basis_matches_normal true after ensure");

    const auto cachedTangent1 = needsBuild.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(needsBuild),
        "ensure_friction_basis reuses cached basis");
    expectNear(
        needsBuild.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "ensure_friction_basis preserves cached tangent1");

    fuse::physics::narrowphase::invalidate_friction_basis(needsBuild);
    expectTrue(
        !fuse::physics::narrowphase::has_cached_friction_basis(needsBuild),
        "invalidate_friction_basis clears cached frame");
    expectTrue(
        fuse::physics::narrowphase::needs_friction_basis_rebuild(needsBuild),
        "needs_friction_basis_rebuild true after invalidate");

    fuse::physics::narrowphase::ContactManifold skip{};
    skip.addPoint({0.f, 0.f, 0.f}, 0.1f);
    expectTrue(
        !fuse::physics::narrowphase::ensure_friction_basis(skip),
        "ensure_friction_basis returns false when tangents should be skipped");
}

void testFrictionBasisRefreshGuards() {
    fuse::physics::narrowphase::ContactManifold stale{};
    stale.contactNormal = {0.f, 1.f, 0.f};
    stale.addPoint({0.f, 0.f, 0.f}, 0.2f);
    stale.buildFrictionBasis();
    stale.contactNormal = {1.f, 0.f, 0.f};

    expectTrue(
        fuse::physics::narrowphase::friction_basis_is_stale(stale),
        "stale guard flags basis built for prior normal");
    expectTrue(
        fuse::physics::narrowphase::needs_friction_basis_refresh(stale),
        "refresh guard true when cached basis is stale");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_friction_basis_rebuild(stale),
        "skip rebuild false for stale basis");

    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_if_needed(stale),
        "rebuild refreshes stale basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(stale),
        "rebuild produces basis matching current normal");
    expectTrue(
        fuse::physics::narrowphase::can_skip_friction_basis_rebuild(stale),
        "skip rebuild true after refresh");

    fuse::physics::narrowphase::ContactManifold fresh{};
    fresh.contactNormal = {1.f, 0.f, 0.f};
    fresh.addPoint({0.f, 0.f, 0.f}, 0.1f);
    fuse::physics::narrowphase::compute_friction_tangents_if_needed(fresh);
    expectTrue(fresh.hasFrictionBasis(), "compute_if_needed builds missing basis");

    const auto cachedTangent1 = fresh.frictionBasis.tangent1;
    fuse::physics::narrowphase::compute_friction_tangents_if_needed(fresh);
    expectNear(
        fresh.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "compute_if_needed preserves valid cached basis");
}

void testContactPairDeepenRejectGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 kinematicA = bodies.addBody({0.f, 4.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    const fuse::u32 kinematicB = bodies.addBody({0.f, 5.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, kinematicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, kinematicB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::is_sleeping_contact_pair({sleepingA, sleepingB}, bodies),
        "sleeping guard detects both-sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::is_sleeping_contact_pair({dynamicA, sleepingA}, bodies),
        "sleeping guard allows dynamic/sleeping mix");
    expectTrue(
        fuse::physics::narrowphase::is_kinematic_contact_pair({kinematicA, kinematicB}, bodies),
        "kinematic guard detects both-kinematic pair");
    expectTrue(
        !fuse::physics::narrowphase::is_kinematic_contact_pair({dynamicA, kinematicA}, bodies),
        "kinematic guard allows dynamic/kinematic mix");

    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({sleepingA, sleepingB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping,
        "deepen reject reason flags both-sleeping pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({kinematicA, kinematicB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothKinematic,
        "deepen reject reason flags both-kinematic pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({sleepingA, sleepingB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::None,
        "base reject reason unchanged for sleeping pair");

    const auto sleepingPreflight =
        fuse::physics::narrowphase::preflight_contact_pair_deepen({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingPreflight.can_dispatch(), "deepen preflight rejects both-sleeping pair");
    expectTrue(
        sleepingPreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping,
        "deepen preflight reports BothSleeping");
    expectTrue(
        fuse::physics::narrowphase::should_skip_contact_pair_deepen_dispatch({sleepingA, sleepingB}, bodies, shapes),
        "deepen skip guard rejects both-sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_contact_pair_deepen_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "deepen skip guard allows valid pair");

    const fuse::u32 capsuleBody = bodies.addBody({6.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Capsule, capsuleBody, {0.5f, 0.f, 0.f});
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({dynamicA, capsuleBody}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::DegenerateShape,
        "deepen reject reason flags zero half-height capsule");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({dynamicA, capsuleBody}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::None,
        "base reject reason unchanged for zero half-height capsule");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
            "BothSleeping") == 0,
        "reject reason name resolves BothSleeping");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::BothKinematic),
            "BothKinematic") == 0,
        "reject reason name resolves BothKinematic");
}

void testCanSkipNarrowphaseGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::can_skip_narrowphase({}, bodies, shapes),
        "can_skip_narrowphase on empty pair list");
    expectTrue(
        fuse::physics::narrowphase::can_skip_narrowphase({{sleepingA, sleepingB}}, bodies, shapes),
        "can_skip_narrowphase when all pairs are deepen-rejected");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_narrowphase({{bodyA, bodyB}}, bodies, shapes),
        "can_skip_narrowphase false when dispatchable pair exists");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_narrowphase(
            {{sleepingA, sleepingB}, {bodyA, bodyB}}, bodies, shapes),
        "can_skip_narrowphase false when mixed rejected and dispatchable pairs");
}

void testManifoldFinalizePreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(empty);
    expectTrue(emptyPreflight.skipped, "finalize preflight skips empty manifold");
    expectTrue(!emptyPreflight.can_finalize(), "finalize preflight cannot finalize empty manifold");
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_finalize(empty),
        "can_skip_manifold_finalize true for empty manifold");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    const auto readyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(!readyPreflight.skipped, "finalize preflight does not skip ready manifold");
    expectTrue(readyPreflight.can_finalize(), "finalize preflight can finalize penetrating manifold");
    expectTrue(!readyPreflight.needsPruning, "finalize preflight reports no pruning for clean manifold");
    expectTrue(readyPreflight.needsFrictionBasis, "finalize preflight needs friction basis");
    expectTrue(!readyPreflight.canReuseFrictionBasis, "finalize preflight cannot reuse missing basis");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_manifold_finalize(ready),
        "can_skip_manifold_finalize false for ready manifold");

    fuse::physics::narrowphase::ContactManifold withBasis = ready;
    withBasis.buildFrictionBasis();
    const auto reusePreflight = fuse::physics::narrowphase::preflight_manifold_finalize(withBasis);
    expectTrue(reusePreflight.canReuseFrictionBasis, "finalize preflight can reuse valid basis");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    const auto separatedPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(separated);
    expectTrue(!separatedPreflight.can_finalize(), "finalize preflight rejects separated manifold");
    expectTrue(separatedPreflight.wouldBeEmptyAfterPrune, "finalize preflight flags empty-after-prune");
}

void testGenerateContactManifoldIfNeededGuard() {
    fuse::physics::narrowphase::ContactManifold empty{};
    empty.valid = true;
    expectTrue(
        !fuse::physics::narrowphase::generate_contact_manifold_if_needed(empty),
        "generate_if_needed no-ops on empty manifold");
    expectTrue(empty.valid, "generate_if_needed leaves empty manifold validity unchanged");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.valid = true;
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        !fuse::physics::narrowphase::generate_contact_manifold_if_needed(separated),
        "generate_if_needed no-ops on non-finalizable manifold");
    expectTrue(separated.valid, "generate_if_needed leaves separated manifold validity unchanged");

    fuse::physics::narrowphase::ContactManifold manifold =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_if_needed(manifold),
        "generate_if_needed finalizes valid manifold");
    expectTrue(manifold.valid, "generate_if_needed sets validity on success");
    expectTrue(manifold.hasFrictionBasis(), "generate_if_needed builds friction basis");
}

void testManifoldShallowPruneSkipGuards() {
    fuse::physics::narrowphase::ContactManifold shallow{};
    shallow.contactNormal = {0.f, 1.f, 0.f};
    shallow.addPoint({0.f, 0.f, 0.f}, 0.5f);
    shallow.addPoint({1.f, 0.f, 0.f}, 0.01f);

    expectTrue(!shallow.canSkipPruneShallowPenetrations(0.05f), "canSkipShallow false with shallow slot");
    expectTrue(shallow.pruneShallowPenetrationsIfNeeded(0.05f), "shallow prune if needed keeps deep point");
    expectTrue(shallow.pointCount == 1u, "shallow prune if needed removes shallow slot");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.5f);
    expectTrue(clean.canSkipPruneShallowPenetrations(0.05f), "canSkipShallow true without shallow slots");
    expectTrue(clean.pruneShallowPenetrationsIfNeeded(0.05f), "shallow prune if needed no-ops on clean manifold");
    expectTrue(clean.pointCount == 1u, "shallow prune if needed preserves clean slots");

    const auto shallowPreflight =
        fuse::physics::narrowphase::preflight_manifold_prune(shallow, 1e-6f, 1e-4f, 0.05f);
    expectTrue(!shallowPreflight.needs_shallow_pruning(0.05f), "shallow preflight false after shallow prune");
}

void testFrictionBasisPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(empty);
    expectTrue(emptyPreflight.skipped, "friction preflight skips empty manifold");
    expectTrue(emptyPreflight.can_skip_rebuild(), "friction preflight can skip empty manifold");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_preflight(empty),
        "should_skip_friction_basis_preflight on empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto needsPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(!needsPreflight.skipped, "friction preflight does not skip valid manifold");
    expectTrue(needsPreflight.needsRebuild, "friction preflight needs rebuild without cached basis");
    expectTrue(!needsPreflight.canReuse, "friction preflight cannot reuse missing basis");
    expectTrue(!needsPreflight.can_skip_rebuild(), "friction preflight cannot skip missing basis");

    fuse::physics::narrowphase::ContactManifold withBasis = needsBuild;
    withBasis.buildFrictionBasis();
    const auto reusePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(withBasis);
    expectTrue(reusePreflight.canReuse, "friction preflight can reuse valid basis");
    expectTrue(!reusePreflight.needsRebuild, "friction preflight does not need rebuild for valid basis");
    expectTrue(reusePreflight.can_skip_rebuild(), "friction preflight can skip valid basis");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_preflight(withBasis),
        "should_skip_friction_basis_preflight on valid basis");

    withBasis.contactNormal = {1.f, 0.f, 0.f};
    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(withBasis);
    expectTrue(stalePreflight.stale, "friction preflight flags stale basis");
    expectTrue(stalePreflight.needsRebuild, "friction preflight needs rebuild for stale basis");
    expectTrue(!stalePreflight.can_skip_rebuild(), "friction preflight cannot skip stale basis");
}

void testContactPairDeepenPassRejectGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 triggerA = bodies.addBody({2.f, 0.f, 0.f}, 0.f, fuse::physics::RB_TRIGGER);
    const fuse::u32 triggerB = bodies.addBody({2.5f, 0.f, 0.f}, 0.f, fuse::physics::RB_TRIGGER);
    const fuse::u32 staticA = bodies.addBody({0.f, 2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 kinematicA = bodies.addBody({0.f, 3.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, triggerA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, triggerB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, staticA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, kinematicA, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::is_any_trigger_contact_pair({dynamicA, triggerA}, bodies),
        "any-trigger guard flags mixed trigger/dynamic pair");
    expectTrue(
        !fuse::physics::narrowphase::is_any_trigger_contact_pair({dynamicA, dynamicB}, bodies),
        "any-trigger guard allows non-trigger pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({dynamicA, triggerA}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::AnyTrigger,
        "deepen reject reason flags any-trigger pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({dynamicA, triggerA}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::None,
        "base reject reason unchanged for mixed trigger pair");

    expectTrue(
        fuse::physics::narrowphase::is_massless_contact_pair({staticA, kinematicA}, bodies),
        "massless guard flags static/kinematic pair");
    expectTrue(
        !fuse::physics::narrowphase::is_massless_contact_pair({dynamicA, staticA}, bodies),
        "massless guard allows dynamic/static mix");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({staticA, kinematicA}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothMassless,
        "deepen reject reason flags both-massless pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {dynamicA, triggerA},
        {dynamicA, dynamicB},
    };
    expectTrue(
        fuse::physics::narrowphase::count_dispatchable_contact_pairs(mixedPairs, bodies, shapes) == 1u,
        "count_dispatchable excludes deepen-rejected pairs");
    expectTrue(
        fuse::physics::narrowphase::has_dispatchable_contact_pair(mixedPairs, bodies, shapes),
        "has_dispatchable true when one pair passes deepen preflight");
    expectTrue(
        !fuse::physics::narrowphase::has_dispatchable_contact_pair({{dynamicA, triggerA}}, bodies, shapes),
        "has_dispatchable false when all pairs deepen-rejected");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::AnyTrigger),
            "AnyTrigger") == 0,
        "reject reason name resolves AnyTrigger");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::BothMassless),
            "BothMassless") == 0,
        "reject reason name resolves BothMassless");
}

void testManifoldPruneDeepenPassGuards() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(!clean.hasNonUnitNormal(), "hasNonUnitNormal false for unit normal");
    expectTrue(!clean.needsNormalNormalization(), "needsNormalNormalization false for unit normal");
    expectTrue(
        fuse::physics::narrowphase::should_skip_manifold_prune(clean),
        "should_skip_manifold_prune true for clean manifold");

    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.3f);
    expectTrue(unnormalized.hasNonUnitNormal(), "hasNonUnitNormal true for scaled normal");
    expectTrue(unnormalized.needsNormalNormalization(), "needsNormalNormalization true for scaled normal");

    const auto unnormalizedPreflight =
        fuse::physics::narrowphase::preflight_manifold_prune(unnormalized);
    expectTrue(
        unnormalizedPreflight.needsNormalNormalize,
        "prune preflight flags non-unit normal");

    fuse::physics::narrowphase::ContactManifold shallow{};
    shallow.contactNormal = {0.f, 1.f, 0.f};
    shallow.addPoint({0.f, 0.f, 0.f}, 0.5f);
    shallow.addPoint({1.f, 0.f, 0.f}, 0.01f);
    expectTrue(
        !fuse::physics::narrowphase::should_skip_manifold_prune(shallow, 1e-6f, 1e-4f, 0.05f),
        "should_skip_manifold_prune false when shallow prune needed");
    shallow.pruneShallowPenetrationsIfNeeded(0.05f);
    expectTrue(
        fuse::physics::narrowphase::should_skip_manifold_prune(shallow, 1e-6f, 1e-4f, 0.05f),
        "should_skip_manifold_prune true after shallow prune");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 2.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    const auto finalizePreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(finalizePreflight.needsNormalNormalize, "finalize preflight flags non-unit normal");
    expectTrue(finalizePreflight.can_finalize(), "finalize preflight can finalize with penetrating points");
}

void testFrictionBasisDeepenPassPreflights() {
    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::contact_normal_needs_normalize(unnormalized),
        "contact_normal_needs_normalize true for scaled normal");
    expectTrue(
        fuse::physics::narrowphase::should_normalize_contact_normal_before_friction(unnormalized),
        "should_normalize before friction for scaled normal");

    const auto needsNormalizePreflight =
        fuse::physics::narrowphase::preflight_friction_basis_rebuild(unnormalized);
    expectTrue(
        needsNormalizePreflight.needsNormalNormalize,
        "friction preflight flags non-unit normal");
    expectTrue(
        needsNormalizePreflight.needsRebuild,
        "friction preflight needs rebuild without cached basis");
    expectTrue(
        !needsNormalizePreflight.can_skip_rebuild(),
        "friction preflight cannot skip missing basis");

    fuse::physics::narrowphase::ContactManifold unit{};
    unit.contactNormal = {0.f, 1.f, 0.f};
    unit.addPoint({0.f, 0.f, 0.f}, 0.2f);
    unit.buildFrictionBasis();
    expectTrue(
        !fuse::physics::narrowphase::should_normalize_contact_normal_before_friction(unit),
        "should_normalize false for unit normal with valid basis");
    const auto reusePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(unit);
    expectTrue(!reusePreflight.needsNormalNormalize, "friction preflight skips normalize for unit normal");
    expectTrue(reusePreflight.can_skip_rebuild(), "friction preflight can skip valid basis");
}

void testGjkSupportAndEpaStub() {
    const fuse::physics::vec3 hull[] = {
        {-1.f, 0.f, 0.f},
        {1.f, 0.f, 0.f},
    };
    const fuse::physics::vec3 supportPoint =
        fuse::physics::narrowphase::support(hull, 2, {1.f, 0.f, 0.f});
    expectTrue(supportPoint.x == 1.f, "support picks furthest hull point");

    const auto manifold = fuse::physics::narrowphase::epa(hull, 2, hull, 2, 0u, 1u);
    expectTrue(manifold.valid, "epa stub reports overlap for identical hulls");
}

void testContactPairDeepenFollowUpRejectGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {sleepingA, sleepingB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejects_for_reason matches both-sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {dynamicA, dynamicB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejects_for_reason does not false-positive valid pair");

    const fuse::u32 planeA = bodies.addBody({0.f, -1.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 planeB = bodies.addBody({0.f, -2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeA, {0.f, 1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeB, {0.f, 1.f, 0.f});
    expectTrue(
        fuse::physics::narrowphase::is_plane_plane_contact_pair({planeA, planeB}, shapes),
        "plane-plane guard flags both-plane pair");
    expectTrue(
        !fuse::physics::narrowphase::is_plane_plane_contact_pair({dynamicA, dynamicB}, shapes),
        "plane-plane guard allows non-plane pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {dynamicA, dynamicB},
    };
    const auto batchPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_batch(mixedPairs, bodies, shapes);
    expectTrue(batchPreflight.pairCount == 2u, "batch preflight reports pair count");
    expectTrue(batchPreflight.dispatchableCount == 1u, "batch preflight counts dispatchable pairs");
    expectTrue(batchPreflight.rejectedCount == 1u, "batch preflight counts rejected pairs");
    expectTrue(batchPreflight.can_dispatch(), "batch preflight can dispatch with one valid pair");
    expectTrue(
        !fuse::physics::narrowphase::narrowphase_batch_rejects_all(mixedPairs, bodies, shapes),
        "batch rejects_all false when one pair dispatchable");
    expectTrue(
        fuse::physics::narrowphase::narrowphase_batch_rejects_all({{sleepingA, sleepingB}}, bodies, shapes),
        "batch rejects_all true when all pairs deepen-rejected");
}

void testManifoldPruneFinalizeFollowUpGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold),
        "prune rejects_for_reason flags empty manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_prune_reject_reason_name(
                fuse::physics::narrowphase::ManifoldPruneRejectReason::AllSeparated),
            "AllSeparated") == 0,
        "prune reject reason name resolves AllSeparated");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            separated, fuse::physics::narrowphase::ManifoldPruneRejectReason::AllSeparated),
        "prune rejects_for_reason flags all-separated manifold");
    expectTrue(
        !fuse::physics::narrowphase::prune_contact_manifold_with_preflight(separated),
        "prune_with_preflight clears all-separated manifold");
    expectTrue(separated.empty(), "prune_with_preflight clears separated slots");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_with_preflight(dirty),
        "prune_with_preflight keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "prune_with_preflight removes separated slot");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            noNormal, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal),
        "finalize rejects_for_reason flags invalid normal");
    expectTrue(
        !fuse::physics::narrowphase::finalize_contact_manifold_with_preflight(noNormal),
        "finalize_with_preflight no-ops on invalid normal");

    fuse::physics::narrowphase::ContactManifold ready =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::finalize_contact_manifold_with_preflight(ready),
        "finalize_with_preflight finalizes valid manifold");
    expectTrue(ready.valid, "finalize_with_preflight sets validity");
    expectTrue(ready.hasFrictionBasis(), "finalize_with_preflight builds friction basis");

    const auto finalizePreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(
        finalizePreflight.reason == fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
        "finalize preflight reports None for finalized manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_finalize_reject_reason_name(
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::NoPenetratingPoints),
            "NoPenetratingPoints") == 0,
        "finalize reject reason name resolves NoPenetratingPoints");
}

void testContactPairDeepenPassDispatchGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::should_run_contact_pair_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "should_run_contact_pair_dispatch allows valid pair");
    expectTrue(
        !fuse::physics::narrowphase::should_run_contact_pair_dispatch({dynamicA, dynamicA}, bodies, shapes),
        "should_run_contact_pair_dispatch rejects self pair");
    expectTrue(
        fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "should_run_contact_pair_deepen_dispatch allows valid pair");
    expectTrue(
        !fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({sleepingA, sleepingB}, bodies, shapes),
        "should_run_contact_pair_deepen_dispatch rejects both-sleeping pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {dynamicA, dynamicB},
    };
    expectTrue(
        fuse::physics::narrowphase::should_run_narrowphase_batch(mixedPairs, bodies, shapes),
        "should_run_narrowphase_batch true when one pair dispatchable");
    expectTrue(
        !fuse::physics::narrowphase::should_run_narrowphase_batch({{sleepingA, sleepingB}}, bodies, shapes),
        "should_run_narrowphase_batch false when all pairs deepen-rejected");
}

void testManifoldPruneFinalizeDeepenPassGuards() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_prune(clean),
        "should_run_manifold_prune false for clean manifold");
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_finalize(clean),
        "should_run_manifold_finalize true for penetrating manifold");
    expectTrue(
        !fuse::physics::narrowphase::can_prune_manifold_in_place(clean),
        "can_prune_manifold_in_place false when prune is no-op");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_prune(dirty),
        "should_run_manifold_prune true when separated slots exist");
    expectTrue(
        fuse::physics::narrowphase::can_prune_manifold_in_place(dirty),
        "can_prune_manifold_in_place true when prune keeps penetrating slots");

    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_finalize(empty),
        "should_run_manifold_finalize false for empty manifold");
}

void testFrictionBasisDeepenPassGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(empty),
        "should_run_friction_basis_rebuild false for empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run_friction_basis_rebuild true without cached basis");

    fuse::physics::narrowphase::compute_friction_tangents(needsBuild);
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run_friction_basis_rebuild false after valid basis built");
}

void testContactBufferDeepenPassGuards() {
    fuse::physics::narrowphase::ContactBufferSoA empty{};
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_compaction_rejects_for_reason(
            empty, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "compaction reject reason flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_compaction(empty),
        "can_skip_contact_buffer_compaction on empty buffer");
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_friction_basis_rejects_for_reason(
            empty, fuse::physics::narrowphase::ContactBufferFrictionBasisRejectReason::EmptyBuffer),
        "friction-basis reject reason flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_friction_basis(empty),
        "can_skip_contact_buffer_friction_basis on empty buffer");

    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.25f);
    manifold.buildFrictionBasis();
    buffer.writeSlot(1u, manifold);
    buffer.compact();

    const auto compactionPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(
        compactionPreflight.reason == fuse::physics::narrowphase::ContactBufferCompactionRejectReason::NoWork,
        "compaction preflight no-ops after compact");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_compaction(buffer),
        "can_skip_contact_buffer_compaction after compact");

    const auto frictionPreflight = fuse::physics::narrowphase::preflight_contact_buffer_friction_basis(buffer);
    expectTrue(frictionPreflight.needsRebuild(), "friction-basis preflight allows rebuild with valid slot");
    expectTrue(
        fuse::physics::narrowphase::should_run_contact_buffer_friction_basis(buffer),
        "should_run_contact_buffer_friction_basis with valid manifold");
    buffer.buildFrictionTangentBases();
    const auto rebuiltBasis = buffer.tangentBasisAt(0u);
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis({0.f, 1.f, 0.f}, rebuiltBasis),
        "buildFrictionTangentBases stores orthonormal basis through gate");

    fuse::physics::narrowphase::ContactBufferSoA staleValidFlags;
    staleValidFlags.preparePairSlots(2u);
    staleValidFlags.activeCount = 1u;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_friction_basis_rejects_for_reason(
            staleValidFlags,
            fuse::physics::narrowphase::ContactBufferFrictionBasisRejectReason::NoValidManifolds),
        "friction-basis reject reason flags active count without valid slots");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_buffer_compaction_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferCompactionRejectReason::NoWork),
            "NoWork") == 0,
        "compaction reject reason name resolves NoWork");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_buffer_friction_basis_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferFrictionBasisRejectReason::NoValidManifolds),
            "NoValidManifolds") == 0,
        "friction-basis reject reason name resolves NoValidManifolds");
}

void testRunNarrowphaseSkipsDeepenRejectedPairs() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingB, {1.f, 0.f, 0.f});

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {
        {sleepingA, sleepingB},
        {dynamicA, dynamicB},
    };

    fuse::physics::narrowphase::ContactBufferSoA buffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(pairs, bodies, shapes, buffer);
    expectTrue(buffer.activeCount == 1u, "narrowphase skips deepen-rejected sleeping pair");
    const auto restored = buffer.manifoldAt(0u);
    expectTrue(restored.bodyA == dynamicA && restored.bodyB == dynamicB, "narrowphase keeps dispatchable pair");
    expectTrue(restored.hasFrictionBasis(), "narrowphase finalizes friction basis for dispatchable pair");
}

void testFrictionBasisFollowUpRejectGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            empty, fuse::physics::narrowphase::FrictionBasisRejectReason::EmptyManifold),
        "friction rejects_for_reason flags empty manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::friction_basis_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRejectReason::InvalidNormal),
            "InvalidNormal") == 0,
        "friction reject reason name resolves InvalidNormal");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            noNormal, fuse::physics::narrowphase::FrictionBasisRejectReason::InvalidNormal),
        "friction rejects_for_reason flags invalid normal");
    expectTrue(
        !fuse::physics::narrowphase::rebuild_friction_basis_with_preflight(noNormal),
        "rebuild_with_preflight skips invalid normal");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_with_preflight(needsBuild),
        "rebuild_with_preflight builds missing basis");
    expectTrue(needsBuild.hasFrictionBasis(), "rebuild_with_preflight stores orthonormal basis");

    const auto cachedTangent1 = needsBuild.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_with_preflight(needsBuild),
        "rebuild_with_preflight reuses valid basis");
    expectNear(
        needsBuild.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "rebuild_with_preflight preserves cached tangent1");

    const auto preflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(
        preflight.reason == fuse::physics::narrowphase::FrictionBasisRejectReason::None,
        "friction preflight reports None for valid basis");
    expectTrue(preflight.can_skip_rebuild(), "friction preflight can skip valid basis");
}

} // namespace

int main() {
    testSphereSphereCollision();
    testSpherePlaneCollision();
    testBoxSphereCollision();
    testCapsuleSphereCollision();
    testContactBufferClearReuse();
    testBoxBoxCollisionPointCount();
    testFrictionBasisOrthogonality();
    testFrictionClampStub();
    testEmptyContacts();
    testManifoldFillAndPointCap();
    testManifoldPointAccessAndFrictionBasis();
    testTangentialVelocityProjection();
    testContactBufferFrictionTangentSoA();
    testContactBufferWarmStartAndPointSlots();
    testRunNarrowphaseIntoBufferJobSafe();
    testContactPairRejectReasonGuards();
    testDetectContactsPairEmptyGuards();
    testGenerateContactManifoldAndFrictionTangents();
    testManifoldPruneNonPenetratingPoints();
    testManifoldPruneHelpers();
    testFrictionTangentEarlyOuts();
    testRunNarrowphaseFinalizesFrictionTangents();
    testContactPointTangentBasisAndManifoldClear();
    testContactBufferCapacityClamp();
    testContactPairDeepenGuards();
    testManifoldPruneDeepenHelpers();
    testFrictionTangentDeepenEarlyOuts();
    testContactPairRejectGuardHelpers();
    testCanFinalizeContactManifoldGuard();
    testManifoldPrunePreflightGuards();
    testManifoldPruneSkipGuards();
    testContactPairPreflightGuards();
    testFrictionBasisGuardHelpers();
    testFrictionBasisRefreshGuards();
    testContactPairDeepenRejectGuards();
    testCanSkipNarrowphaseGuards();
    testManifoldFinalizePreflightGuards();
    testGenerateContactManifoldIfNeededGuard();
    testManifoldShallowPruneSkipGuards();
    testFrictionBasisPreflightGuards();
    testContactPairDeepenPassRejectGuards();
    testManifoldPruneDeepenPassGuards();
    testFrictionBasisDeepenPassPreflights();
    testGjkSupportAndEpaStub();
    testContactPairDeepenFollowUpRejectGuards();
    testManifoldPruneFinalizeFollowUpGuards();
    testFrictionBasisFollowUpRejectGuards();
    testContactPairDeepenPassDispatchGuards();
    testManifoldPruneFinalizeDeepenPassGuards();
    testFrictionBasisDeepenPassGuards();
    testContactBufferDeepenPassGuards();
    testRunNarrowphaseSkipsDeepenRejectedPairs();

    if (g_failures == 0) {
        std::printf("fuse_physics_narrowphase_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_narrowphase_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}

// --- deepen additive from deepen-b4-narrowphase-manifold-prune-friction-guards-e8ef ---
void testContactPairRejectReasonNames() {
                fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedShapePair),
                fuse::physics::narrowphase::ContactPairRejectReason::None),
void testIsValidContactManifoldGuards() {
    testContactPairRejectReasonNames();

// --- deepen additive from deepen-b4-narrowphase-manifold-prune-4828 ---
void testManifoldNeedsPruneGuard() {
void testFrictionForManifoldCompositeGuard() {
        fuse::physics::narrowphase::should_skip_friction_for_manifold(empty, 0.5f, 0.3f, 1.f),
        fuse::physics::narrowphase::should_skip_friction_for_manifold(manifold, 0.f, 0.f, 1.f),
        !fuse::physics::narrowphase::should_skip_friction_for_manifold(manifold, 0.5f, 0.3f, 0.25f),
void testContactPairStaticSleepingRejectGuards() {

// --- deepen additive from deepen-b4-narrowphase-manifold-prune-friction-guards-78b1 ---
void testContactPairSleepingKinematicGuards() {
void testManifoldFinalizeGuards() {
void testFrictionBasisRebuildGuards() {

// --- deepen additive from deepen-b4-narrowphase-guards-56bb ---
void testContactPairDispatchGuards() {
void testManifoldPrunePreflightExGuards() {
        fuse::physics::narrowphase::can_skip_manifold_prune(dirtyPreflight) == false,
    expectTrue(dirty.pruneFromPreflight(dirtyPreflight), "pruneFromPreflight keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "pruneFromPreflight removes separated and duplicate slots");
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_friction_basis(empty);
        fuse::physics::narrowphase::should_skip_friction_basis_compute(empty),
        "should_skip_friction_basis_compute true for empty manifold");
    const auto cachedPreflight = fuse::physics::narrowphase::preflight_friction_basis(fresh);
    expectTrue(cachedPreflight.canSkipRebuild, "friction preflight can skip valid cached basis");
        fuse::physics::narrowphase::should_skip_friction_basis_compute(fresh),
        "should_skip_friction_basis_compute true with valid basis");
    testManifoldPrunePreflightExGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-d8a9 ---
void testContactPairDeepenPreflightGuards() {
            selfPreflight,
    const auto planePreflight =
            planePreflight,
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_finalize_contact_manifold(empty);
    expectTrue(!emptyPreflight.can_finalize(), "finalize preflight cannot finalize empty");
    const auto noNormalPreflight = fuse::physics::narrowphase::preflight_finalize_contact_manifold(noNormal);
    expectTrue(noNormalPreflight.invalidNormal, "finalize preflight flags invalid normal");
    expectTrue(!noNormalPreflight.can_finalize(), "finalize preflight rejects invalid normal");
    const auto separatedPreflight = fuse::physics::narrowphase::preflight_finalize_contact_manifold(separated);
    expectTrue(separatedPreflight.noPenetratingPoints, "finalize preflight flags no penetrating points");
    const auto readyPreflight = fuse::physics::narrowphase::preflight_finalize_contact_manifold(ready);
    expectTrue(readyPreflight.can_finalize(), "finalize preflight accepts penetrating manifold");
void testManifoldPruneDeepenPreflightGuards() {
    expectTrue(cleanPreflight.can_skip_prune(), "prune preflight can_skip_prune on clean manifold");
    const auto shallowPreflight = fuse::physics::narrowphase::preflight_manifold_prune(shallow, 1e-6f, 1e-4f, 0.05f);
    expectTrue(shallowPreflight.hasShallow, "prune preflight flags shallow penetrations");
    expectTrue(shallowPreflight.needs_pruning(), "prune preflight needs pruning with shallow flag");
    expectTrue(!emptyPreflight.needs_rebuild(), "friction preflight does not rebuild empty manifold");
    const auto missingPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(missingPreflight.missing, "friction preflight flags missing basis");
    expectTrue(missingPreflight.needs_rebuild(), "friction preflight needs rebuild when missing");
    const auto freshPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(freshPreflight.can_reuse(), "friction preflight can_reuse fresh basis");
    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(stalePreflight.stale, "friction preflight flags stale basis after normal change");
    expectTrue(stalePreflight.needs_rebuild(), "friction preflight needs rebuild when stale");
    testContactPairDeepenPreflightGuards();
    testManifoldPruneDeepenPreflightGuards();

// --- deepen additive from b4-narrowphase-deepen-guards-e063 ---
void testContactPairRejectBreakdownGuards() {
        selfBreakdown.reason == fuse::physics::narrowphase::ContactPairRejectReason::SelfPair,
        triggerBreakdown.reason == fuse::physics::narrowphase::ContactPairRejectReason::BothTriggers,
    expectTrue(!emptyPreflight.can_finalize(), "finalize preflight rejects empty manifold");
        fuse::physics::narrowphase::should_skip_manifold_finalize(empty),
        "should_skip_finalize true for empty manifold");
    const auto noNormalPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(noNormal);
    expectTrue(separatedPreflight.wouldBeEmptyAfterPrune, "finalize preflight flags prune-to-empty");
        !readyPreflight.needs_prune_before_finalize(),
    const auto dirtyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(dirty);
    expectTrue(dirtyPreflight.needs_prune_before_finalize(), "finalize preflight requests prune");
    expectTrue(dirtyPreflight.can_finalize(), "finalize preflight still allows finalize after prune");
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild(empty),
        "should_skip_friction_rebuild true for empty manifold");
    expectTrue(missingPreflight.missingBasis, "friction preflight flags missing basis");
    expectTrue(missingPreflight.needs_rebuild(), "friction preflight needs rebuild without basis");
        !fuse::physics::narrowphase::should_skip_friction_basis_rebuild(needsBuild),
        "should_skip_friction_rebuild false when basis missing");
    const auto cachedPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(cachedPreflight.can_reuse_cached(), "friction preflight reuses valid cached basis");
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild(needsBuild),
        "should_skip_friction_rebuild true for cached basis");
    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(stale);
    expectTrue(stalePreflight.staleBasis, "friction preflight flags stale basis");
    expectTrue(stalePreflight.needs_rebuild(), "friction preflight needs rebuild for stale basis");

// --- deepen additive from b4-narrowphase-guards-deepen-2074 ---
    const auto overCapPreflight = fuse::physics::narrowphase::preflight_manifold_prune(overCap);
    expectTrue(overCapPreflight.exceedsMaxPoints, "preflight flags point count above manifold cap");
    expectTrue(overCapPreflight.needs_pruning(), "preflight needs pruning when over cap");
    expectTrue(overCapPreflight.can_prune_in_place(), "preflight can prune over-cap manifold in place");
    const auto staticPreflight =
    expectTrue(!staticPreflight.can_dispatch(), "preflight rejects both-static pair");
        staticPreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::BothStatic,
    const auto triggerPreflight =
    expectTrue(!triggerPreflight.can_dispatch(), "preflight rejects both-trigger pair");
        triggerPreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::BothTriggers,
    const auto degeneratePreflight =
    expectTrue(!degeneratePreflight.can_dispatch(), "preflight rejects degenerate shape pair");
        degeneratePreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::DegenerateShape,
    const auto unsupportedPreflight =
    expectTrue(!unsupportedPreflight.can_dispatch(), "preflight rejects unsupported plane-plane pair");
        unsupportedPreflight.reason ==
void testContactManifoldFinalizePreflightGuards() {
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_contact_manifold_finalize(empty);
    expectTrue(emptyPreflight.empty, "finalize preflight flags empty manifold");
        fuse::physics::narrowphase::should_skip_contact_manifold_finalize(empty),
        "should_skip finalize true for empty manifold");
    const auto noNormalPreflight = fuse::physics::narrowphase::preflight_contact_manifold_finalize(noNormal);
    const auto separatedPreflight = fuse::physics::narrowphase::preflight_contact_manifold_finalize(separated);
    expectTrue(separatedPreflight.noPenetratingPoints, "finalize preflight flags all-separated points");
    expectTrue(separatedPreflight.pruneWouldEmpty, "finalize preflight flags prune-would-empty");
        fuse::physics::narrowphase::should_skip_contact_manifold_finalize(separated),
        "should_skip finalize true for separated manifold");
    const auto readyPreflight = fuse::physics::narrowphase::preflight_contact_manifold_finalize(ready);
        !fuse::physics::narrowphase::should_skip_contact_manifold_finalize(ready),
        "should_skip finalize false for ready manifold");
void testNarrowphaseDispatchPreflightWiring() {
    testContactManifoldFinalizePreflightGuards();
    testNarrowphaseDispatchPreflightWiring();

// --- deepen additive from deepen-b4-narrowphase-guards-d11b ---
void testContactPairEmptyInputGuards() {
    const auto rejectedPreflight =
        fuse::physics::narrowphase::contact_pair_was_rejected(rejectedPreflight),
        !fuse::physics::narrowphase::contact_pair_was_rejected(validPreflight),
        fuse::physics::narrowphase::should_skip_finalize_contact_manifold(empty),
    expectTrue(noNormalPreflight.invalidNormal, "finalize preflight flags zero-length normal");
    expectTrue(separatedPreflight.allSeparated, "finalize preflight flags all-separated points");
void testManifoldPruneGuardedDispatch() {
    expectTrue(emptyPreflight.shouldSkipTangents, "friction preflight flags skip-tangents");
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild_preflight(empty),
    const auto needsBuildPreflight =
    expectTrue(needsBuildPreflight.missingBasis, "friction preflight flags missing basis");
    expectTrue(needsBuildPreflight.needs_rebuild(), "friction preflight requests rebuild");
    expectTrue(!needsBuildPreflight.can_skip_rebuild(), "friction preflight cannot skip rebuild");
    expectTrue(cachedPreflight.can_skip_rebuild(), "friction preflight skips valid cached basis");
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild_preflight(needsBuild),
    expectTrue(stalePreflight.needs_rebuild(), "friction preflight requests stale rebuild");
void testDetectContactsPairGuarded() {

// --- deepen additive from b4-narrowphase-deepen-guards-c64f ---
void testContactPairB45DispatchGuards() {
            selfPreflight, fuse::physics::narrowphase::ContactPairRejectReason::SelfPair),
void testManifoldFinalizeB45Guards() {
    expectTrue(finalizePreflight.can_finalize(), "finalize preflight accepts penetrating manifold");
        !fuse::physics::narrowphase::should_skip_finalize_contact_manifold(clean),
    expectTrue(emptyPreflight.wouldFail, "finalize preflight marks empty manifold as failure");
void testFrictionBasisB45PreflightGuards() {
    expectTrue(needsPreflight.needs_rebuild(), "preflight rebuild true without cached basis");
    expectTrue(needsPreflight.needsNormalization, "preflight flags non-unit normal");
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild(cached),
    expectTrue(!cachedPreflight.needs_rebuild(), "preflight rebuild false with valid cached basis");
    testFrictionBasisB45PreflightGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-d755 ---
void testContactPairRejectPreflightGuards() {
    expectTrue(validPreflight.can_dispatch(), "reject preflight allows valid pair");
    expectTrue(!validPreflight.rejected, "reject preflight does not flag valid pair");
    expectTrue(!validPreflight.selfPair, "reject preflight self flag false for valid pair");
    expectTrue(!selfPreflight.can_dispatch(), "reject preflight rejects self pair");
    expectTrue(selfPreflight.selfPair, "reject preflight sets self flag");
    expectTrue(triggerPreflight.bothTriggers, "reject preflight sets both-triggers flag");
    expectTrue(emptyPreflight.empty, "finalize preflight sets empty flag");
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::Empty,
        !fuse::physics::narrowphase::should_skip_finalize_contact_manifold(ready),
        "should_skip_finalize false for ready manifold");
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::NoPenetratingPoints,
        fuse::physics::narrowphase::should_skip_prune_contact_manifold(onlySeparated),
        "should_skip_prune true when prune would empty manifold");
    const auto combinedPreflight =
    expectTrue(combinedPreflight.can_finalize_after_prune(), "combined preflight allows finalize on clean manifold");
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::PruneWouldEmpty),
void testFrictionBasisRebuildPreflightGuards() {
    expectTrue(missingPreflight.needs_rebuild(), "friction preflight needs rebuild without cached basis");
    expectTrue(cachedPreflight.canReuse, "friction preflight can reuse valid basis");
    expectTrue(cachedPreflight.can_skip_rebuild(), "friction preflight skips rebuild for cached basis");
    testContactPairRejectPreflightGuards();
    testFrictionBasisRebuildPreflightGuards();

// --- deepen additive from b4-narrowphase-guards-deepen-ea96 ---
void testContactPairPreflightDeepenGuards() {
    expectTrue(validPreflight.can_dispatch(), "deepen preflight allows valid pair");
        fuse::physics::narrowphase::can_dispatch_contact_pair(validPreflight),
    expectTrue(!validPreflight.isSelfPair, "valid preflight has no self flag");
    expectTrue(!validPreflight.isOutOfRange, "valid preflight has no OOB flag");
    expectTrue(!validPreflight.isMissingShape, "valid preflight has no missing-shape flag");
    const auto missingShapePreflight =
    expectTrue(!missingShapePreflight.can_dispatch(), "deepen preflight rejects missing shape");
    expectTrue(missingShapePreflight.isMissingShape, "deepen preflight flags missing shape");
            missingShapePreflight,
            fuse::physics::narrowphase::ContactPairRejectReason::MissingShape),
    expectTrue(triggerPreflight.isBothTriggers, "deepen preflight flags both-trigger pair");
            triggerPreflight,
            fuse::physics::narrowphase::ContactPairRejectReason::BothTriggers),
    expectTrue(staticPreflight.isBothStatic, "deepen preflight flags both-static pair");
    expectTrue(degeneratePreflight.isDegenerateShape, "deepen preflight flags degenerate shape");
            degeneratePreflight,
            fuse::physics::narrowphase::ContactPairRejectReason::DegenerateShape),
    expectTrue(emptyPreflight.isEmpty, "finalize preflight flags empty");
        "should_skip_manifold_finalize true for empty");
    expectTrue(noNormalPreflight.hasInvalidNormal, "finalize preflight flags invalid normal");
        !fuse::physics::narrowphase::can_finalize_with_preflight(noNormalPreflight),
    expectTrue(separatedPreflight.hasNoPenetratingPoints, "finalize preflight flags all-separated");
        fuse::physics::narrowphase::should_skip_manifold_finalize(separated),
        "should_skip_manifold_finalize true for separated");
        fuse::physics::narrowphase::can_finalize_with_preflight(readyPreflight),
        !fuse::physics::narrowphase::should_skip_manifold_finalize(ready),
        "should_skip_manifold_finalize false for ready manifold");
void testManifoldPruneShallowPreflightGuards() {
    expectTrue(shallowPreflight.hasShallowPenetrations, "prune preflight flags shallow penetrations");
    expectTrue(shallowPreflight.needs_pruning(), "prune preflight needs pruning with shallow slot");
    expectTrue(!cleanPreflight.hasShallowPenetrations, "prune preflight clean without shallow slots");
    expectTrue(!cleanPreflight.needs_pruning(), "prune preflight skips clean manifold with shallow check");
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild_preflight(emptyPreflight),
    expectTrue(needsBuildPreflight.needs_rebuild(), "friction preflight needs rebuild without cache");
    expectTrue(!needsBuildPreflight.can_reuse(), "friction preflight cannot reuse missing basis");
        !fuse::physics::narrowphase::should_skip_friction_basis_rebuild_preflight(needsBuildPreflight),
    expectTrue(cachedPreflight.hasCachedBasis, "friction preflight detects cached basis");
    expectTrue(cachedPreflight.can_reuse(), "friction preflight can reuse valid basis");
        fuse::physics::narrowphase::can_reuse_friction_basis(cachedPreflight),
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild_preflight(cachedPreflight),
    expectTrue(stalePreflight.wouldRebuild, "friction preflight would rebuild stale basis");
        !fuse::physics::narrowphase::should_skip_friction_basis_rebuild_preflight(stalePreflight),
    testContactPairPreflightDeepenGuards();
    testManifoldPruneShallowPreflightGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-5111 ---
void testContactPairKinematicSleepingRejectGuards() {
void testContactPairDispatchPreflightGuards() {
    expectTrue(validPreflight.can_dispatch(), "dispatch preflight allows valid pair");
    expectTrue(validPreflight.has_valid_bodies, "dispatch preflight marks valid bodies");
    expectTrue(validPreflight.has_valid_shapes, "dispatch preflight marks valid shapes");
    expectTrue(!selfPreflight.can_dispatch(), "dispatch preflight rejects self pair");
    expectTrue(!selfPreflight.has_valid_bodies, "self pair fails body validity");
    expectTrue(!emptyPreflight.can_finalize(), "empty manifold cannot finalize");
    const auto unnormalizedPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(unnormalized);
    expectTrue(unnormalizedPreflight.needsNormalization, "finalize preflight flags non-unit normal");
    expectTrue(separatedPreflight.noPenetrating, "finalize preflight flags all-separated points");
    expectTrue(emptyPreflight.skipped, "friction rebuild preflight skips empty manifold");
    expectTrue(emptyPreflight.skipTangents, "friction rebuild preflight marks skip tangents");
    expectTrue(needsPreflight.needs_rebuild(), "rebuild preflight true without cached basis");
    expectTrue(cachedPreflight.can_skip_rebuild(), "rebuild preflight skips valid cached basis");
    expectTrue(stalePreflight.isStale, "rebuild preflight flags stale cached basis");
    expectTrue(stalePreflight.needs_rebuild(), "rebuild preflight requires refresh for stale basis");
    testContactPairDispatchPreflightGuards();

// --- deepen additive from b4-narrowphase-deepen-guards-f32b ---
    expectTrue(!validReject.rejected, "reject preflight allows valid pair");
    expectTrue(validReject.can_dispatch(), "reject preflight can_dispatch for valid pair");
    expectTrue(!validReject.selfPair, "valid pair is not self pair");
    expectTrue(selfReject.rejected, "reject preflight marks self pair");
    expectTrue(selfReject.selfPair, "reject preflight flags selfPair");
    expectTrue(staticReject.rejected, "reject preflight marks both-static pair");
    expectTrue(staticReject.bothStatic, "reject preflight flags bothStatic");
    const auto dispatchPreflight =
    expectTrue(dispatchPreflight.can_dispatch(), "dispatch preflight allows valid pair");
    expectTrue(!dispatchPreflight.skipped, "dispatch preflight does not skip valid pair");
    expectTrue(readyPreflight.hasValidNormal, "finalize preflight sees valid normal");
    expectTrue(readyPreflight.hasPenetrating, "finalize preflight sees penetrating points");
        "should_skip false for ready manifold");
    expectTrue(!separatedPreflight.can_finalize(), "finalize preflight rejects separated points");
        "should_skip true for separated manifold");
    const auto missingPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(fresh);
    expectTrue(missingPreflight.missing, "rebuild preflight flags missing basis");
    expectTrue(missingPreflight.needs_rebuild(), "rebuild preflight needs build when missing");
        !fuse::physics::narrowphase::should_skip_friction_basis_rebuild(fresh),
        "should_skip false when basis missing");
    const auto cachedPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(fresh);
    expectTrue(cachedPreflight.canReuse, "rebuild preflight canReuse valid cached basis");
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild(fresh),
        "should_skip true when basis cached");
    expectTrue(stalePreflight.stale, "rebuild preflight flags stale basis");
    expectTrue(stalePreflight.needs_rebuild(), "rebuild preflight needs rebuild when stale");
    const auto skippedPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(empty);
    expectTrue(skippedPreflight.skipped, "rebuild preflight skips empty manifold");

// --- deepen additive from deepen-b4-narrowphase-guards-914a ---
void testContactPairGuardedDispatch() {
        skipped.reason == fuse::physics::narrowphase::ContactPairRejectReason::SelfPair,
        emptyPreflight.reason == fuse::physics::narrowphase::ManifoldFinalizeFailureReason::Empty,
        "should_skip_manifold_finalize true for empty manifold");
        noNormalPreflight.reason ==
    const auto prunePreflight = fuse::physics::narrowphase::preflight_manifold_finalize(allSeparated);
        prunePreflight.reason ==
    expectTrue(readyPreflight.can_finalize(), "finalize preflight allows penetrating manifold");
void testManifoldFinalizeGuardedEntryPoints() {
void testManifoldPrunePreflightShallowFlag() {
    const auto skipPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(empty);
    expectTrue(skipPreflight.skipped, "friction preflight skips empty manifold");
    expectTrue(skipPreflight.shouldSkip, "friction preflight marks shouldSkip for empty manifold");
        "should_skip_friction_basis_preflight true for empty manifold");
    expectTrue(missingPreflight.needs_rebuild(), "friction preflight needs rebuild when basis missing");
    expectTrue(freshPreflight.can_reuse(), "friction preflight can reuse valid basis");
    testManifoldFinalizeGuardedEntryPoints();
    testManifoldPrunePreflightShallowFlag();

// --- deepen additive from deepen-b4-narrowphase-guards-7360 ---
void testRunNarrowphaseDeepenDispatchGuards() {
void testBuildFrictionTangentBasesReuseGuard() {

// --- deepen additive from b4-narrowphase-deepen-guards-a773 ---
void testNarrowphaseRejectReasonGuards() {
            {}, bodies, shapes, fuse::physics::narrowphase::NarrowphaseRejectReason::EmptyPairList),
                fuse::physics::narrowphase::NarrowphaseRejectReason::AllPairsRejected),
            {{bodyA, bodyB}}, bodies, shapes, fuse::physics::narrowphase::NarrowphaseRejectReason::None),
void testContactPairDeepenRejectsForReasonGuards() {
void testNarrowphasePairListPreflightGuards() {
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_narrowphase_pair_list({}, bodies, shapes);
    expectTrue(!emptyPreflight.can_dispatch(), "empty pair-list preflight cannot dispatch");
    expectTrue(emptyPreflight.emptyPairList, "empty pair-list preflight marks empty list");
    expectTrue(!rejectedPreflight.can_dispatch(), "all-rejected pair-list preflight cannot dispatch");
    expectTrue(rejectedPreflight.allPairsRejected, "all-rejected pair-list preflight marks all rejected");
    const auto mixedPreflight = fuse::physics::narrowphase::preflight_narrowphase_pair_list(
    expectTrue(mixedPreflight.can_dispatch(), "mixed pair-list preflight can dispatch");
    expectTrue(mixedPreflight.dispatchablePairCount == 1u, "mixed pair-list preflight counts dispatchable pairs");
void testManifoldPruneRejectReasonGuards() {
            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::NoPruningNeeded),
            allSeparated, fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmptyAfterPrune),
                fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmptyAfterPrune),
            dirty, fuse::physics::narrowphase::ManifoldPruneRejectReason::None),
        dirtyPreflight.reason == fuse::physics::narrowphase::ManifoldPruneRejectReason::NoPruningNeeded,
void testManifoldFinalizeRejectReasonGuards() {
            empty, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold),
            ready, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None),
    expectTrue(readyPreflight.can_finalize(), "finalize preflight accepts ready manifold with reason None");
        readyPreflight.reason == fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
            separated, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune),
void testFrictionBasisRejectReasonGuards() {
            empty, fuse::physics::narrowphase::FrictionBasisRejectReason::SkippedManifold),
            needsBuild, fuse::physics::narrowphase::FrictionBasisRejectReason::None),
        !fuse::physics::narrowphase::should_skip_friction_basis_preflight(needsBuild),
        "should_skip_friction_basis_preflight false when rebuild may proceed");
            needsBuild, fuse::physics::narrowphase::FrictionBasisRejectReason::ValidCachedBasis),
                fuse::physics::narrowphase::FrictionBasisRejectReason::ValidCachedBasis),
    const auto reusePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(reusePreflight.can_skip_rebuild(), "friction preflight can skip valid cached basis");
        reusePreflight.reason == fuse::physics::narrowphase::FrictionBasisRejectReason::ValidCachedBasis,
    expectTrue(stalePreflight.needsRebuild, "stale friction preflight needs rebuild");
    expectTrue(!stalePreflight.can_skip_rebuild(), "stale friction preflight cannot skip rebuild");
    testNarrowphaseRejectReasonGuards();
    testNarrowphasePairListPreflightGuards();
    testManifoldPruneRejectReasonGuards();
    testManifoldFinalizeRejectReasonGuards();
    testFrictionBasisRejectReasonGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-72f5 ---
void testNarrowphaseBatchPreflightGuards() {
    expectTrue(!emptyPreflight.can_run(), "batch preflight cannot run on empty pair list");
    expectTrue(emptyPreflight.totalPairs == 0u, "batch preflight reports zero total pairs");
    expectTrue(!rejectedPreflight.can_run(), "batch preflight cannot run when all pairs rejected");
    expectTrue(rejectedPreflight.rejectedPairs == 1u, "batch preflight counts rejected pair");
    const auto mixedPreflight = fuse::physics::narrowphase::preflight_narrowphase(mixed, bodies, shapes);
    expectTrue(mixedPreflight.can_run(), "batch preflight can run with dispatchable pair");
    expectTrue(mixedPreflight.dispatchablePairs == 1u, "batch preflight counts one dispatchable pair");
    expectTrue(mixedPreflight.rejectedPairs == 1u, "batch preflight counts one rejected pair");
            fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold,
            fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmptyAfterPrune,
        "should_skip_manifold_prune on clean manifold");
    expectTrue(cleanPreflight.can_skip_prune(), "prune preflight can skip clean manifold");
        cleanPreflight.reason == fuse::physics::narrowphase::ManifoldPruneRejectReason::None,
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold,
        "should_skip_manifold_finalize on empty manifold");
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal,
    expectTrue(readyPreflight.can_finalize(), "finalize preflight can finalize ready manifold");
            fuse::physics::narrowphase::FrictionBasisRejectReason::SkippedManifold,
            fuse::physics::narrowphase::FrictionBasisRejectReason::BasisCurrent,
                fuse::physics::narrowphase::FrictionBasisRejectReason::BasisCurrent),
    testNarrowphaseBatchPreflightGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-c9f2 ---
void testZeroFrictionAndBatchPreflightGuards() {
    expectTrue(batchPreflight.totalPairs == 2u, "batch preflight counts total pairs");
    expectTrue(batchPreflight.can_dispatch(), "batch preflight can dispatch with mixed list");
    expectTrue(!batchPreflight.can_skip(), "batch preflight cannot skip mixed list");
void testManifoldPruneAndFinalizePreflightHelpers() {
    const auto cleanPrunePreflight = fuse::physics::narrowphase::preflight_manifold_prune(clean);
    expectTrue(cleanPrunePreflight.can_skip_prune(), "clean manifold prune preflight can skip");
    const auto readyFinalizePreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(!readyFinalizePreflight.can_skip_finalize(), "ready manifold finalize preflight cannot skip");
    const auto separatedFinalizePreflight =
    expectTrue(separatedFinalizePreflight.can_skip_finalize(), "separated manifold finalize preflight can skip");
void testFrictionBasisPreflightRebuildHelpers() {
    expectTrue(needsPreflight.needs_work(), "friction preflight needs work without cached basis");
    expectTrue(!reusePreflight.needs_work(), "friction preflight has no work after build");
    expectTrue(stalePreflight.needs_work(), "friction preflight needs work for stale basis");
    testZeroFrictionAndBatchPreflightGuards();
    testManifoldPruneAndFinalizePreflightHelpers();
    testFrictionBasisPreflightRebuildHelpers();

// --- deepen additive from deepen-b4-narrowphase-guards-d130 ---
void testContactPairDeepenPassGuards() {
    const auto listPreflight =
    expectTrue(listPreflight.totalPairs == 2u, "pair-list preflight reports total pairs");
    expectTrue(listPreflight.dispatchableCount == 1u, "pair-list preflight reports dispatchable count");
    expectTrue(listPreflight.rejectedCount == 1u, "pair-list preflight reports rejected count");
    expectTrue(listPreflight.can_dispatch(), "pair-list preflight can dispatch mixed list");
    expectTrue(!listPreflight.can_skip(), "pair-list preflight cannot skip mixed list");
            fuse::physics::narrowphase::ManifoldPruneRejectReason::Empty,
            empty, fuse::physics::narrowphase::ManifoldPruneRejectReason::Empty),
                fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmpty),
        fuse::physics::narrowphase::should_skip_manifold_prune(empty),
        "should_skip_manifold_prune on empty manifold");
            fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmpty,
        fuse::physics::narrowphase::should_skip_manifold_prune(allSeparated),
        "should_skip_manifold_prune when prune would leave no points");
    expectTrue(dirtyPreflight.can_skip_prune(), "can_skip_prune true after prune_if_needed");

// --- deepen additive from b4-narrowphase-deepen-guards-699f ---
void testNarrowphasePairBatchPreflightGuards() {
    const auto batchPreflight = fuse::physics::narrowphase::preflight_narrowphase_pairs(mixed, bodies, shapes);
    expectTrue(batchPreflight.hasDispatchable, "batch preflight has dispatchable pair");
    expectTrue(!batchPreflight.can_skip_batch(), "batch preflight cannot skip mixed list");
    expectTrue(batchPreflight.can_dispatch_any(), "batch preflight can dispatch any");
    expectTrue(rejectedPreflight.can_skip_batch(), "batch preflight skips all-rejected list");
    expectTrue(!rejectedPreflight.can_dispatch_any(), "batch preflight cannot dispatch all-rejected list");
void testManifoldPruneFinalizeDeepenGuards() {
    const auto dirtyPreflight = fuse::physics::narrowphase::preflight_manifold_prune(dirty, 1e-6f, 1e-4f, 0.05f);
    expectTrue(!dirtyPreflight.needs_any_pruning(0.05f), "prune preflight clean after conditional prune");
    const auto finalizePreflight = fuse::physics::narrowphase::preflight_manifold_finalize(manifold);
    expectTrue(finalizePreflight.needs_any_work(), "finalize preflight needs work before finalize");
void testFrictionBasisDeepenPreflightGuards() {
    expectTrue(needsPreflight.can_rebuild(), "friction preflight can_rebuild without cached basis");
    testNarrowphasePairBatchPreflightGuards();
    testFrictionBasisDeepenPreflightGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-1468 ---
void testContactPairDeepen2RejectGuards() {
            fuse::physics::narrowphase::ContactPairRejectReason::BothZeroInvMass,
            fuse::physics::narrowphase::ContactPairRejectReason::NoColliderDispatch,
    const auto zeroMassPreflight =
    expectTrue(!zeroMassPreflight.can_dispatch(), "deepen2 preflight rejects both zero-inv-mass pair");
        zeroMassPreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::BothZeroInvMass,
        fuse::physics::narrowphase::should_skip_contact_pair_deepen2_dispatch({zeroMassA, zeroMassB}, bodies, shapes),
        !fuse::physics::narrowphase::should_skip_contact_pair_deepen2_dispatch({dynamicA, dynamicB}, bodies, shapes),
                fuse::physics::narrowphase::ContactPairRejectReason::BothZeroInvMass),
                fuse::physics::narrowphase::ContactPairRejectReason::NoColliderDispatch),
void testCanSkipNarrowphaseDeepen2Guards() {
void testManifoldNormalizeAndFinalizeDeepenGuards() {
    const auto readyDeepenPreflight = fuse::physics::narrowphase::preflight_manifold_finalize_deepen(ready);
    expectTrue(readyDeepenPreflight.can_finalize(), "deepen finalize preflight can finalize ready manifold");
    expectTrue(!readyDeepenPreflight.needsNormalNormalization, "deepen finalize preflight no normal fixup needed");
    const auto partialPreflight =
    expectTrue(partialPreflight.partial, "deepen friction preflight flags partial basis");
    expectTrue(partialPreflight.needsRebuild, "deepen friction preflight needs rebuild for partial basis");
    expectTrue(!partialPreflight.can_skip_rebuild(), "deepen friction preflight cannot skip partial basis");
        !fuse::physics::narrowphase::should_skip_friction_basis_deepen_preflight(partial),
        "should_skip_deepen_preflight false for partial basis");
        fuse::physics::narrowphase::should_skip_friction_basis_deepen_preflight(partial),
        "should_skip_deepen_preflight true after completion");
        unnormalizedPreflight.needsNormalNormalization,

// --- deepen additive from deepen-b4-narrowphase-guards-b463 ---
void testContactPairBatchPreflightGuards() {
void testContactPairPlanePlaneAndDeepenDispatchGuards() {
void testManifoldFinalizeDeepenGuards() {
    expectTrue(unnormalizedPreflight.needsNormalNormalization, "finalize preflight flags non-unit normal");
void testContactBufferPreflightGuards() {
    expectTrue(!compactionPreflight.needsCompaction(), "compaction preflight false after compact");
    const auto clampPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(buffer);
    expectTrue(clampPreflight.needsClamp(), "clamp preflight true when over capacity");
void testContactBufferFrictionRebuildPreflightGuards() {
    const auto initialPreflight =
    expectTrue(!initialPreflight.can_skip_rebuild(), "friction preflight needs rebuild for stale basis");
    expectTrue(initialPreflight.staleSlotCount == 1u, "friction preflight counts one stale slot");
    expectTrue(initialPreflight.rebuildSlotCount == 1u, "friction preflight counts one rebuild slot");
    const auto rebuiltPreflight =
    expectTrue(rebuiltPreflight.can_skip_rebuild(), "friction preflight can skip after rebuild");
void testWarmStartFrictionPreflightGuards() {
        fuse::physics::narrowphase::should_skip_warm_start_friction(empty),
        "should_skip_warm_start on empty manifold");
    const auto noImpulsePreflight =
    expectTrue(!noImpulsePreflight.can_warm_start(), "warm-start preflight false without impulses");
    const auto readyPreflight = fuse::physics::narrowphase::preflight_warm_start_friction(ready);
    expectTrue(readyPreflight.hasWarmImpulse, "warm-start preflight detects warm impulses");
    expectTrue(readyPreflight.hasValidBasis, "warm-start preflight accepts valid basis");
    expectTrue(readyPreflight.can_warm_start(), "warm-start preflight can warm-start ready manifold");
        !fuse::physics::narrowphase::should_skip_warm_start_friction(ready),
        "should_skip false for warm-startable manifold");
    const auto stalePreflight = fuse::physics::narrowphase::preflight_warm_start_friction(ready);
    expectTrue(!stalePreflight.hasValidBasis, "warm-start preflight rejects stale basis");
        fuse::physics::narrowphase::should_skip_warm_start_friction(ready),
        "should_skip true when basis is stale");
    testContactPairBatchPreflightGuards();
    testContactBufferPreflightGuards();
    testContactBufferFrictionRebuildPreflightGuards();
    testWarmStartFrictionPreflightGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-1764 ---
            needsBuild, fuse::physics::narrowphase::FrictionBasisRejectReason::MissingBasis),
        needsPreflight.reason == fuse::physics::narrowphase::FrictionBasisRejectReason::MissingBasis,
            stale, fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis),
                fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis),

// --- deepen additive from deepen-b4-narrowphase-guards-ae90 ---
            allRejected, bodies, shapes, fuse::physics::narrowphase::NarrowphaseRejectReason::AllPairsRejected),
            mixed, bodies, shapes, fuse::physics::narrowphase::NarrowphaseRejectReason::None),
    const fuse::physics::narrowphase::NarrowphasePreflight preflight =
            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::None),
            fresh, fuse::physics::narrowphase::FrictionBasisRejectReason::None),

// --- deepen additive from b4-narrowphase-deepen-guards-6e88 ---
void testContactPairZeroInvMassDeepenGuards() {
            fuse::physics::narrowphase::ContactPairRejectReason::ZeroInvMass,
            fuse::physics::narrowphase::ContactPairRejectReason::ZeroInvMass),
    expectTrue(emptyPreflight.skipped, "batch preflight skips empty pair list");
    expectTrue(!emptyPreflight.can_dispatch(), "batch preflight cannot dispatch empty list");
    expectTrue(rejectedPreflight.skipped, "batch preflight skips all-rejected pairs");
    expectTrue(rejectedPreflight.stats.rejectedCount == 1u, "batch preflight counts rejected pair");
    expectTrue(!mixedPreflight.skipped, "batch preflight does not skip mixed batch");
    expectTrue(mixedPreflight.can_dispatch(), "batch preflight can dispatch mixed batch");
    expectTrue(mixedPreflight.stats.dispatchableCount == 1u, "batch preflight counts dispatchable pair");
void testManifoldGeneratePreflightGuards() {
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_generate_contact_manifold(empty);
    expectTrue(emptyPreflight.skipped, "generate preflight skips empty manifold");
    expectTrue(!emptyPreflight.can_generate(), "generate preflight cannot generate empty manifold");
    const auto readyPreflight = fuse::physics::narrowphase::preflight_generate_contact_manifold(ready);
    expectTrue(!readyPreflight.skipped, "generate preflight does not skip ready manifold");
    expectTrue(readyPreflight.can_generate(), "generate preflight can generate penetrating manifold");
    expectTrue(readyPreflight.needsFrictionBasis, "generate preflight needs friction basis");
    expectTrue(!readyPreflight.needsPruning, "generate preflight reports no pruning for clean manifold");
void testManifoldPruneChainPreflightGuards() {
void testManifoldFinalizeChainGuards() {
    const auto chainPreflight =
    expectTrue(!chainPreflight.skipped, "finalize chain preflight does not skip detected manifold");
    expectTrue(chainPreflight.can_finalize(), "finalize chain preflight can finalize detected manifold");
void testFrictionBasisEnsurePreflightGuards() {
    const auto needsPreflight = fuse::physics::narrowphase::preflight_friction_basis_ensure(needsBuild);
    expectTrue(!needsPreflight.skipped, "ensure preflight does not skip valid manifold");
    expectTrue(needsPreflight.needsEnsure, "ensure preflight needs build without cached basis");
    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_ensure(needsBuild);
    expectTrue(stalePreflight.stale, "ensure preflight flags stale basis");
    expectTrue(!stalePreflight.can_skip_ensure(), "ensure preflight cannot skip stale basis");
    testManifoldGeneratePreflightGuards();
    testManifoldPruneChainPreflightGuards();
    testFrictionBasisEnsurePreflightGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-ddb5 ---
void testContactPairDeepenFollowUpGuards() {
            fuse::physics::narrowphase::ContactPairRejectReason::NegativeInverseMass,
            fuse::physics::narrowphase::ContactPairRejectReason::NegativeInverseMass),
            fuse::physics::narrowphase::ContactPairRejectReason::BothZeroMass,
            fuse::physics::narrowphase::ContactPairRejectReason::SleepingKinematicMix,
                fuse::physics::narrowphase::ContactPairRejectReason::SleepingKinematicMix),
            empty, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::Empty),
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyAfterPrune,
        readyPreflight.rejectReason == fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyAfterPrune),
void testManifoldPruneDispatchGuards() {
        !fuse::physics::narrowphase::should_skip_manifold_prune(dirty),
        "should_skip_manifold_prune false when separated slots exist");
            fuse::physics::narrowphase::FrictionBasisRejectReason::Skipped,
            fuse::physics::narrowphase::FrictionBasisRejectReason::MissingNormal,
            fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis,
        stalePreflight.rejectReason == fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis,

// --- deepen additive from deepen-b4-narrowphase-guards-f4c2 ---
    const auto allRejectedPreflight = fuse::physics::narrowphase::preflight_narrowphase_batch(
    expectTrue(!allRejectedPreflight.can_dispatch(), "batch preflight rejects all-sleeping list");
        allRejectedPreflight.reason ==
            fuse::physics::narrowphase::NarrowphaseRejectReason::AllPairsRejected,
    const auto mixedPreflight = fuse::physics::narrowphase::preflight_narrowphase_batch(
    expectTrue(mixedPreflight.can_dispatch(), "batch preflight allows mixed list");
    expectTrue(mixedPreflight.dispatchableCount == 1u, "batch preflight counts one dispatchable pair");
    expectTrue(mixedPreflight.rejectedCount == 1u, "batch preflight counts one rejected pair");
void testDetectContactsPairIfNeededGuard() {
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::NoPenetration),
        separatedPreflight.reason ==
    expectTrue(!separatedPreflight.can_finalize(), "finalize preflight cannot finalize separated manifold");
    expectTrue(shallowPreflight.needsShallowPrune, "prune dispatch preflight flags shallow slot");
void testFrictionBasisRejectAndPreflightDispatchGuards() {
    const auto noNormalPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(noNormal);
            fuse::physics::narrowphase::FrictionBasisRejectReason::InvalidNormal,
    testFrictionBasisRejectAndPreflightDispatchGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-0339 ---
void testContactPairDeepenPassShouldRunGuards() {
        "should_run mirrors should_skip for base dispatch");
void testManifoldPruneDeepenPassRejectReasonGuards() {
        "should_skip_manifold_prune true for empty manifold");
            fuse::physics::narrowphase::ManifoldPruneRejectReason::AlreadyClean,
        dirtyPreflight.reason == fuse::physics::narrowphase::ManifoldPruneRejectReason::AlreadyClean,
    expectTrue(dirtyPreflight.can_skip_prune(), "preflight can_skip_prune after conditional prune");
                fuse::physics::narrowphase::ManifoldPruneRejectReason::AlreadyClean),
void testManifoldFinalizeDeepenPassRejectReasonGuards() {
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune,
void testFrictionBasisDeepenPassShouldRunGuards() {
            fuse::physics::narrowphase::FrictionBasisRejectReason::EmptyOrInvalid,
            empty, fuse::physics::narrowphase::FrictionBasisRejectReason::EmptyOrInvalid),
        "should_skip mirrors should_run for empty manifold");
            fuse::physics::narrowphase::FrictionBasisRejectReason::BasisReusable,
    expectTrue(reusePreflight.can_skip_rebuild(), "preflight can_skip_rebuild for reusable basis");
    expectTrue(!reusePreflight.can_rebuild(), "preflight cannot rebuild reusable basis");
                fuse::physics::narrowphase::FrictionBasisRejectReason::BasisReusable),
    testManifoldPruneDeepenPassRejectReasonGuards();
    testManifoldFinalizeDeepenPassRejectReasonGuards();

// --- deepen additive from b4-narrowphase-deepen-guards-931e ---
    expectTrue(batchPreflight.stats.totalPairs == 2u, "batch preflight reports total pairs");
    expectTrue(batchPreflight.stats.dispatchablePairs == 1u, "batch preflight reports dispatchable count");
    expectTrue(batchPreflight.stats.rejectedPairs == 1u, "batch preflight reports rejected count");
    expectTrue(batchPreflight.can_dispatch_any(), "batch preflight can dispatch mixed list");
void testFrictionBasisRebuildDeepenPassGuards() {

// --- deepen additive from b4-narrowphase-deepen-guards-6242 ---
void testContactPairDeepenPassLayerGuards() {
            fuse::physics::narrowphase::ContactPairRejectReason::PlanePlane,
            fuse::physics::narrowphase::ContactPairRejectReason::InvalidPlaneNormal,
    const auto deepenPassPreflight =
    expectTrue(deepenPassPreflight.can_dispatch(), "deepen-pass preflight allows valid pair");
        !fuse::physics::narrowphase::should_skip_contact_pair_deepen_pass_dispatch({dynamicA, dynamicB}, bodies, shapes),
        fuse::physics::narrowphase::should_skip_contact_pair_deepen_pass_dispatch({planeA, planeB}, bodies, shapes),
    expectTrue(!batchPreflight.allRejected, "batch preflight not all rejected for mixed list");
        !fuse::physics::narrowphase::should_skip_contact_pair_batch(mixedPairs, bodies, shapes),
        fuse::physics::narrowphase::should_skip_contact_pair_batch({{planeA, planeB}}, bodies, shapes),
                fuse::physics::narrowphase::ContactPairRejectReason::PlanePlane),
            fuse::physics::narrowphase::ManifoldPruneRejectReason::NoPruneNeeded,
            fuse::physics::narrowphase::ManifoldPruneRejectReason::NoPruneNeeded),
void testFrictionBasisDeepenPassRejectGuards() {
            fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::SkippedEmpty,
            fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::SkippedEmpty),
            fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::NeedsRebuild,
            fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuse,
                fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::NeedsRebuild),

// --- deepen additive from deepen-b4-narrowphase-guards-071f ---
            fuse::physics::narrowphase::FrictionBasisRejectReason::EmptyManifold,
            fuse::physics::narrowphase::FrictionBasisRejectReason::CanReuseBasis,
                fuse::physics::narrowphase::FrictionBasisRejectReason::CanReuseBasis),

// --- deepen additive from deepen-narrowphase-guards-6b0c ---
        static_cast<fuse::u32>(fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ManifoldPruneRejectReason::NothingToPrune),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ManifoldPruneRejectReason::None),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmpty),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None),
        static_cast<fuse::u32>(fuse::physics::narrowphase::FrictionBasisRejectReason::EmptyManifold),
        static_cast<fuse::u32>(fuse::physics::narrowphase::FrictionBasisRejectReason::None),
        static_cast<fuse::u32>(fuse::physics::narrowphase::FrictionBasisRejectReason::CanReuse),
                fuse::physics::narrowphase::FrictionBasisRejectReason::CanReuse),

// --- deepen additive from deepen-b4-narrowphase-guards-fbc2 ---
void testContactPairDeepenPassRejectReasonGuards() {
    expectTrue(batchPreflight.dispatchableCount == 1u, "batch preflight reports dispatchable count");
    expectTrue(batchPreflight.rejectedCount == 1u, "batch preflight reports rejected count");
    expectTrue(!batchPreflight.canSkip, "batch preflight canSkip false with dispatchable pair");
    expectTrue(batchPreflight.has_dispatchable(), "batch preflight has_dispatchable true");
    expectTrue(sleepingPreflight.isSleeping, "deepen preflight flags sleeping reject");
        dirtyPreflight.reason == fuse::physics::narrowphase::ManifoldPruneRejectReason::None,
            allSeparated, fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmpty),
void testFrictionBasisRebuildRejectReasonGuards() {
            empty, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::Skipped),
            needsBuild, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::None),
            needsBuild, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuse),
        reusePreflight.reason == fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuse,
                fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::Skipped),
    testContactPairDeepenPassRejectReasonGuards();
    testFrictionBasisRebuildRejectReasonGuards();

// --- deepen additive from deepen-b4-narrowphase-guard-pass-9852 ---
void testContactBufferWriteRejectGuards() {
            buffer, 0u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::None),
            buffer, 0u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
            buffer, 1u, invalid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
            buffer, 99u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
void testContactBufferCompactionClampRejectGuards() {
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::None),
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
                fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::CleanManifold),
            separated, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::NoPenetratingPoints),
void testContactPairShouldRunDispatchGuards() {
void testFrictionBasisRebuildRejectGuards() {
            empty, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::EmptyManifold),
            noNormal, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::NoValidNormal),
            withBasis, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuseBasis),
                fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuseBasis),

// --- deepen additive from b4-narrowphase-deepen-guards-046d ---
void testContactBufferDeepenPassPreflights() {
        fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer,
    const auto writePreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, valid);
    expectTrue(writePreflight.canWrite(), "write preflight allows valid manifold");
            buffer, 99u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidSlot),
        fuse::physics::narrowphase::contactBufferWriteRejectReason(buffer, 0u, selfPair) ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair,
        fuse::physics::narrowphase::preflightContactBufferClamp(buffer).reason ==
            fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity,
void testNarrowphaseBatchDeepenPassGuards() {
    expectTrue(batchPreflight.can_dispatch(), "batch preflight can dispatch mixed list");
void testManifoldPruneFinalizeCombinedPreflights() {
        !fuse::physics::narrowphase::should_skip_manifold_prune_finalize(ready),
void testFrictionBasisNormalizeCombinedPreflights() {
        !fuse::physics::narrowphase::should_skip_friction_basis_normalize_rebuild(unnormalized),
        fuse::physics::narrowphase::should_skip_friction_basis_normalize_rebuild(unit),
void testNarrowphaseDispatchPreflightGuards() {
    expectTrue(dispatchPreflight.can_dispatch(), "dispatch preflight allows valid pair list");
    expectTrue(skippedPreflight.skipped, "dispatch preflight skips empty pair list");
    testContactBufferDeepenPassPreflights();
    testManifoldPruneFinalizeCombinedPreflights();
    testFrictionBasisNormalizeCombinedPreflights();
    testNarrowphaseDispatchPreflightGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-bc5b ---
void testContactPairDeepenShouldRunGuards() {
void testContactBufferRejectReasonGuards() {
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRange),
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::None),
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
    testContactBufferRejectReasonGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-b130 ---
void testContactPairDeepenGuardPassHelpers() {
void testManifoldPruneFinalizeGuardPassHelpers() {
void testFrictionBasisRebuildGuardPassHelpers() {
                fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuseCached),
            withBasis, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuseCached),
void testContactBufferGuardPassHelpers() {
void testNarrowphasePairSlotGuardPassHelpers() {
    expectTrue(validPreflight.can_process(), "pair-slot preflight allows valid pair");
    expectTrue(validPreflight.canDispatch, "pair-slot preflight can dispatch valid pair");
        !fuse::physics::narrowphase::should_skip_narrowphase_pair_slot({bodyA, bodyB}, bodies, shapes),
        "should_skip pair-slot false for valid pair");
    expectTrue(!selfPreflight.can_process(), "pair-slot preflight rejects self pair");
    expectTrue(selfPreflight.pairRejected, "pair-slot preflight marks self pair rejected");
        fuse::physics::narrowphase::should_skip_narrowphase_pair_slot({bodyA, bodyA}, bodies, shapes),
        "should_skip pair-slot true for self pair");

// --- deepen additive from b4-narrowphase-deepen-guards-4d64 ---
    expectTrue(dispatchPreflight.can_dispatch(), "dispatch preflight allows mixed pair batch");
    expectTrue(dispatchPreflight.dispatchableCount == 1u, "dispatch preflight counts dispatchable pairs");
    expectTrue(emptyPreflight.skipped, "dispatch preflight skips empty pair list");
    expectTrue(!emptyPreflight.can_dispatch(), "dispatch preflight cannot dispatch empty list");
void testManifoldPruneFinalizeRejectReasonGuards() {
            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::AllValid),
            withBasis, fuse::physics::narrowphase::FrictionBasisRejectReason::CanReuse),
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::EmptyBuffer),
            buffer, 0u, invalid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
    testManifoldPruneFinalizeRejectReasonGuards();

// --- deepen additive from b4-narrowphase-guard-pass-0376 ---
void testContactPairDeepenDispatchSkipGuards() {
                fuse::physics::narrowphase::ManifoldPruneRejectReason::AllClean),
            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::AllClean),
            withBasis, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuse),
void testContactBufferWriteRejectReasonGuards() {
            buffer, 9u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidSlot),
void testContactBufferCompactionRejectReasonGuards() {
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllInvalid),
void testRunNarrowphaseDeepenIntoBufferGuards() {
    testContactBufferWriteRejectReasonGuards();
    testContactBufferCompactionRejectReasonGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-37c2 ---
void testContactPairGuardPassShouldRunHelpers() {
void testManifoldPruneFinalizeGuardPassShouldRun() {
void testFrictionBasisRebuildGuardPassShouldRun() {
        "should_skip preflight true for empty manifold");
void testContactBufferGuardPassPreflights() {
            fuse::physics::narrowphase::ContactBufferCompactRejectReason::AllValid),
            fuse::physics::narrowphase::ContactBufferCompactRejectReason::EmptyBuffer),
void testNarrowphasePairSlotPreflightGuards() {
    expectTrue(validPreflight.can_dispatch(), "pair-slot preflight allows valid pair");
        "should_skip pair slot false for valid pair");
    expectTrue(!selfPreflight.can_dispatch(), "pair-slot preflight rejects self pair");
        "should_skip pair slot true for self pair");
    testContactBufferGuardPassPreflights();
    testNarrowphasePairSlotPreflightGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-e6d2 ---
void testContactPairDeepenPassRejectHelpers() {
void testManifoldRejectReasonGuards() {
            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::AlreadyClean),
            empty, fuse::physics::narrowphase::FrictionBasisRejectReason::Skipped),
            needsBuild, fuse::physics::narrowphase::FrictionBasisRejectReason::CanReuse),
    const auto invalidSlotPreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 9u, valid);
    expectTrue(!invalidSlotPreflight.canWrite(), "write preflight rejects invalid slot");
    const auto validWritePreflight =
    expectTrue(validWritePreflight.canWrite(), "write preflight accepts valid manifold");
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity) == false,
    expectTrue(emptyPreflight.emptyPairs, "dispatch preflight marks empty pair list");
    expectTrue(emptyPreflight.can_skip_entire_dispatch(), "dispatch preflight can skip empty list");
    expectTrue(!emptyPreflight.can_prepare_buffer(), "dispatch preflight cannot prepare empty list");
    expectTrue(allRejectedPreflight.allPairsDeepenRejected, "dispatch preflight marks all deepen-rejected");
        allRejectedPreflight.dispatchablePairCount == 0u,
        fuse::physics::narrowphase::should_skip_narrowphase_pair_slot(sleepingPairs[0], bodies, shapes),
    expectTrue(!mixedPreflight.can_skip_entire_dispatch(), "dispatch preflight cannot skip mixed list");
    expectTrue(mixedPreflight.dispatchablePairCount == 1u, "dispatch preflight counts dispatchable pair");
        !fuse::physics::narrowphase::should_skip_narrowphase_pair_slot(mixedPairs[1], bodies, shapes),
    testManifoldRejectReasonGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-26f2 ---
    expectTrue(!batchPreflight.can_skip_dispatch(), "batch preflight does not skip mixed list");
        finalizePreflight.should_run_finalize(),
void testFrictionBasisFollowUpPreflights() {
        fuse::physics::narrowphase::should_skip_contact_buffer_compact(buffer),
        "should_skip_contact_buffer_compact on empty buffer");
    const auto compactionPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compact(buffer);
    expectTrue(compactionPreflight.needsCompaction, "compaction preflight flags sparse slots");
    expectTrue(compactionPreflight.should_run_compaction(), "compaction preflight should_run_compaction");
        "should_skip_contact_buffer_compact after compact");
    expectTrue(clampPreflight.needsClamp, "clamp preflight flags over-capacity buffer");
    expectTrue(clampPreflight.should_run_clamp(), "clamp preflight should_run_clamp");
        fuse::physics::narrowphase::should_skip_contact_buffer_friction_rebuild(buffer),
        "should_skip_contact_buffer_friction_rebuild with valid tangents");
    const auto staleFrictionPreflight =
    expectTrue(staleFrictionPreflight.needsRebuildCount == 1u, "friction preflight flags stale tangent slot");
        staleFrictionPreflight.should_run_rebuild(),
void testRunNarrowphaseIntoBufferDeepenGuards() {
    testFrictionBasisFollowUpPreflights();

// --- deepen additive from narrowphase-guard-pass-4c08 ---
void testContactPairGuardPassRejectGuards() {
    const auto deepenPreflight =
    expectTrue(deepenPreflight.bothSleeping, "deepen preflight flags bothSleeping");
    expectTrue(!deepenPreflight.can_dispatch(), "deepen preflight rejects both-sleeping pair");
void testManifoldPruneFinalizeGuardPass() {
void testFrictionBasisGuardPass() {
        reusePreflight.reason == fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuseBasis,
void testContactBufferGuardPass() {

// --- deepen additive from deepen-b4-narrowphase-guards-754b ---
void testContactPairBatchDeepenPreflightGuards() {
    expectTrue(batchPreflight.totalPairs == 3u, "batch preflight reports total pair count");
    expectTrue(batchPreflight.rejectedCount == 2u, "batch preflight counts rejected pairs");
    expectTrue(!batchPreflight.allRejected, "batch preflight not all-rejected with one dispatchable pair");
    expectTrue(batchPreflight.can_dispatch_any(), "batch preflight can dispatch any when one valid pair");
    const auto allRejectedPreflight = fuse::physics::narrowphase::preflight_contact_pair_batch_deepen(
    expectTrue(allRejectedPreflight.allRejected, "batch preflight all-rejected when none dispatchable");
    expectTrue(!allRejectedPreflight.can_dispatch_any(), "batch preflight cannot dispatch when all rejected");
void testManifoldProcessPreflightGuards() {
void testFrictionBasisRebuildUsingPreflightGuards() {
void testContactBufferWritePreflightGuards() {
void testRunNarrowphaseDeepenPreflightGuards() {
    testContactPairBatchDeepenPreflightGuards();
    testManifoldProcessPreflightGuards();
    testFrictionBasisRebuildUsingPreflightGuards();
    testContactBufferWritePreflightGuards();
    testRunNarrowphaseDeepenPreflightGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-c6ee ---
void testContactPairDeepenDispatchGuards() {
    expectTrue(validPreflight.can_write(), "write preflight allows valid manifold");
    expectTrue(!selfPreflight.can_write(), "write preflight rejects self pair");
    expectTrue(selfPreflight.selfPair, "write preflight flags self pair");
        fuse::physics::narrowphase::should_skip_contact_buffer_write(1u, buffer, selfPair),
        "should_skip_contact_buffer_write on self pair");
    const auto invalidPreflight =
    expectTrue(!invalidPreflight.can_write(), "write preflight rejects invalid manifold");
    expectTrue(invalidPreflight.invalidManifold, "write preflight flags invalid manifold");
void testManifoldFinalizeWithPreflightGuards() {
void testFrictionBasisRebuildWithPreflightGuards() {
    testManifoldFinalizeWithPreflightGuards();
    testFrictionBasisRebuildWithPreflightGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-7d67 ---
void testManifoldPruneFinalizeIfNeededGuards() {
void testFrictionBasisDeepenPassPreflightWrappers() {
    expectTrue(!invalidPreflight.canWrite(), "write preflight rejects self pair");
    expectTrue(!buffer.writeSlotWithPreflight(0u, selfPair), "write with preflight skips self pair");
    expectTrue(buffer.writeSlotWithPreflight(0u, valid), "write with preflight accepts valid manifold");
void testRunNarrowphaseDeepenPreflightDispatch() {
    fuse::physics::narrowphase::runNarrowphaseIntoBufferWithDeepenPreflight(mixedPairs, bodies, shapes, buffer);
    testFrictionBasisDeepenPassPreflightWrappers();
    testRunNarrowphaseDeepenPreflightDispatch();

// --- deepen additive from b4-narrowphase-deepen-guards-5d1f ---
            buffer, 4u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidSlot),
            allInvalid, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllInvalid),

// --- deepen additive from b4-narrowphase-deepen-guards-8a17 ---
        "can_skip deepen dispatch mirrors should_skip for sleeping pair");
            fuse::physics::narrowphase::NarrowphaseBatchRejectReason::None),
            fuse::physics::narrowphase::NarrowphaseBatchRejectReason::AllRejected),
            fuse::physics::narrowphase::NarrowphaseBatchRejectReason::EmptyPairList),
    expectTrue(batchPreflight.can_run(), "batch preflight can_run with dispatchable pair");
void testManifoldPruneFinalizePassGuards() {
void testFrictionBasisRebuildPassGuards() {
void testContactBufferCompactClampPassGuards() {
            buffer, fuse::physics::narrowphase::ContactBufferCompactRejectReason::EmptyBuffer),
void testContactBufferFrictionBuildPassGuards() {
            buffer, fuse::physics::narrowphase::ContactBufferFrictionBuildRejectReason::EmptyBuffer),
    const auto compactAndClampPreflight =
        compactAndClampPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork,

// --- deepen additive from b4-narrowphase-deepen-deea ---
    expectTrue(validPreflight.canWrite(), "write preflight accepts valid manifold");
        fuse::physics::narrowphase::contactBufferWriteRejectReason(buffer, 0u, valid) ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::None,
void testContactBufferCompactionPreflightGuards() {
            fuse::physics::narrowphase::contactBufferCompactionRejectReasonName(
void testContactBufferClampPreflightGuards() {
void testContactBufferCompactAndClampPreflightGuards() {
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer),
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork),
    testContactBufferCompactionPreflightGuards();
    testContactBufferClampPreflightGuards();
    testContactBufferCompactAndClampPreflightGuards();

// --- deepen additive from b4-narrowphase-deepen-guards-68c9 ---
        invalidSlotPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidSlot,
    const auto compactionPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compaction(packed);
    expectTrue(compactionPreflight.allValid, "compaction preflight marks all-valid slots");
void testContactBufferCompactionClampPreflights() {
    expectTrue(clampPreflight.needsClamp(), "clamp preflight requests overflow truncation");
    const auto noWorkPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compact_and_clamp(packed);
        noWorkPreflight.reason ==
void testNarrowphaseRunPreflightGuards() {
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_run_narrowphase(emptyPairs, bodies, shapes);
    expectTrue(emptyPreflight.emptyInput, "run preflight marks empty input");
    expectTrue(emptyPreflight.canSkip, "run preflight can skip empty input");
        fuse::physics::narrowphase::preflightNarrowphaseRun(validPairs, bodies, shapes);
    expectTrue(validPreflight.can_run(), "run preflight can run with valid pairs");
    expectTrue(validPreflight.batch.dispatchableCount == 1u, "run preflight carries batch dispatchable count");
    expectTrue(!deepenRejected.valid, "deepen preflight dispatch rejects both-sleeping pair");
void testManifoldShallowPrunePreflightGuards() {
            fuse::physics::narrowphase::ManifoldShallowPruneRejectReason::EmptyManifold,
            fuse::physics::narrowphase::ManifoldShallowPruneRejectReason::NoShallowPoints,
                fuse::physics::narrowphase::ManifoldShallowPruneRejectReason::NoShallowPoints),
void testFrictionComputeTangentsPreflightGuard() {
    testContactBufferCompactionClampPreflights();
    testNarrowphaseRunPreflightGuards();
    testManifoldShallowPrunePreflightGuards();
    testFrictionComputeTangentsPreflightGuard();

// --- deepen additive from deepen-b4-narrowphase-b46-8196 ---
    const auto emptyCompactionPreflight =
    expectTrue(emptyCompactionPreflight.emptyBuffer, "compaction preflight marks empty buffer");
        !emptyCompactionPreflight.needsCompaction(),
    const auto invalidWritePreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, selfPair);
    expectTrue(!invalidWritePreflight.canWrite(), "write preflight rejects self pair");
        invalidWritePreflight.reason ==
    const auto outOfRangeWritePreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 4u, valid);
    expectTrue(!outOfRangeWritePreflight.canWrite(), "write preflight rejects out-of-range slot");
        outOfRangeWritePreflight.reason ==
    fuse::physics::narrowphase::writeContactBufferSlotWithPreflight(buffer, 0u, valid);
    fuse::physics::narrowphase::writeContactBufferSlotWithPreflight(buffer, 1u, selfPair);
    fuse::physics::narrowphase::writeContactBufferSlotWithPreflight(buffer, 1u, deeper);
    const auto clampPreflight = fuse::physics::narrowphase::preflightContactBufferClamp(buffer);
    expectTrue(clampPreflight.needsClamp(), "clamp preflight needs clamp when over capacity");
        fuse::physics::narrowphase::clampContactBufferWithPreflight(buffer) == 1u,
            buffer, fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer),
        fuse::physics::narrowphase::compactAndClampContactBufferWithPreflight(buffer) == 0u,
    const auto runPreflight =
    expectTrue(runPreflight.can_run(), "run preflight can dispatch with one valid pair");
    expectTrue(runPreflight.batch.dispatchableCount == 1u, "run preflight reports dispatchable count");
        !fuse::physics::narrowphase::should_skip_narrowphase_run(mixedPairs, bodies, shapes),
        "should_skip run false when one pair dispatchable");
        fuse::physics::narrowphase::should_skip_narrowphase_run({{sleepingA, sleepingB}}, bodies, shapes),
        "should_skip run true when all pairs deepen-rejected");
        fuse::physics::narrowphase::should_skip_narrowphase_run({}, bodies, shapes),
        "should_skip run true for empty pair list");

// --- deepen additive from deepen-b4-narrowphase-6c66 ---
    expectTrue(validWritePreflight.canWrite(), "write preflight allows valid manifold");
    const auto outOfRangePreflight =
    expectTrue(!outOfRangePreflight.canWrite(), "write preflight rejects out-of-range slot");
    expectTrue(outOfRangePreflight.outOfRangeSlot, "write preflight flags out-of-range slot");
            allValidBuffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(buffer);
    expectTrue(noWorkPreflight.noWork, "compact-and-clamp preflight flags no-work buffer");
    const auto frictionPreflight = fuse::physics::narrowphase::preflightContactBufferFrictionBuild(buffer);
    expectTrue(frictionPreflight.canBuild(), "friction-build preflight allows valid contacts");
    fuse::physics::narrowphase::buildContactBufferFrictionTangentBasesWithPreflight(buffer);
void testContactManifoldBufferWritePreflightGuards() {
            empty, fuse::physics::narrowphase::ContactManifoldWriteRejectReason::EmptyManifold),
        fuse::physics::narrowphase::should_skip_contact_manifold_buffer_write(empty),
        "should_skip_manifold_buffer_write on empty manifold");
            noNormal, fuse::physics::narrowphase::ContactManifoldWriteRejectReason::InvalidNormal),
            notFinalized, fuse::physics::narrowphase::ContactManifoldWriteRejectReason::NotFinalized),
    testContactManifoldBufferWritePreflightGuards();

// --- deepen additive from b4-narrowphase-deepen-pass6-949f ---
void testContactPairDeepenPass6Guards() {
void testManifoldPruneFinalizePass6Guards() {
void testFrictionBasisPass6Preflights() {
    expectTrue(!invalidSlotPreflight.can_write(), "write preflight rejects invalid slot");
    expectTrue(buffer.writeSlotWithPreflight(0u, valid), "write_with_preflight accepts valid manifold");
    expectTrue(compactionPreflight.needs_compaction(), "compaction preflight needs work with invalid slot");
    expectTrue(buffer.compactWithPreflight() == 1u, "compact_with_preflight keeps valid slot");
    buffer.writeSlotWithPreflight(0u, valid);
    buffer.writeSlotWithPreflight(1u, deeper);
    expectTrue(buffer.compactWithPreflight() == 2u, "compact before clamp gathers both slots");
    expectTrue(clampPreflight.needs_clamp(), "clamp preflight needs overflow truncation");
    expectTrue(buffer.compactAndClampWithPreflight() == 1u, "compact_and_clamp_with_preflight truncates");
    const auto frictionPreflight = fuse::physics::narrowphase::preflight_contact_buffer_friction_basis(empty);
    expectTrue(!frictionPreflight.can_rebuild(), "friction preflight rejects empty buffer");
        buffer.buildFrictionTangentBasesWithPreflight(),
    testFrictionBasisPass6Preflights();

// --- deepen additive from deepen-b4-narrowphase-9067 ---
    expectTrue(writePreflight.can_write(), "contact-buffer write preflight allows valid manifold");
        !fuse::physics::narrowphase::should_skip_contact_buffer_write(buffer, 0u, valid),
            buffer, 1u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid,
    const auto clampPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(clampBuffer);
    expectTrue(clampPreflight.needs_clamp(), "contact-buffer clamp preflight needs clamp over capacity");
            emptyBuffer, fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer),
void testNarrowphaseIntoBufferPreflightGuards() {
    const auto slotPreflight =
    expectTrue(slotPreflight.can_dispatch(), "narrowphase pair-slot preflight allows valid pair");
    expectTrue(slotPreflight.canWriteSlot, "narrowphase pair-slot preflight can write slot");
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping) == 1u,
void testManifoldGenerateWithPreflightGuards() {
            overCap, fuse::physics::narrowphase::ManifoldPruneRejectReason::ExceedsMaxPoints),
                fuse::physics::narrowphase::ManifoldPruneRejectReason::ExceedsMaxPoints),
    testNarrowphaseIntoBufferPreflightGuards();
    testManifoldGenerateWithPreflightGuards();

// --- deepen additive from b4-narrowphase-deepen-e124 ---
    const auto compactionPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
        fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(empty).reason ==
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer,
    const auto frictionPreflight = fuse::physics::narrowphase::preflightContactBufferFrictionBases(buffer);
    expectTrue(frictionPreflight.needsRebuild(), "friction-bases preflight needs rebuild with valid slots");
        fuse::physics::narrowphase::buildContactBufferFrictionBasesWithPreflight(buffer),
        "buildFrictionBasesWithPreflight rebuilds tangents");
        fuse::physics::narrowphase::preflightNarrowphaseIntoBuffer(emptyPairs, bodies, shapes);
    expectTrue(emptyPreflight.emptyPairs, "into-buffer preflight marks empty pair list");
        fuse::physics::narrowphase::should_skip_narrowphase_into_buffer(emptyPairs, bodies, shapes),
        "should_skip_into_buffer on empty pair list");
        fuse::physics::narrowphase::preflightNarrowphaseIntoBuffer(validPairs, bodies, shapes);
    expectTrue(validPreflight.can_run(), "into-buffer preflight can run with valid pairs");
    expectTrue(validPreflight.dispatchableCount == 1u, "into-buffer preflight counts dispatchable pair");
    const auto slotPreflight = fuse::physics::narrowphase::preflight_narrowphase_pair_slot(
    expectTrue(!slotPreflight.can_dispatch(), "pair-slot preflight rejects self pair");
        slotPreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::SelfPair,
        fuse::physics::narrowphase::should_skip_narrowphase_pair_slot(0u, {bodyA, bodyA}, bodies, shapes),
        "should_skip_pair_slot on self pair");
void testFrictionBasisEnsureWithPreflight() {
    testFrictionBasisEnsureWithPreflight();

// --- deepen additive from deepen-b4-narrowphase-guards-1644 ---
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold,
    expectTrue(compactionPreflight.needsCompaction(), "compaction preflight needs compaction work");
    expectTrue(clampPreflight.needsClamp(), "clamp preflight needs clamp work");
    const auto tangentPreflight =
        tangentPreflight.needsFrictionTangentBuild(),
                fuse::physics::narrowphase::ContactBufferFrictionTangentRejectReason::AllValid),
    const auto validPreflight = fuse::physics::narrowphase::preflight_contact_pair_dispatch(
    expectTrue(!validPreflight.rejected, "dispatch preflight does not reject valid pair");
    const auto sleepingPreflight = fuse::physics::narrowphase::preflight_contact_pair_dispatch(
    expectTrue(!sleepingPreflight.can_dispatch(), "dispatch preflight rejects both-sleeping pair");
        fuse::physics::narrowphase::should_skip_contact_pair_dispatch_preflight(
        "should_skip dispatch preflight on rejected pair");
            fuse::physics::narrowphase::ManifoldShallowPruneRejectReason::NoShallowPenetrations,
    expectTrue(shallowPreflight.can_prune(), "shallow prune preflight allows mixed manifold");
    expectTrue(shallowPreflight.hasShallow, "shallow prune preflight detects shallow slot");
void testContactNormalNormalizePreflightGuards() {
            fuse::physics::narrowphase::ContactNormalNormalizeRejectReason::EmptyManifold),
            fuse::physics::narrowphase::ContactNormalNormalizeRejectReason::AlreadyUnit),
    const auto normalizePreflight =
    expectTrue(normalizePreflight.can_normalize(), "normalize preflight allows non-unit normal");
    expectTrue(dispatchPreflight.can_dispatch(), "dispatch preflight can dispatch with one valid pair");
    expectTrue(!dispatchPreflight.can_skip(), "dispatch preflight does not skip mixed batch");
        !fuse::physics::narrowphase::should_skip_narrowphase_dispatch(mixedPairs, bodies, shapes),
        "should_skip dispatch false for mixed batch");
        fuse::physics::narrowphase::should_skip_narrowphase_dispatch(
        "should_skip dispatch true when all pairs rejected");
    testContactNormalNormalizePreflightGuards();

// --- deepen additive from b4-narrowphase-deepen-d3be ---
            fuse::physics::narrowphase::ContactBufferWriteSlotRejectReason::SelfPair),
        fuse::physics::narrowphase::should_skip_contact_buffer_write_slot(buffer, 1u, selfPair),
        "should_skip write-slot on self pair");
void testNarrowphaseDeepenPassGuards() {

// --- deepen additive from deepen-narrowphase-b4-guards-61f0 ---
void testContactBufferClampRejectReasonGuards() {
    const auto withinPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(buffer);
        withinPreflight.withinCapacity,
void testContactPairPlanePlaneDeepenRejectGuards() {
void testDetectContactsPairWithPreflightGuard() {
void testNormalizeContactNormalIfNeededGuard() {
void testNarrowphaseBufferFinalizePreflightGuards() {
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_narrowphase_buffer_finalize(buffer);
    expectTrue(emptyPreflight.skipped, "buffer finalize preflight skips empty buffer");
    const auto dirtyPreflight = fuse::physics::narrowphase::preflight_narrowphase_buffer_finalize(buffer);
    expectTrue(!dirtyPreflight.skipped, "buffer finalize preflight does not skip prepared slots");
    expectTrue(dirtyPreflight.needsCompaction, "buffer finalize preflight needs compaction");
void testFrictionBasisNormalizeBeforeRebuildGuard() {
    testContactBufferClampRejectReasonGuards();
    testDetectContactsPairWithPreflightGuard();
    testNarrowphaseBufferFinalizePreflightGuards();

// --- deepen additive from deepen-b4-narrowphase-preflights-09a2 ---
    expectTrue(validPreflight.can_write(), "write preflight accepts valid manifold");
        validPreflight.reason == fuse::physics::narrowphase::ContactBufferWriteRejectReason::None,
        fuse::physics::narrowphase::should_skip_contact_buffer_write(buffer, 0u, invalid),
void testContactBufferCompactionClampPreflightGuards() {
    expectTrue(!compactionPreflight.needs_compaction(), "compaction preflight skips all-valid slots");
    expectTrue(compactionPreflight.allValid, "compaction preflight marks all-valid buffer");
    expectTrue(clampPreflight.needs_clamp(), "clamp preflight requests overflow truncation");
    const auto withinPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(withinBuffer);
    expectTrue(!withinPreflight.needs_clamp(), "clamp preflight skips when within capacity");
    expectTrue(withinPreflight.withinCapacity, "clamp preflight marks within-capacity buffer");
    testContactBufferCompactionClampPreflightGuards();

// --- deepen additive from b4-narrowphase-buffer-preflights-025e ---
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
    const fuse::physics::narrowphase::ContactBufferWritePreflight preflight =
        "should_skip false for valid manifold");
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferCompactionRejectReason::None),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferClampRejectReason::EmptyBuffer),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferClampRejectReason::None),
            buffer, fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork),
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::None),

// --- deepen additive from deepen-b4-narrowphase-guards-ef6e ---
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, invalid);
        fuse::physics::narrowphase::writeContactBufferSlotWithPreflight(buffer, 0u, valid),
        !fuse::physics::narrowphase::writeContactBufferSlotWithPreflight(buffer, 1u, selfPair),
        fuse::physics::narrowphase::preflightContactBufferClamp(clampBuffer);
        fuse::physics::narrowphase::preflightContactBufferClamp(withinBuffer);
    expectTrue(!withinPreflight.needsClamp(), "clamp preflight skips when within capacity");
    const auto compactClampPreflight =
        fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(compactClampBuffer);
    expectTrue(compactClampPreflight.needsCompactAndClamp(),
    expectTrue(noWorkPreflight.noWork, "compact-and-clamp preflight skips when already compact");
void testDeepenFollowUpPreflightWrappers() {
    testDeepenFollowUpPreflightWrappers();

// --- deepen additive from fuse-b4-narrowphase-deepen-5724 ---
    const auto emptyPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(emptyPreflight.emptyBuffer, "compaction preflight marks empty buffer");
    expectTrue(!emptyPreflight.needsCompaction(), "compaction preflight skips empty buffer");
    const auto allValidPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(allValidPreflight.allValid, "compaction preflight marks all-valid buffer");
    const auto withinPreflight = fuse::physics::narrowphase::preflightContactBufferClamp(buffer);
    const auto noWorkPreflight = fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(buffer);
    expectTrue(noWorkPreflight.noWork, "compactAndClamp preflight marks no-work after compact");
            fuse::physics::narrowphase::contactBufferCompactAndClampRejectReasonName(

// --- deepen additive from b4-narrowphase-deepen-guards-78d0 ---
    expectTrue(validPreflight.canWrite(), "write preflight allows valid manifold");
            buffer, 9u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
            fuse::physics::narrowphase::contactBufferClampRejectReasonName(

// --- deepen additive from deepen-b4-narrowphase-guards-b135 ---
        "writeSlotWithPreflight succeeds for valid manifold");
        !buffer.writeSlotWithPreflight(1u, invalid),
        "writeSlotWithPreflight rejects invalid manifold");
    const auto needsPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(needsPreflight.needsCompaction(), "compaction preflight needs work with holes");
    const auto cleanPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
        withinPreflight.reason ==
    const auto needsPreflight = fuse::physics::narrowphase::preflightContactBufferClamp(buffer);
    expectTrue(needsPreflight.needsClamp(), "clamp preflight needs work after compact overflow");
    const auto emptyPreflight = fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(buffer);
    const auto workPreflight = fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(buffer);
    expectTrue(workPreflight.needsCompactAndClamp(), "compact-and-clamp preflight needs work");
    const auto emptyPreflight = fuse::physics::narrowphase::preflightContactBufferFrictionRebuild(buffer);
            fuse::physics::narrowphase::ContactBufferFrictionRebuildRejectReason::EmptyBuffer,
    const auto orthonormalPreflight =
        fuse::physics::narrowphase::preflightContactBufferFrictionRebuild(buffer);
        orthonormalPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferFrictionRebuildRejectReason::AllOrthonormal,
    const auto stalePreflight = fuse::physics::narrowphase::preflightContactBufferFrictionRebuild(buffer);
    expectTrue(stalePreflight.needsRebuild(), "friction rebuild preflight needs stale slot");
void testContactPairBothPlaneDeepenRejectGuards() {
            fuse::physics::narrowphase::ContactPairRejectReason::BothPlane,
        fuse::physics::narrowphase::should_skip_contact_pair_deepen_dispatch({planeA, planeB}, bodies, shapes),
                fuse::physics::narrowphase::ContactPairRejectReason::BothPlane),
void testManifoldNormalizeContactNormalGuards() {

// --- deepen additive from b4-narrowphase-deepen-guards-ed7c ---
        "writeSlotWithPreflight stores valid manifold");
        !buffer.writeSlotWithPreflight(1u, selfPair),
        "writeSlotWithPreflight no-ops on self pair");
        buffer.compactAndClampWithPreflight() == 2u,
        "compactAndClampWithPreflight truncates to max capacity");
    expectTrue(emptyPreflight.emptyBuffer, "compact-and-clamp preflight marks empty buffer");
        buffer.compactAndClampWithPreflight() == 0u,
        "compactAndClampWithPreflight early-outs on empty buffer");
        "compactAndClampWithPreflight no-ops when already compact");
void testContactPairPlanePlaneDeepenGuards() {
        fuse::physics::narrowphase::should_skip_narrowphase_batch(pairs, bodies, shapes),
        "should_skip_narrowphase_batch true when all pairs rejected");
void testFinalizeContactManifoldIfNeededGuard() {
void testFrictionBasisStaleRejectGuards() {

// --- deepen additive from deepen-fuse-b4-narrowphase-0ba7 ---
    expectTrue(buffer.slotIsValid(0u), "writeSlotWithPreflight marks slot valid");
        "writeSlotWithPreflight rejects self pair");
        fuse::physics::narrowphase::should_skip_contact_buffer_write(buffer, 1u, invalid),
        "should_skip rejects invalid manifold");
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    const auto needsPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(needsPreflight.needsCompaction(), "compaction preflight needs work with hole");
    const auto allValidPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(allValidPreflight.allValid, "compaction preflight all-valid after compact");
    expectTrue(withinPreflight.withinCapacity, "clamp preflight within capacity before overflow");
    const auto needsClampPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(overflowBuffer);
    expectTrue(needsClampPreflight.needsClamp(), "clamp preflight needs work when over capacity");
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compact_and_clamp(buffer);
    const auto noWorkPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compact_and_clamp(buffer);
    expectTrue(noWorkPreflight.noWork, "compact-and-clamp preflight no-work when clean");
void testContactPairBothPlanesDeepenGuard() {
            fuse::physics::narrowphase::ContactPairRejectReason::BothPlanes,
void testManifoldNormalizeAndPruneIfNeededGuards() {
void testFrictionTangentsWithPreflightGuard() {
    testFrictionTangentsWithPreflightGuard();

// --- deepen additive from deepen-b4-narrowphase-guards-e78a ---
        fuse::physics::narrowphase::writeContactSlotWithPreflight(buffer, 0u, valid),
        !fuse::physics::narrowphase::writeContactSlotWithPreflight(buffer, 1u, invalid),
void testContactPairDeepenRejectedGuards() {
void testGenerateContactManifoldWithPreflightGuard() {
void testComputeFrictionTangentsWithPreflightGuard() {
    testGenerateContactManifoldWithPreflightGuard();
    testComputeFrictionTangentsWithPreflightGuard();

// --- deepen additive from deepen-narrowphase-b4-guards-1d13 ---
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 1u, selfPair);
    expectTrue(!selfPreflight.canWrite(), "write preflight rejects self pair");
        emptyPreflight.reason == fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer,
        fuse::physics::narrowphase::preflightContactBufferClamp(buffer).needsClamp(),
                fuse::physics::narrowphase::ContactPairRejectReason::BothPlanes),
void testNormalizeAndRebuildFrictionBasisPreflight() {
    testNormalizeAndRebuildFrictionBasisPreflight();

// --- deepen additive from deepen-b4-narrowphase-guards-f881 ---
        buffer.writeSlotIfPreflight(0u, valid),
        "writeSlotIfPreflight writes valid manifold");
        !buffer.writeSlotIfPreflight(1u, invalid),
        "writeSlotIfPreflight skips invalid manifold");
    expectTrue(noWorkPreflight.noWork, "compact-and-clamp preflight no-work on clean buffer");
void testContactPairBufferDispatchGuards() {
        fuse::physics::narrowphase::should_skip_contact_pair_for_buffer(
        !fuse::physics::narrowphase::should_skip_contact_pair_for_buffer(
    expectTrue(slotPreflight.can_dispatch(), "slot preflight allows valid pair");
    expectTrue(slotPreflight.canWrite, "slot preflight can write valid pair");
    expectTrue(rejectedPreflight.pairRejected, "slot preflight rejects sleeping pair");
    expectTrue(!rejectedPreflight.canWrite, "slot preflight cannot write rejected pair");
void testManifoldFinalizeDeepenPassGuards() {
            unnormalized, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::NeedsNormalNormalize),
void testRunNarrowphaseIfDispatchableGuard() {

// --- deepen additive from deepen-b4-narrowphase-2570 ---
        "should_skip false for valid manifold write");
        fuse::physics::narrowphase::should_skip_contact_buffer_write(buffer, 0u, selfPair),
        "should_skip true for self-pair write");
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::None,
            fuse::physics::narrowphase::ContactBufferClampRejectReason::EmptyBuffer,
            fuse::physics::narrowphase::ContactBufferClampRejectReason::None,
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::None,
    const auto afterPreflight =
        afterPreflight.reason ==

// --- deepen additive from b4-narrowphase-deepen-guards-cdff ---
    expectTrue(compactAndClampPreflight.needs_compact_and_clamp(),
void testContactBufferGuardedWriteAndCompact() {
void testFrictionBasisDeepenGuards() {

// --- deepen additive from deepen-b4-narrowphase-guards-56fd ---
void testContactPairBeyondDeepenRejectGuards() {
            fuse::physics::narrowphase::ContactPairRejectReason::UndispatchableShapePair,
    const auto beyondPreflight =
    expectTrue(!beyondPreflight.can_dispatch(), "beyond preflight rejects capsule-capsule pair");
        beyondPreflight.reason ==
        fuse::physics::narrowphase::should_skip_contact_pair_beyond_dispatch(
            fuse::physics::narrowphase::ContactPairRejectReason::UndispatchableShapePair),
void testManifoldBeyondPruneFinalizeGuards() {
        !fuse::physics::narrowphase::should_skip_manifold_beyond_prune(dirty),
        "should_skip_manifold_beyond_prune false when prune needed");
        fuse::physics::narrowphase::should_skip_manifold_beyond_prune(clean),
        "should_skip_manifold_beyond_prune true for clean manifold");
void testFrictionBasisBeyondRebuildGuards() {
    expectTrue(needsPreflight.needsRebuild, "beyond friction preflight needs rebuild without basis");
    expectTrue(!needsPreflight.can_skip_rebuild(), "beyond friction preflight cannot skip missing basis");
    expectTrue(stalePreflight.stale, "beyond friction preflight flags stale basis");
        !fuse::physics::narrowphase::should_skip_friction_basis_beyond_rebuild(stale),
void testRunNarrowphaseBeyondAndBufferGuards() {

// --- deepen additive from b4-narrowphase-deepen-pass-6859 ---
void testContactPairDeepenPass2RejectGuards() {
            fuse::physics::narrowphase::ContactPairRejectReason::NonCanonicalPair,
            fuse::physics::narrowphase::ContactPairRejectReason::DuplicatePairInBatch,
    expectTrue(!deepenPassPreflight.can_dispatch(), "deepen pass preflight rejects duplicate pair");
        deepenPassPreflight.reason ==
    expectTrue(slotPreflight.can_dispatch(), "slot preflight allows first canonical pair");
        slotPreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::None,
                fuse::physics::narrowphase::ContactPairRejectReason::NonCanonicalPair),
                fuse::physics::narrowphase::ContactPairRejectReason::DuplicatePairInBatch),
void testManifoldPruneFinalizePass2Guards() {
            unnormalized, fuse::physics::narrowphase::ManifoldPruneRejectReason::NeedsNormalNormalize),
            capped, fuse::physics::narrowphase::ManifoldPruneRejectReason::ExceedsMaxPoints),
                fuse::physics::narrowphase::ManifoldPruneRejectReason::NeedsNormalNormalize),
void testFrictionBasisPass2RejectGuards() {
void testContactBufferFrictionPass2Guards() {
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_buffer_friction_rebuild(empty);
    expectTrue(emptyPreflight.skipped, "empty buffer friction preflight skipped");
        fuse::physics::narrowphase::should_skip_buffer_friction_rebuild(empty),
        fuse::physics::narrowphase::should_skip_buffer_friction_rebuild(buffer),
void testNarrowphasePairSlotPass2Guards() {
    expectTrue(validPreflight.can_dispatch(), "pair slot preflight allows canonical pair");
        !fuse::physics::narrowphase::should_skip_narrowphase_pair_slot(pairs[0], bodies, shapes, pairs, 0u),
    const auto swappedPreflight =
    expectTrue(!swappedPreflight.can_dispatch(), "pair slot preflight rejects non-canonical pair");
        fuse::physics::narrowphase::should_skip_narrowphase_pair_slot(pairs[1], bodies, shapes, pairs, 1u),
        swappedPreflight.pair.reason ==

// --- deepen additive from deepen-b4-narrowphase-guards-d666 ---
void testShouldRunContactPairDispatchGuards() {
void testNarrowphaseDispatchRejectReasonGuards() {
            fuse::physics::narrowphase::NarrowphaseDispatchRejectReason::EmptyPairList),
            fuse::physics::narrowphase::NarrowphaseDispatchRejectReason::AllPairsRejected),
        batchPreflight.reason ==
            fuse::physics::narrowphase::NarrowphaseDispatchRejectReason::AllPairsRejected,
    expectTrue(!batchPreflight.can_dispatch(), "batch preflight cannot dispatch all-rejected list");
            buffer, fuse::physics::narrowphase::ContactBufferCompactRejectReason::AllValid),
            clean, fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork),
void testShouldRunManifoldPruneGuards() {
void testShouldRunFrictionBasisRebuildGuards() {
        "should_skip_friction_basis_preflight true with valid cached basis");
    testNarrowphaseDispatchRejectReasonGuards();

// --- deepen additive from b4-narrowphase-deepen-guards-68a1 ---
void testContactPairBothPlaneDeepenGuards() {
void testShouldRunNarrowphaseDispatchGuards() {
void testManifoldPruneInvalidNormalGuards() {
            fuse::physics::narrowphase::ManifoldPruneRejectReason::InvalidNormal,
                fuse::physics::narrowphase::ManifoldPruneRejectReason::InvalidNormal),
void testShouldRunManifoldPruneFinalizeGuards() {
void testFrictionBasisShouldRunAndComputePreflightGuards() {
            fuse::physics::narrowphase::ContactBufferCompactRejectReason::EmptyBuffer,
            fuse::physics::narrowphase::ContactBufferCompactRejectReason::NoWork,
            buffer, fuse::physics::narrowphase::ContactBufferCompactRejectReason::NoWork),
    testFrictionBasisShouldRunAndComputePreflightGuards();

// --- deepen additive from b4-narrowphase-deepen-guards-01df ---
    const auto basePreflight =
    expectTrue(basePreflight.can_dispatch(), "base detect preflight allows valid pair");
    expectTrue(!basePreflight.usesDeepenReject, "base detect preflight does not use deepen reject");
    expectTrue(!deepenPreflight.can_dispatch(), "deepen detect preflight rejects both-sleeping pair");
    expectTrue(deepenPreflight.usesDeepenReject, "deepen detect preflight records deepen path");
        fuse::physics::narrowphase::should_skip_detect_contacts_pair(
        "should_skip_detect flags self pair");
void testContactBufferWriteCompactPreflightGuards() {
    const auto compactPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compact(buffer);
    expectTrue(compactPreflight.can_compact(), "compact preflight allows buffer with valid slot");
    expectTrue(buffer.compactWithPreflight() == 1u, "compactWithPreflight gathers valid slot");
            empty, fuse::physics::narrowphase::ContactBufferCompactRejectReason::AllInvalid),
    expectTrue(empty.compactWithPreflight() == 0u, "compactWithPreflight clears all-invalid buffer");
                fuse::physics::narrowphase::ContactBufferCompactRejectReason::AllInvalid),
    clampBuffer.writeSlotWithPreflight(0u, shallow);
    clampBuffer.writeSlotWithPreflight(1u, deep);
        clampBuffer.compactAndClampWithPreflight() == 1u,
        "compactAndClampWithPreflight compacts then clamps");
void testManifoldNormalizeAndChainFinalizeGuards() {
void testFrictionBasisComputeWithPreflightGuards() {
    testContactBufferWriteCompactPreflightGuards();
    testFrictionBasisComputeWithPreflightGuards();

// --- deepen additive from deepen-narrowphase-guards-f8e8 ---
void testContactPairDeepenPassB46Guards() {
            fuse::physics::narrowphase::ContactPairRejectReason::BothNoGravity,
            fuse::physics::narrowphase::ContactPairRejectReason::BothCcd,
    expectTrue(deepenPreflight.base.dispatchableCount == 1u, "batch deepen preflight counts dispatchable pairs");
    expectTrue(deepenPreflight.noGravityRejectedCount == 1u, "batch deepen preflight counts no-gravity rejects");
                fuse::physics::narrowphase::ContactPairRejectReason::BothNoGravity),
                fuse::physics::narrowphase::ContactPairRejectReason::BothCcd),
void testManifoldPruneFinalizeB46Guards() {
void testFrictionBasisB46Guards() {

// --- deepen additive from deepen-b4-narrowphase-guards-5ef6 ---
            buffer, fuse::physics::narrowphase::ContactBufferFrictionBasisRejectReason::EmptyBuffer),
            buffer, 1u, manifold, fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),

// --- deepen additive from deepen-narrowphase-b4-guards-2406 ---
void testContactPairDeepenSecondRejectGuards() {
            fuse::physics::narrowphase::ContactPairRejectReason::MeshShapePair,
            fuse::physics::narrowphase::ContactPairRejectReason::BoxThinPair,
    const auto secondPreflight =
    expectTrue(secondPreflight.can_dispatch(), "second preflight allows valid pair");
        !fuse::physics::narrowphase::should_skip_contact_pair_deepen_second_dispatch({dynamicA, dynamicB}, bodies, shapes),
                fuse::physics::narrowphase::ContactPairRejectReason::MeshShapePair),
                fuse::physics::narrowphase::ContactPairRejectReason::BoxThinPair),
void testNarrowphaseBatchSecondPreflightGuards() {
    expectTrue(batchPreflight.pairCount == 2u, "second batch preflight reports pair count");
    expectTrue(batchPreflight.dispatchableCount == 1u, "second batch preflight counts dispatchable pairs");
    expectTrue(batchPreflight.rejectedCount == 1u, "second batch preflight counts rejected pairs");
void testManifoldPruneFinalizeSecondGuards() {
            fuse::physics::narrowphase::ManifoldPruneRejectReason::ExceedsMaxPoints,
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::NeedsNormalNormalize,
void testFrictionBasisSecondRebuildGuards() {
        !fuse::physics::narrowphase::should_skip_friction_basis_second_preflight(stale),
void testContactBufferSecondPreflightGuards() {
        "writeSlotWithPreflight accepts valid manifold");
    expectTrue(buffer.compactWithPreflight() == 2u, "compactWithPreflight gathers valid slots");
            full, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
    expectTrue(buffer.compactAndClampWithPreflight() == 1u, "compactAndClampWithPreflight clamps to max");
    expectTrue(buffer.droppedCount == 1u, "compactAndClampWithPreflight tracks dropped contacts");
            empty, fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer),
    testNarrowphaseBatchSecondPreflightGuards();
    testContactBufferSecondPreflightGuards();

// --- deepen additive from b4-narrowphase-deepen-guards-9857 ---
void testNarrowphasePairDispatchPreflightGuards() {
    expectTrue(validPreflight.can_dispatch(), "pair dispatch preflight allows valid pair");
    expectTrue(!sleepingPreflight.can_dispatch(), "pair dispatch preflight rejects both-sleeping pair");
    expectTrue(validPreflight.can_write(), "buffer write preflight allows valid manifold");
    expectTrue(buffer.writeSlotWithPreflight(0u, valid), "writeSlotWithPreflight accepts valid slot");
void testContactBufferFrictionBuildPreflightGuards() {
            empty, fuse::physics::narrowphase::ContactBufferFrictionBuildRejectReason::EmptyBuffer),
    const auto validPreflight = fuse::physics::narrowphase::preflight_contact_buffer_friction_build(buffer);
            fuse::physics::narrowphase::ContactBufferFrictionBuildRejectReason::AllBasesValid,
void testRunNarrowphaseWithDeepenGuards() {
    testNarrowphasePairDispatchPreflightGuards();
    testContactBufferFrictionBuildPreflightGuards();

// --- deepen additive from deepen-narrowphase-b4-guards-3d95 ---
void testContactPairNoDispatchPathGuards() {
            fuse::physics::narrowphase::ContactPairRejectReason::NoDispatchPath,
                fuse::physics::narrowphase::ContactPairRejectReason::NoDispatchPath),
void testNarrowphaseSlotPreflightGuards() {
    expectTrue(validPreflight.can_dispatch(), "slot preflight allows valid pair");
        !fuse::physics::narrowphase::should_skip_narrowphase_slot_dispatch({dynamicA, dynamicB}, bodies, shapes),
    const auto undispatchedPreflight =
    expectTrue(!undispatchedPreflight.can_dispatch(), "slot preflight rejects undispatched pair");
        undispatchedPreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::NoDispatchPath,
        fuse::physics::narrowphase::should_skip_narrowphase_slot_dispatch({boxBody, planeBody}, bodies, shapes),
void testManifoldNormalizeAndIfNeededGuards() {
    const auto stalePreflight = fuse::physics::narrowphase::preflight_contact_buffer_friction_rebuild(buffer);
    expectTrue(stalePreflight.needs_rebuild(), "buffer friction preflight needs rebuild for stale slot");
    expectTrue(stalePreflight.staleCount == 1u, "buffer friction preflight counts one stale slot");
    testNarrowphaseSlotPreflightGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-fd4e ---
void testContactBufferDeepenFollowUpGuards() {
        !fuse::physics::narrowphase::writeSlotWithPreflight(0u, selfPair, buffer),
        fuse::physics::narrowphase::writeSlotWithPreflight(0u, valid, buffer),
        fuse::physics::narrowphase::compactWithPreflight(buffer) == 2u,
        fuse::physics::narrowphase::compactAndClampWithPreflight(buffer) == 1u,
void testContactPairDeepenFollowUpDispatchGuards() {
void testManifoldGenerateDeepenFollowUpGuards() {
void testFrictionBasisDeepenFollowUpGuards() {

// --- deepen additive from b4-narrowphase-b46-deepen-ee53 ---
void testContactPairDeepenB46RejectGuards() {
            fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedMeshPair,
            fuse::physics::narrowphase::ContactPairRejectReason::DegeneratePlaneNormal,
            fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedMeshPair) == 2u,
            fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedMeshPair),
                fuse::physics::narrowphase::ContactPairRejectReason::DegeneratePlaneNormal),
            ready, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::AlreadyFinalized),
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::AlreadyFinalized),
void testFrictionBasisB46RejectGuards() {
void testContactBufferB46Guards() {

// --- deepen additive from b4-narrowphase-guards-91b3 ---
        validWrite.reason == fuse::physics::narrowphase::ContactBufferWriteRejectReason::None,
            fuse::physics::narrowphase::ContactBufferFrictionRebuildRejectReason::EmptyBuffer),
    expectTrue(emptyPreflight.can_skip(), "into-buffer preflight skips empty pair list");
    expectTrue(validPreflight.can_dispatch(), "into-buffer preflight dispatches non-empty pair list");
    expectTrue(validPreflight.batchPreflight.dispatchableCount == 1u,
void testDetectContactsPairDeepenGuards() {
void testFinalizeContactManifoldIfNeededDeepenGuard() {
                fuse::physics::narrowphase::FrictionBasisRejectReason::StaleNormal),

// --- deepen additive from deepen-narrowphase-b4-guards-b5a3 ---
        validWritePreflight.reason == fuse::physics::narrowphase::ContactBufferWriteRejectReason::None,
    const auto selfWritePreflight =
    expectTrue(!selfWritePreflight.canWrite(), "write preflight rejects self pair");
    expectTrue(selfWritePreflight.selfPair, "write preflight marks self pair");
    expectTrue(!invalidWritePreflight.canWrite(), "write preflight rejects invalid manifold");
    expectTrue(invalidWritePreflight.invalidManifold, "write preflight marks invalid manifold");
    expectTrue(outOfRangePreflight.outOfRangeSlot, "write preflight marks out-of-range slot");
    expectTrue(compactionPreflight.needsCompaction(), "compaction preflight needs compaction after invalidation");
    expectTrue(!allValidPreflight.needsCompaction(), "compaction preflight skips all-valid buffer");
    expectTrue(!emptyCompactionPreflight.needsCompaction(), "compaction preflight skips empty buffer");
            emptyBuffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
    expectTrue(clampPreflight.needsClamp(), "clamp preflight needs clamp when over max");
    const auto withinClampPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(clampBuffer);
    expectTrue(!withinClampPreflight.needsClamp(), "clamp preflight skips within-capacity buffer");
    expectTrue(withinClampPreflight.withinCapacity, "clamp preflight marks within-capacity buffer");
    expectTrue(noWorkPreflight.noWork, "compactAndClamp preflight marks no-work buffer");
    expectTrue(emptyPreflight.can_skip(), "dispatch preflight can skip empty pair list");
        fuse::physics::narrowphase::should_skip_narrowphase_dispatch({}, bodies, shapes),
        "should_skip_narrowphase_dispatch on empty pair list");
    expectTrue(!validPreflight.emptyPairs, "dispatch preflight does not mark non-empty pair list");
    expectTrue(validPreflight.can_dispatch(), "dispatch preflight can dispatch valid pair");
        !fuse::physics::narrowphase::should_skip_narrowphase_dispatch(validPairs, bodies, shapes),
        "should_skip_narrowphase_dispatch false for dispatchable pair");

// --- deepen additive from deepen-b4-narrowphase-guards-aa41 ---
            buffer, 5u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
    expectTrue(compactionPreflight.needsCompaction(), "compaction preflight needs work for sparse slots");
void testContactPairUndispatchedShapeGuards() {

// --- deepen additive from b4-narrowphase-deepen-ffe6 ---
void testContactBufferGuardHelpers() {
        fuse::physics::narrowphase::should_skip_contact_buffer_iteration(buffer),
        "should_skip on empty buffer");
    expectTrue(emptyPreflight.emptyPairList, "dispatch preflight flags empty pair list");
    expectTrue(emptyPreflight.canSkipDispatch, "dispatch preflight skips empty pair list");
    expectTrue(!validPreflight.canSkipDispatch, "dispatch preflight allows valid pair");
    expectTrue(validPreflight.batch.dispatchableCount == 1u, "dispatch preflight counts dispatchable pair");
void testManifoldFinalizeIfNotFinalizedGuards() {
void testFrictionNormalizeAndRebuildGuards() {

// --- deepen additive from b4-narrowphase-deepen-guards-69fc ---
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 2u, valid);
        outOfRangePreflight.reason ==
    expectTrue(!invalidPreflight.canWrite(), "write preflight rejects invalid manifold");
        invalidPreflight.reason ==
        selfPreflight.reason == fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair,
void testContactBufferCompactionGuards() {
void testContactBufferClampAndCapacityGuards() {
void testContactBufferFrictionBasisPreflightGuards() {
    fuse::physics::narrowphase::buildFrictionTangentBasesWithPreflight(buffer);
void testContactPairDeepenPassSixGuards() {
void testManifoldFinalizeIfNeededGuards() {
void testCanSkipNarrowphaseDispatchGuard() {
    testContactBufferFrictionBasisPreflightGuards();

// --- deepen additive from b4-narrowphase-buffer-guards-0bb9 ---
            fuse::physics::narrowphase::contactBufferWriteSlotRejectReasonName(
                fuse::physics::narrowphase::ContactBufferWriteSlotRejectReason::InvalidManifold),
    expectTrue(!compactionPreflight.needsCompaction(), "preflight reports all-valid slot flags before activeCount sync");
    const auto packedPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(!packedPreflight.needsCompaction(), "preflight skips already-packed buffer");
    expectTrue(clampPreflight.needsClamp(), "preflight needs clamp when over max capacity");
    const auto noClampPreflight = fuse::physics::narrowphase::preflightContactBufferClamp(buffer);
    expectTrue(!noClampPreflight.needsClamp(), "preflight skips clamp within capacity");
void testContactBufferCompactAndClampPreflights() {
        noWorkPreflight.reason == fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork,
void testContactBufferFrictionWarmStartExportGuards() {
            buffer, 0u, fuse::physics::narrowphase::ContactBufferWarmStartRejectReason::OutOfRangeSlot),
        frictionPreflight.reason == fuse::physics::narrowphase::ContactBufferFrictionBuildRejectReason::AllCached,
    const auto exportPreflight = fuse::physics::narrowphase::preflightContactBufferToVector(buffer);
    expectTrue(exportPreflight.canExport(), "toVector preflight allows non-empty buffer");
void testNarrowphaseBufferDispatchPreflight() {
    expectTrue(emptyPreflight.emptyPairList, "buffer dispatch preflight marks empty pair list");
    expectTrue(validPreflight.can_dispatch(), "buffer dispatch preflight allows non-empty pair list");
    expectTrue(validPreflight.canFinalizeBuffer, "buffer dispatch preflight can finalize buffer");
void testDetectContactsPairDeepenPreflightWrapper() {
void testManifoldNormalizeAndFrictionPreflightWrappers() {
    testContactBufferCompactAndClampPreflights();
    testNarrowphaseBufferDispatchPreflight();
    testDetectContactsPairDeepenPreflightWrapper();
    testManifoldNormalizeAndFrictionPreflightWrappers();

// --- deepen additive from deepen-narrowphase-b4-guards-5b57 ---
    expectTrue(validWritePreflight.can_write(), "write preflight accepts valid manifold");
            fuse::physics::narrowphase::ContactBufferWriteSlotRejectReason::OutOfRangeSlot),
    expectTrue(clampPreflight.needs_clamp(), "clamp preflight needs clamp when over capacity");
void testContactBufferToVectorAndFrictionPreflightGuards() {
            fuse::physics::narrowphase::ContactBufferToVectorRejectReason::EmptyBuffer),
    expectTrue(frictionPreflight.needs_friction_basis_build(), "friction preflight needs build with valid contacts");
void testNarrowphaseBufferDispatchDeepenGuards() {
    const auto deepenPreflight = fuse::physics::narrowphase::preflight_narrowphase_buffer_dispatch(
    expectTrue(deepenPreflight.skipped, "deepen buffer preflight skipped when all rejected");
    expectTrue(deepenPreflight.allRejected, "deepen buffer preflight flags allRejected");
    const auto validPreflight = fuse::physics::narrowphase::preflight_narrowphase_buffer_dispatch(
    expectTrue(validPreflight.can_dispatch(), "deepen buffer preflight dispatches valid pair");
    expectTrue(validPreflight.dispatchableCount == 1u, "deepen buffer preflight counts dispatchable pair");
    testContactBufferToVectorAndFrictionPreflightGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-d1c7 ---
        fuse::physics::narrowphase::preflightContactBufferWriteSlot(buffer, 0u, valid);
        fuse::physics::narrowphase::preflightContactBufferWriteSlot(buffer, 0u, selfPair);
            buffer, 0u, selfPair, fuse::physics::narrowphase::ContactBufferWriteSlotRejectReason::SelfPair),
        emptyCompactionPreflight.reason == fuse::physics::narrowphase::ContactBufferCompactionRejectReason::None,
    const auto clearedCompactionPreflight =
        fuse::physics::narrowphase::preflightContactBufferCompaction(clearedBuffer);
        clearedCompactionPreflight.reason ==
        allValidPreflight.reason == fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid,
void testContactBufferClampAndCompactPreflightGuards() {
        withinPreflight.reason == fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity,
        fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(compactBuffer);
        compactAndClampPreflight.needsCompactAndClamp(),
        fuse::physics::narrowphase::compactContactBufferWithPreflight(compactBuffer) == 1u,
    const auto emptyExport = fuse::physics::narrowphase::preflightContactBufferToVector(buffer);
            buffer, fuse::physics::narrowphase::ContactBufferToVectorRejectReason::EmptyBuffer),
    expectTrue(exportPreflight.canExport(), "toVector preflight allows populated buffer");
    const auto frictionPreflight = fuse::physics::narrowphase::preflightContactBufferFrictionBasis(buffer);
    expectTrue(frictionPreflight.canBuild(), "friction preflight can build on valid contacts");
    fuse::physics::narrowphase::buildContactBufferFrictionBasesWithPreflight(buffer);
    const auto noValidPreflight = fuse::physics::narrowphase::preflightContactBufferFrictionBasis(buffer);
        noValidPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferFrictionBasisRejectReason::NoValidContacts,
void testContactBufferWriteSlotWithPreflight() {
        !fuse::physics::narrowphase::writeContactBufferSlotWithPreflight(buffer, 0u, invalid),
void testNarrowphaseIntoBufferSkipGuard() {
void testContactPairDeepenDispatchWithPreflight() {
void testGenerateContactManifoldWithPreflight() {
void testComputeFrictionTangentsWithPreflight() {
    testContactBufferClampAndCompactPreflightGuards();
    testContactBufferWriteSlotWithPreflight();
    testContactPairDeepenDispatchWithPreflight();
    testGenerateContactManifoldWithPreflight();
    testComputeFrictionTangentsWithPreflight();

// --- deepen additive from b4-narrowphase-deepen-guards-8627 ---
void testContactBufferDeepenGuardHelpers() {
            buffer, 4u, valid, fuse::physics::narrowphase::ContactBufferWriteSlotRejectReason::OutOfRangeSlot),
            buffer, 0u, invalid, fuse::physics::narrowphase::ContactBufferWriteSlotRejectReason::InvalidManifold),
    expectTrue(writePreflight.canWrite(), "write-slot preflight accepts valid manifold");
void testContactPairManifoldFrictionDeepenRunGuards() {

// --- deepen additive from b4-narrowphase-deepen-guards-bee6 ---
            buffer, 99u, valid, fuse::physics::narrowphase::ContactBufferWriteSlotRejectReason::OutOfRangeSlot),
            buffer, 99u, fuse::physics::narrowphase::ContactBufferWarmStartRejectReason::OutOfRangeSlot),
void testContactPairManifoldFrictionDeepenPassGuards() {

// --- deepen additive from b4-narrowphase-deepen-c201 ---
void testContactBufferSoAGuards() {
            fuse::physics::narrowphase::ContactBufferFrictionTangentRejectReason::AllOrthonormal),
void testNarrowphaseIntoBufferGuards() {
    expectTrue(skippedPreflight.can_skip(), "into-buffer preflight skips all-rejected batch");
    expectTrue(skippedPreflight.skipped, "into-buffer preflight marks skipped");
void testContactPairDetectPreflightGuards() {
    expectTrue(validPreflight.can_detect(), "detect preflight allows valid pair");
    expectTrue(!selfPreflight.can_detect(), "detect preflight rejects self pair");
            fuse::physics::narrowphase::ContactPairDetectRejectReason::RejectedPair),
void testManifoldFrictionPreflightSkipGuards() {
    testContactPairDetectPreflightGuards();
    testManifoldFrictionPreflightSkipGuards();

// --- deepen additive from b4-narrowphase-deepen-8324 ---
void testContactBufferSoAGuardPreflights() {
    expectTrue(writePreflight.can_write(), "write preflight allows valid manifold in range");
    expectTrue(!clampPreflight.needs_clamp(), "clamp preflight skips within-capacity buffer");
            exportBuffer, fuse::physics::narrowphase::ContactBufferToVectorRejectReason::EmptyBuffer),
    const auto dispatchPreflight = fuse::physics::narrowphase::preflight_run_narrowphase(
    expectTrue(dispatchPreflight.batch.dispatchableCount == 1u, "dispatch preflight counts dispatchable pairs");
    expectTrue(dispatchPreflight.can_dispatch(), "dispatch preflight can dispatch mixed batch");
    fuse::physics::narrowphase::runNarrowphaseIntoBufferWithPreflight(
    testContactBufferSoAGuardPreflights();

// --- deepen additive from b4-narrowphase-deepen-guards-af6c ---
    const auto manifoldPreflight =
    expectTrue(manifoldPreflight.can_read(), "contact buffer manifoldAt preflight allows valid index");
    expectTrue(dispatchPreflight.can_dispatch(), "narrowphase dispatch preflight allows valid pair");
void testContactPairManifoldDeepenFollowupGuards() {
        fuse::physics::narrowphase::should_skip_contact_pair_deepen_followup_dispatch({planeA, planeB}, bodies, shapes),
void testFrictionDeepenFollowupGuards() {

// --- deepen additive from b4-narrowphase-deepen-guards-ea87 ---
void testContactBufferSoAGuardHelpers() {
        !buffer.writeSlotWithPreflight(99u, valid),
        "writeSlotWithPreflight rejects out-of-range slot");
    expectTrue(buffer.compactWithPreflight() == 1u, "compactWithPreflight compacts valid slot");
    expectTrue(singleSlotBuffer.compactWithPreflight() == 1u, "single-slot compact succeeds");
        "compactAndClampWithPreflight clamps to max capacity");
        buffer.applyWarmStartStubWithPreflight(0u, valid),
        "applyWarmStartStubWithPreflight copies warm-start data");
        !buffer.applyWarmStartStubWithPreflight(99u, valid),
        "applyWarmStartStubWithPreflight rejects out-of-range slot");
void testContactPairDeepenPreflightDetectGuards() {
void testManifoldFinalizeAfterPruneGuards() {
        shallowPreflight.can_skip_shallow_prune(0.05f),
void testFrictionBasisPreflightWrapperGuards() {
    testContactPairDeepenPreflightDetectGuards();
    testFrictionBasisPreflightWrapperGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-bfb5 ---
            fuse::physics::narrowphase::ContactBufferWriteSlotRejectReason::None),
            buffer, fuse::physics::narrowphase::ContactBufferToVectorRejectReason::None),
void testManifoldPruneFinalizeFollowUpCanSkipGuards() {
void testFrictionBasisFollowUpCanSkipGuards() {

// --- deepen additive from b4-narrowphase-deepen-guards-04be ---
            buffer, 2u, valid, fuse::physics::narrowphase::ContactBufferWriteSlotRejectReason::OutOfRangeSlot),
            buffer, fuse::physics::narrowphase::ContactBufferFrictionBasisRejectReason::AllValid),
    const auto validPreflight = fuse::physics::narrowphase::preflight_detect_contacts_pair(
    expectTrue(validPreflight.can_detect(), "detect preflight accepts valid pair");
    const auto rejectedPreflight = fuse::physics::narrowphase::preflight_detect_contacts_pair(
    expectTrue(!rejectedPreflight.can_detect(), "detect preflight rejects any-trigger pair");
void testManifoldPruneAndFinalizePreflightGuards() {
    expectTrue(readyPreflight.can_prune_and_finalize(), "combined preflight accepts ready manifold");
void testFrictionComputeTangentsPreflightGuards() {
    expectTrue(dispatchPreflight.can_dispatch(), "dispatch preflight can dispatch valid pair");
    testManifoldPruneAndFinalizePreflightGuards();
    testFrictionComputeTangentsPreflightGuards();

// --- deepen additive from deepen-b4-narrowphase-guards-950d ---
void testContactBufferSoAIterationGuards() {
void testContactBufferWriteSlotPreflightGuards() {
void testContactBufferClampAndExportGuards() {
        fuse::physics::narrowphase::compactAndClampContactBufferWithPreflight(buffer);
void testContactPairDeepenPassPlanePlaneGuards() {
void testManifoldNormalizeDeepenPassGuards() {
            empty, fuse::physics::narrowphase::ManifoldNormalizeRejectReason::EmptyManifold),
    const auto pruneFinalizePreflight =
    expectTrue(pruneFinalizePreflight.can_finalize(), "prune/finalize preflight can finalize normalized manifold");
void testFrictionTangentComputeDeepenPassGuards() {
            empty, fuse::physics::narrowphase::FrictionTangentComputeRejectReason::EmptyManifold),
            withBasis, fuse::physics::narrowphase::FrictionTangentComputeRejectReason::CachedBasis),
                fuse::physics::narrowphase::FrictionTangentComputeRejectReason::CachedBasis),
void testRunNarrowphaseBufferPreflightGuards() {
    testContactBufferWriteSlotPreflightGuards();
    testRunNarrowphaseBufferPreflightGuards();

// --- deepen additive from b4-narrowphase-deepen-guards-7827 ---
    expectTrue(dispatchPreflight.can_dispatch(), "dispatch preflight can dispatch valid batch");
    expectTrue(!dispatchPreflight.can_skip(), "dispatch preflight does not skip valid batch");
void testManifoldFrictionDeepenPassFollowUpGuards() {

// --- deepen additive from deepen-b4-narrowphase-guards-5f30 ---
void testContactBufferWriteSlotRejectReasonGuards() {
            fuse::physics::narrowphase::ContactBufferWriteSlotRejectReason::None,
            fuse::physics::narrowphase::ContactBufferWriteSlotRejectReason::OutOfRangeSlot,
            fuse::physics::narrowphase::ContactBufferWriteSlotRejectReason::InvalidManifold,
            fuse::physics::narrowphase::ContactBufferWriteSlotRejectReason::SelfPair,
void testContactBufferToVectorAndCompactionGuards() {
            fuse::physics::narrowphase::ContactBufferToVectorRejectReason::EmptyBuffer,
            fuse::physics::narrowphase::ContactBufferToVectorRejectReason::None,
void testContactBufferWarmStartAndFrictionSkipGuards() {
            fuse::physics::narrowphase::ContactBufferWarmStartRejectReason::OutOfRangeSlot,
            fuse::physics::narrowphase::ContactBufferWarmStartRejectReason::InvalidSlot,
void testRunNarrowphaseIntoBufferPreflightGuards() {
    fuse::physics::narrowphase::runNarrowphaseIntoBufferWithPreflight(rejectedPairs, bodies, shapes, buffer);
void testManifoldFinalizePreflightWrapperGuards() {
void testFrictionTangentsPreflightWrapperGuards() {
    testContactBufferWriteSlotRejectReasonGuards();
    testRunNarrowphaseIntoBufferPreflightGuards();
    testManifoldFinalizePreflightWrapperGuards();
    testFrictionTangentsPreflightWrapperGuards();

// --- deepen additive from b4-narrowphase-deepen-guards-c120 ---
void testContactBufferPreflightWrappers() {
    expectTrue(frictionPreflight.canRebuild(), "friction preflight can rebuild with valid slot");
void testNarrowphaseDispatchDeepenPassGuards() {
void testManifoldFrictionDeepenPassSkipGuards() {
    testContactBufferPreflightWrappers();
