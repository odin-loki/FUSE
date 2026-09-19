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
    first.contactNormal = {0.f, 1.f, 0.f};
    first.penetrationDepth = 0.25f;
    first.addPoint({0.f, 0.f, 0.f}, 0.25f);
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
        fuse::physics::narrowphase::contact_pair_reject_reason({staticA, staticB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothStatic,
        "reject reason flags both-static pair");
        fuse::physics::narrowphase::is_static_contact_pair({staticA, staticB}, bodies),
        "static guard detects both-static pair");
        !fuse::physics::narrowphase::is_static_contact_pair({dynamicA, staticA}, bodies),
        "static guard allows dynamic/static mix");

    const fuse::u32 zeroRadiusBody = bodies.addBody({5.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, zeroRadiusBody, {0.f, 0.f, 0.f});
        fuse::physics::narrowphase::contact_pair_reject_reason({dynamicA, zeroRadiusBody}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::DegenerateShape,
        "reject reason flags zero-radius sphere");
        fuse::physics::narrowphase::is_degenerate_shape_pair({dynamicA, zeroRadiusBody}, shapes),
        "degenerate guard flags zero-radius shape");

    const auto staticPair =
        fuse::physics::narrowphase::detect_contacts_pair({staticA, staticB}, bodies, shapes);
    expectTrue(!staticPair.valid, "both-static pair returns invalid manifold");

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
        !emptyAfterPrune.pruneIfEmpty(),
        "pruneIfEmpty returns false when all points separated");
    expectTrue(emptyAfterPrune.empty(), "pruneIfEmpty clears separated manifold");

    fuse::physics::narrowphase::ContactManifold survives{};
    survives.contactNormal = {0.f, 1.f, 0.f};
    survives.addPoint({0.f, 0.f, 0.f}, 0.3f);
    expectTrue(survives.pruneIfEmpty(), "pruneIfEmpty returns true when points remain");
    expectTrue(survives.pointCount == 1u, "pruneIfEmpty keeps penetrating point");

void testFrictionTangentDeepenEarlyOuts() {
    fuse::physics::narrowphase::ContactManifold withBasis{};
    withBasis.contactNormal = {0.f, 1.f, 0.f};
    withBasis.addPoint({0.f, 0.f, 0.f}, 0.2f);
    withBasis.buildFrictionBasis();
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
        fuse::physics::narrowphase::tangentialSpeed(slip),
        0.5f,
        "tangentialSpeed computes magnitude");

        fuse::physics::narrowphase::should_skip_tangential_velocity_solve(
            {1e-9f, 1e-9f}, 0.5f, 0.3f, 1.f),
        "combined early-out skips negligible slip with friction");
            slip, 0.f, 0.f, 1.f),
        "combined early-out skips zero friction coefficients");
        !fuse::physics::narrowphase::should_skip_tangential_velocity_solve(
            slip, 0.5f, 0.3f, 0.5f),
        "combined early-out proceeds with slip and friction");

void testContactPairRejectGuardHelpers() {
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyNoShape = bodies.addBody({2.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

        fuse::physics::narrowphase::is_self_contact_pair({bodyA, bodyA}),
        "self guard flags identical body indices");
        !fuse::physics::narrowphase::is_self_contact_pair({bodyA, bodyB}),
        "self guard allows distinct bodies");
        fuse::physics::narrowphase::is_out_of_range_contact_pair({bodyA, 99u}, bodies),
        "out-of-range guard flags invalid body index");
        !fuse::physics::narrowphase::is_out_of_range_contact_pair({bodyA, bodyB}, bodies),
        "out-of-range guard allows in-range pair");
        fuse::physics::narrowphase::is_missing_shape_contact_pair({bodyA, bodyNoShape}, shapes),
        "missing-shape guard flags body without shape");
        !fuse::physics::narrowphase::is_missing_shape_contact_pair({bodyA, bodyB}, shapes),
        "missing-shape guard allows fully shaped pair");
        fuse::physics::narrowphase::contact_pair_rejects_for_reason(
            {bodyA, bodyA},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::SelfPair),
        "rejects_for_reason matches self pair");
        !fuse::physics::narrowphase::contact_pair_rejects_for_reason(
            {bodyA, bodyB},
        "rejects_for_reason does not false-positive valid pair");

void testCanFinalizeContactManifoldGuard() {
    fuse::physics::narrowphase::ContactManifold empty{};
        !fuse::physics::narrowphase::can_finalize_contact_manifold(empty),
        "can_finalize rejects empty manifold");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
        !fuse::physics::narrowphase::can_finalize_contact_manifold(noNormal),
        "can_finalize rejects zero-length normal");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
        !fuse::physics::narrowphase::can_finalize_contact_manifold(separated),
        "can_finalize rejects all-separated points");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
        fuse::physics::narrowphase::can_finalize_contact_manifold(ready),
        "can_finalize accepts penetrating manifold with valid normal");

void testManifoldPrunePreflightGuards() {
    fuse::physics::narrowphase::ContactManifold mixed{};
    mixed.contactNormal = {0.f, 1.f, 0.f};
    mixed.addPoint({0.f, 0.f, 0.f}, 0.4f);
    mixed.addPoint({1.f, 0.f, 0.f}, -0.2f);
    mixed.addPoint({0.00001f, 0.f, 0.f}, 0.35f);

    expectTrue(mixed.hasSeparatedPoints(), "hasSeparatedPoints flags separated slot");
    expectTrue(mixed.hasDuplicatePoints(1e-3f), "hasDuplicatePoints flags near-identical slots");
    expectTrue(!mixed.wouldBeEmptyAfterPrune(), "wouldBeEmptyAfterPrune false when penetrating remain");

void testContactPairRejectReasonNames() {
            "SelfPair") == 0,
        "reject reason name maps SelfPair");
                fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedShapePair),
            "UnsupportedShapePair") == 0,
        "reject reason name maps UnsupportedShapePair");
                fuse::physics::narrowphase::ContactPairRejectReason::None),
            "None") == 0,
        "reject reason name maps None");

void testPenetratingPointCounts() {
    manifold.addPoint({0.f, 0.f, 0.f}, 0.3f);
    manifold.addPoint({1.f, 0.f, 0.f}, 0.f);
    manifold.addPoint({2.f, 0.f, 0.f}, 0.15f);
    manifold.addPoint({3.f, 0.f, 0.f}, -0.05f);

    expectTrue(manifold.penetratingPointCount() == 3u, "penetratingPointCount keeps touching points");
    expectTrue(manifold.separatedPointCount() == 1u, "separatedPointCount tracks separated slots");
    expectTrue(manifold.hasPenetratingPoints(), "hasPenetratingPoints flags positive penetration");

    fuse::physics::narrowphase::ContactManifold resting{};
    resting.contactNormal = {0.f, 1.f, 0.f};
    resting.addPoint({0.f, 0.f, 0.f}, 0.f);
    resting.addPoint({1.f, 0.f, 0.f}, -0.01f);
    expectTrue(!resting.hasPenetratingPoints(), "hasPenetratingPoints false for resting contacts only");
    expectTrue(resting.penetratingPointCount() == 1u, "penetratingPointCount counts zero-penetration touch");

void testManifoldPruneIfEmpty() {
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

    fuse::physics::narrowphase::ContactManifold overCap{};
    overCap.contactNormal = {0.f, 1.f, 0.f};
    overCap.addPoint({0.f, 0.f, 0.f}, 0.5f);
    overCap.addPoint({1.f, 0.f, 0.f}, 0.4f);
    overCap.addPoint({2.f, 0.f, 0.f}, 0.3f);
    overCap.addPoint({3.f, 0.f, 0.f}, 0.2f);
    overCap.pointCount = 5u;
    const auto overCapPreflight = fuse::physics::narrowphase::preflight_manifold_prune(overCap);
    expectTrue(overCapPreflight.exceedsMaxPoints, "preflight flags point count above manifold cap");
    expectTrue(overCapPreflight.needs_pruning(), "preflight needs pruning when over cap");
    expectTrue(overCapPreflight.can_prune_in_place(), "preflight can prune over-cap manifold in place");
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
        validPreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::None,
        "preflight reports None for valid pair");
        !fuse::physics::narrowphase::should_skip_contact_pair_dispatch({bodyA, bodyB}, bodies, shapes),
        "skip guard allows valid pair");

    const auto selfPreflight =
        fuse::physics::narrowphase::preflight_contact_pair({bodyA, bodyA}, bodies, shapes);
    expectTrue(!selfPreflight.can_dispatch(), "preflight rejects self pair");
    expectTrue(selfPreflight.rejected, "preflight marks self pair rejected");
        fuse::physics::narrowphase::should_skip_contact_pair_dispatch({bodyA, bodyA}, bodies, shapes),
        "skip guard rejects self pair");

    const fuse::u32 staticA = bodies.addBody({0.f, 2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 staticB = bodies.addBody({0.f, 3.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, staticA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, staticB, {1.f, 0.f, 0.f});
    const auto staticPreflight =
        fuse::physics::narrowphase::preflight_contact_pair({staticA, staticB}, bodies, shapes);
    expectTrue(!staticPreflight.can_dispatch(), "preflight rejects both-static pair");
    expectTrue(
        staticPreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::BothStatic,
        "preflight reports BothStatic");

    const fuse::u32 triggerA = bodies.addBody({2.f, 0.f, 0.f}, 1.f, fuse::physics::RB_TRIGGER);
    const fuse::u32 triggerB = bodies.addBody({3.f, 0.f, 0.f}, 1.f, fuse::physics::RB_TRIGGER);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, triggerA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, triggerB, {1.f, 0.f, 0.f});
    const auto triggerPreflight =
        fuse::physics::narrowphase::preflight_contact_pair({triggerA, triggerB}, bodies, shapes);
    expectTrue(!triggerPreflight.can_dispatch(), "preflight rejects both-trigger pair");
        triggerPreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::BothTriggers,
        "preflight reports BothTriggers");

    const fuse::u32 zeroRadiusBody = bodies.addBody({5.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, zeroRadiusBody, {0.f, 0.f, 0.f});
    const auto degeneratePreflight =
        fuse::physics::narrowphase::preflight_contact_pair({bodyA, zeroRadiusBody}, bodies, shapes);
    expectTrue(!degeneratePreflight.can_dispatch(), "preflight rejects degenerate shape pair");
        degeneratePreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::DegenerateShape,
        "preflight reports DegenerateShape");

    const fuse::u32 planeA = bodies.addBody({0.f, -1.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 planeB = bodies.addBody({0.f, -2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeA, {0.f, 1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeB, {0.f, 1.f, 0.f});
    const auto unsupportedPreflight =
        fuse::physics::narrowphase::preflight_contact_pair({planeA, planeB}, bodies, shapes);
    expectTrue(!unsupportedPreflight.can_dispatch(), "preflight rejects unsupported plane-plane pair");
        unsupportedPreflight.reason ==
            fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedShapePair,
        "preflight reports UnsupportedShapePair");
}

void testContactManifoldFinalizePreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_contact_manifold_finalize(empty);
    expectTrue(emptyPreflight.skipped, "finalize preflight skips empty manifold");
    expectTrue(emptyPreflight.empty, "finalize preflight flags empty manifold");
        fuse::physics::narrowphase::should_skip_contact_manifold_finalize(empty),
        "should_skip finalize true for empty manifold");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto noNormalPreflight = fuse::physics::narrowphase::preflight_contact_manifold_finalize(noNormal);
    expectTrue(noNormalPreflight.invalidNormal, "finalize preflight flags invalid normal");
        !noNormalPreflight.can_finalize(),
        "finalize preflight rejects zero-length normal");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    const auto separatedPreflight = fuse::physics::narrowphase::preflight_contact_manifold_finalize(separated);
    expectTrue(separatedPreflight.noPenetratingPoints, "finalize preflight flags all-separated points");
    expectTrue(separatedPreflight.pruneWouldEmpty, "finalize preflight flags prune-would-empty");
        fuse::physics::narrowphase::should_skip_contact_manifold_finalize(separated),
        "should_skip finalize true for separated manifold");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    const auto readyPreflight = fuse::physics::narrowphase::preflight_contact_manifold_finalize(ready);
    expectTrue(readyPreflight.can_finalize(), "finalize preflight accepts penetrating manifold");
        !fuse::physics::narrowphase::should_skip_contact_manifold_finalize(ready),
        "should_skip finalize false for ready manifold");
        fuse::physics::narrowphase::can_finalize_contact_manifold(ready),
        "can_finalize agrees with finalize preflight for ready manifold");

void testNarrowphaseDispatchPreflightWiring() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {
        {bodyA, bodyB},
        {staticA, staticB},
        {bodyA, bodyA},
    };
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(pairs, bodies, shapes, buffer);

    expectTrue(buffer.activeCount == 1u, "dispatch preflight wiring keeps only valid overlapping pair");
    const auto restored = buffer.manifoldAt(0u);
    expectTrue(restored.valid, "dispatch wiring produces valid finalized manifold");
    expectTrue(restored.hasFrictionBasis(), "dispatch wiring finalizes friction basis");

void testFrictionBasisGuardHelpers() {
    fuse::physics::narrowphase::ContactManifold empty{};
        !fuse::physics::narrowphase::needs_friction_basis_rebuild(empty),
        "needs_friction_basis_rebuild false for empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
        fuse::physics::narrowphase::needs_friction_basis_rebuild(needsBuild),
        "needs_friction_basis_rebuild true without cached basis");

        fuse::physics::narrowphase::ensure_friction_basis(needsBuild),
        "ensure_friction_basis builds orthonormal frame");
        fuse::physics::narrowphase::friction_basis_matches_normal(needsBuild),
        "friction_basis_matches_normal true after ensure");

    const auto cachedTangent1 = needsBuild.frictionBasis.tangent1;
        "ensure_friction_basis reuses cached basis");
    expectNear(
        needsBuild.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "ensure_friction_basis preserves cached tangent1");

    fuse::physics::narrowphase::invalidate_friction_basis(needsBuild);
        !fuse::physics::narrowphase::has_cached_friction_basis(needsBuild),
        "invalidate_friction_basis clears cached frame");
        "needs_friction_basis_rebuild true after invalidate");

    fuse::physics::narrowphase::ContactManifold skip{};
    skip.addPoint({0.f, 0.f, 0.f}, 0.1f);
        !fuse::physics::narrowphase::ensure_friction_basis(skip),
        "ensure_friction_basis returns false when tangents should be skipped");

void testContactPairRejectBreakdownGuards() {
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

    const auto validBreakdown =
        fuse::physics::narrowphase::contact_pair_reject_breakdown({bodyA, bodyB}, bodies, shapes);
    expectTrue(validBreakdown.can_dispatch(), "breakdown allows valid pair dispatch");
    expectTrue(!validBreakdown.rejected(), "breakdown reports no reject for valid pair");

    const auto selfBreakdown =
        fuse::physics::narrowphase::contact_pair_reject_breakdown({bodyA, bodyA}, bodies, shapes);
    expectTrue(selfBreakdown.selfPair, "breakdown flags self pair");
    expectTrue(
        selfBreakdown.reason == fuse::physics::narrowphase::ContactPairRejectReason::SelfPair,
        "breakdown reason is SelfPair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_rejects_with_breakdown(
            {bodyA, bodyA},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::SelfPair),
        "rejects_with_breakdown matches self pair");

    const auto triggerBreakdown =
        fuse::physics::narrowphase::contact_pair_reject_breakdown({triggerA, triggerB}, bodies, shapes);
    expectTrue(triggerBreakdown.bothTriggers, "breakdown flags both-trigger pair");
    expectTrue(
        triggerBreakdown.reason == fuse::physics::narrowphase::ContactPairRejectReason::BothTriggers,
        "breakdown reason is BothTriggers");
}

void testManifoldFinalizePreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(empty);
    expectTrue(emptyPreflight.skipped, "finalize preflight skips empty manifold");
    expectTrue(!emptyPreflight.can_finalize(), "finalize preflight rejects empty manifold");
    expectTrue(
        fuse::physics::narrowphase::should_skip_manifold_finalize(empty),
        "should_skip_finalize true for empty manifold");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto noNormalPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(noNormal);
    expectTrue(noNormalPreflight.invalidNormal, "finalize preflight flags invalid normal");
    expectTrue(!noNormalPreflight.can_finalize(), "finalize preflight rejects invalid normal");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.2f);
    const auto separatedPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(separated);
    expectTrue(separatedPreflight.wouldBeEmptyAfterPrune, "finalize preflight flags prune-to-empty");
    expectTrue(!separatedPreflight.can_finalize(), "finalize preflight rejects separated manifold");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    const auto readyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(readyPreflight.can_finalize(), "finalize preflight accepts penetrating manifold");
    expectTrue(
        !readyPreflight.needs_prune_before_finalize(),
        "finalize preflight skips prune for clean manifold");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    const auto dirtyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(dirty);
    expectTrue(dirtyPreflight.needs_prune_before_finalize(), "finalize preflight requests prune");
    expectTrue(dirtyPreflight.can_finalize(), "finalize preflight still allows finalize after prune");

    fuse::physics::narrowphase::ContactManifold finalizeTarget = ready;
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_if_needed(finalizeTarget),
        "generate_if_needed finalizes ready manifold");
    expectTrue(finalizeTarget.valid, "generate_if_needed marks manifold valid");
    expectTrue(finalizeTarget.hasFrictionBasis(), "generate_if_needed builds friction basis");

    fuse::physics::narrowphase::ContactManifold skipTarget = separated;
    skipTarget.valid = true;
    expectTrue(
        !fuse::physics::narrowphase::generate_contact_manifold_if_needed(skipTarget),
        "generate_if_needed skips separated manifold");
    expectTrue(!skipTarget.valid, "generate_if_needed clears validity on skip");
    expectTrue(skipTarget.empty(), "generate_if_needed clears points on skip");
}

void testFrictionBasisPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(empty);
    expectTrue(emptyPreflight.skipped, "friction preflight skips empty manifold");
    expectTrue(!emptyPreflight.needs_rebuild(), "friction preflight does not rebuild empty manifold");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild(empty),
        "should_skip_friction_rebuild true for empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto missingPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(missingPreflight.missingBasis, "friction preflight flags missing basis");
    expectTrue(missingPreflight.needs_rebuild(), "friction preflight needs rebuild without basis");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_friction_basis_rebuild(needsBuild),
        "should_skip_friction_rebuild false when basis missing");

    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_from_preflight(needsBuild),
        "rebuild_from_preflight builds missing basis");
    expectTrue(needsBuild.hasFrictionBasis(), "rebuild_from_preflight stores orthonormal basis");

    const auto cachedPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(cachedPreflight.can_reuse_cached(), "friction preflight reuses valid cached basis");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild(needsBuild),
        "should_skip_friction_rebuild true for cached basis");

    fuse::physics::narrowphase::ContactManifold stale = needsBuild;
    stale.contactNormal = {1.f, 0.f, 0.f};
    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(stale);
    expectTrue(stalePreflight.staleBasis, "friction preflight flags stale basis");
    expectTrue(stalePreflight.needs_rebuild(), "friction preflight needs rebuild for stale basis");
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_from_preflight(stale),
        "rebuild_from_preflight refreshes stale basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(stale),
        "rebuild_from_preflight matches current normal");
}

void testFrictionBasisRefreshGuards() {
    fuse::physics::narrowphase::ContactManifold stale{};
    stale.contactNormal = {0.f, 1.f, 0.f};
    stale.addPoint({0.f, 0.f, 0.f}, 0.2f);
    stale.buildFrictionBasis();
    stale.contactNormal = {1.f, 0.f, 0.f};

        fuse::physics::narrowphase::friction_basis_is_stale(stale),
        "stale guard flags basis built for prior normal");
        fuse::physics::narrowphase::needs_friction_basis_refresh(stale),
        "refresh guard true when cached basis is stale");
        !fuse::physics::narrowphase::can_skip_friction_basis_rebuild(stale),
        "skip rebuild false for stale basis");

        fuse::physics::narrowphase::rebuild_friction_basis_if_needed(stale),
        "rebuild refreshes stale basis");
        fuse::physics::narrowphase::friction_basis_matches_normal(stale),
        "rebuild produces basis matching current normal");
        fuse::physics::narrowphase::can_skip_friction_basis_rebuild(stale),
        "skip rebuild true after refresh");

    fuse::physics::narrowphase::ContactManifold fresh{};
    fresh.contactNormal = {1.f, 0.f, 0.f};
    fresh.addPoint({0.f, 0.f, 0.f}, 0.1f);
    fuse::physics::narrowphase::compute_friction_tangents_if_needed(fresh);
    expectTrue(fresh.hasFrictionBasis(), "compute_if_needed builds missing basis");

    const auto cachedTangent1 = fresh.frictionBasis.tangent1;
        fresh.frictionBasis.tangent1.x,
        "compute_if_needed preserves valid cached basis");

void testContactPairDeepenRejectGuards() {
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

        fuse::physics::narrowphase::is_sleeping_contact_pair({sleepingA, sleepingB}, bodies),
        "sleeping guard detects both-sleeping pair");
        !fuse::physics::narrowphase::is_sleeping_contact_pair({dynamicA, sleepingA}, bodies),
        "sleeping guard allows dynamic/sleeping mix");
        fuse::physics::narrowphase::is_kinematic_contact_pair({kinematicA, kinematicB}, bodies),
        "kinematic guard detects both-kinematic pair");
        !fuse::physics::narrowphase::is_kinematic_contact_pair({dynamicA, kinematicA}, bodies),
        "kinematic guard allows dynamic/kinematic mix");

        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({sleepingA, sleepingB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping,
        "deepen reject reason flags both-sleeping pair");
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({kinematicA, kinematicB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothKinematic,
        "deepen reject reason flags both-kinematic pair");
        fuse::physics::narrowphase::contact_pair_reject_reason({sleepingA, sleepingB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::None,
        "base reject reason unchanged for sleeping pair");

    const auto sleepingPreflight =
        fuse::physics::narrowphase::preflight_contact_pair_deepen({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingPreflight.can_dispatch(), "deepen preflight rejects both-sleeping pair");
        sleepingPreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping,
        "deepen preflight reports BothSleeping");
        fuse::physics::narrowphase::should_skip_contact_pair_deepen_dispatch({sleepingA, sleepingB}, bodies, shapes),
        "deepen skip guard rejects both-sleeping pair");
        !fuse::physics::narrowphase::should_skip_contact_pair_deepen_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "deepen skip guard allows valid pair");

    const fuse::u32 capsuleBody = bodies.addBody({6.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Capsule, capsuleBody, {0.5f, 0.f, 0.f});
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({dynamicA, capsuleBody}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::DegenerateShape,
        "deepen reject reason flags zero half-height capsule");
        fuse::physics::narrowphase::contact_pair_reject_reason({dynamicA, capsuleBody}, bodies, shapes) ==
        "base reject reason unchanged for zero half-height capsule");

        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
            "BothSleeping") == 0,
        "reject reason name resolves BothSleeping");
                fuse::physics::narrowphase::ContactPairRejectReason::BothKinematic),
            "BothKinematic") == 0,
        "reject reason name resolves BothKinematic");

void testCanSkipNarrowphaseGuards() {

        fuse::physics::narrowphase::can_skip_narrowphase({}, bodies, shapes),
        "can_skip_narrowphase on empty pair list");
        fuse::physics::narrowphase::can_skip_narrowphase({{sleepingA, sleepingB}}, bodies, shapes),
        "can_skip_narrowphase when all pairs are deepen-rejected");
        !fuse::physics::narrowphase::can_skip_narrowphase({{bodyA, bodyB}}, bodies, shapes),
        "can_skip_narrowphase false when dispatchable pair exists");
        !fuse::physics::narrowphase::can_skip_narrowphase(
            {{sleepingA, sleepingB}, {bodyA, bodyB}}, bodies, shapes),
        "can_skip_narrowphase false when mixed rejected and dispatchable pairs");

void testManifoldFinalizePreflightGuards() {
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(empty);
    expectTrue(emptyPreflight.skipped, "finalize preflight skips empty manifold");
    expectTrue(!emptyPreflight.can_finalize(), "finalize preflight cannot finalize empty manifold");
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

void testGenerateContactManifoldIfNeededGuard() {
    empty.valid = true;
        !fuse::physics::narrowphase::generate_contact_manifold_if_needed(empty),
        "generate_if_needed no-ops on empty manifold");
    expectTrue(empty.valid, "generate_if_needed leaves empty manifold validity unchanged");

    separated.valid = true;
        !fuse::physics::narrowphase::generate_contact_manifold_if_needed(separated),
        "generate_if_needed no-ops on non-finalizable manifold");
    expectTrue(separated.valid, "generate_if_needed leaves separated manifold validity unchanged");

    fuse::physics::narrowphase::ContactManifold manifold =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
        fuse::physics::narrowphase::generate_contact_manifold_if_needed(manifold),
        "generate_if_needed finalizes valid manifold");
    expectTrue(manifold.valid, "generate_if_needed sets validity on success");
    expectTrue(manifold.hasFrictionBasis(), "generate_if_needed builds friction basis");

void testManifoldShallowPruneSkipGuards() {

    expectTrue(!shallow.canSkipPruneShallowPenetrations(0.05f), "canSkipShallow false with shallow slot");
    expectTrue(shallow.pruneShallowPenetrationsIfNeeded(0.05f), "shallow prune if needed keeps deep point");
    expectTrue(shallow.pointCount == 1u, "shallow prune if needed removes shallow slot");

    clean.addPoint({0.f, 0.f, 0.f}, 0.5f);
    expectTrue(clean.canSkipPruneShallowPenetrations(0.05f), "canSkipShallow true without shallow slots");
    expectTrue(clean.pruneShallowPenetrationsIfNeeded(0.05f), "shallow prune if needed no-ops on clean manifold");
    expectTrue(clean.pointCount == 1u, "shallow prune if needed preserves clean slots");

    const auto shallowPreflight =
        fuse::physics::narrowphase::preflight_manifold_prune(shallow, 1e-6f, 1e-4f, 0.05f);
    expectTrue(!shallowPreflight.needs_shallow_pruning(0.05f), "shallow preflight false after shallow prune");

void testFrictionBasisPreflightGuards() {
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(empty);
    expectTrue(emptyPreflight.skipped, "friction preflight skips empty manifold");
    expectTrue(emptyPreflight.can_skip_rebuild(), "friction preflight can skip empty manifold");
        fuse::physics::narrowphase::should_skip_friction_basis_preflight(empty),
        "should_skip_friction_basis_preflight on empty manifold");

    const auto needsPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(!needsPreflight.skipped, "friction preflight does not skip valid manifold");
    expectTrue(needsPreflight.needsRebuild, "friction preflight needs rebuild without cached basis");
    expectTrue(!needsPreflight.canReuse, "friction preflight cannot reuse missing basis");
    expectTrue(!needsPreflight.can_skip_rebuild(), "friction preflight cannot skip missing basis");

    fuse::physics::narrowphase::ContactManifold withBasis = needsBuild;
    const auto reusePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(withBasis);
    expectTrue(reusePreflight.canReuse, "friction preflight can reuse valid basis");
    expectTrue(!reusePreflight.needsRebuild, "friction preflight does not need rebuild for valid basis");
    expectTrue(reusePreflight.can_skip_rebuild(), "friction preflight can skip valid basis");
        fuse::physics::narrowphase::should_skip_friction_basis_preflight(withBasis),
        "should_skip_friction_basis_preflight on valid basis");

    withBasis.contactNormal = {1.f, 0.f, 0.f};
    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(withBasis);
    expectTrue(stalePreflight.stale, "friction preflight flags stale basis");
    expectTrue(stalePreflight.needsRebuild, "friction preflight needs rebuild for stale basis");
    expectTrue(!stalePreflight.can_skip_rebuild(), "friction preflight cannot skip stale basis");

void testContactPairDeepenPassRejectGuards() {
    const fuse::u32 triggerA = bodies.addBody({2.f, 0.f, 0.f}, 0.f, fuse::physics::RB_TRIGGER);
    const fuse::u32 triggerB = bodies.addBody({2.5f, 0.f, 0.f}, 0.f, fuse::physics::RB_TRIGGER);
    const fuse::u32 staticA = bodies.addBody({0.f, 2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 kinematicA = bodies.addBody({0.f, 3.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, triggerA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, triggerB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, staticA, {1.f, 0.f, 0.f});

        fuse::physics::narrowphase::is_any_trigger_contact_pair({dynamicA, triggerA}, bodies),
        "any-trigger guard flags mixed trigger/dynamic pair");
        !fuse::physics::narrowphase::is_any_trigger_contact_pair({dynamicA, dynamicB}, bodies),
        "any-trigger guard allows non-trigger pair");
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({dynamicA, triggerA}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::AnyTrigger,
        "deepen reject reason flags any-trigger pair");
        fuse::physics::narrowphase::contact_pair_reject_reason({dynamicA, triggerA}, bodies, shapes) ==
        "base reject reason unchanged for mixed trigger pair");

        fuse::physics::narrowphase::is_massless_contact_pair({staticA, kinematicA}, bodies),
        "massless guard flags static/kinematic pair");
        !fuse::physics::narrowphase::is_massless_contact_pair({dynamicA, staticA}, bodies),
        "massless guard allows dynamic/static mix");
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({staticA, kinematicA}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothMassless,
        "deepen reject reason flags both-massless pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {dynamicA, triggerA},
        {dynamicA, dynamicB},
    };
        fuse::physics::narrowphase::count_dispatchable_contact_pairs(mixedPairs, bodies, shapes) == 1u,
        "count_dispatchable excludes deepen-rejected pairs");
        fuse::physics::narrowphase::has_dispatchable_contact_pair(mixedPairs, bodies, shapes),
        "has_dispatchable true when one pair passes deepen preflight");
        !fuse::physics::narrowphase::has_dispatchable_contact_pair({{dynamicA, triggerA}}, bodies, shapes),
        "has_dispatchable false when all pairs deepen-rejected");

                fuse::physics::narrowphase::ContactPairRejectReason::AnyTrigger),
            "AnyTrigger") == 0,
        "reject reason name resolves AnyTrigger");
                fuse::physics::narrowphase::ContactPairRejectReason::BothMassless),
            "BothMassless") == 0,
        "reject reason name resolves BothMassless");

void testManifoldPruneDeepenPassGuards() {
    expectTrue(!clean.hasNonUnitNormal(), "hasNonUnitNormal false for unit normal");
    expectTrue(!clean.needsNormalNormalization(), "needsNormalNormalization false for unit normal");
        fuse::physics::narrowphase::should_skip_manifold_prune(clean),
        "should_skip_manifold_prune true for clean manifold");

    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.3f);
    expectTrue(unnormalized.hasNonUnitNormal(), "hasNonUnitNormal true for scaled normal");
    expectTrue(unnormalized.needsNormalNormalization(), "needsNormalNormalization true for scaled normal");

    const auto unnormalizedPreflight =
        fuse::physics::narrowphase::preflight_manifold_prune(unnormalized);
        unnormalizedPreflight.needsNormalNormalize,
        "prune preflight flags non-unit normal");

        !fuse::physics::narrowphase::should_skip_manifold_prune(shallow, 1e-6f, 1e-4f, 0.05f),
        "should_skip_manifold_prune false when shallow prune needed");
    shallow.pruneShallowPenetrationsIfNeeded(0.05f);
        fuse::physics::narrowphase::should_skip_manifold_prune(shallow, 1e-6f, 1e-4f, 0.05f),
        "should_skip_manifold_prune true after shallow prune");

    ready.contactNormal = {0.f, 2.f, 0.f};
    const auto finalizePreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(finalizePreflight.needsNormalNormalize, "finalize preflight flags non-unit normal");
    expectTrue(finalizePreflight.can_finalize(), "finalize preflight can finalize with penetrating points");

void testFrictionBasisDeepenPassPreflights() {
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.2f);
        fuse::physics::narrowphase::contact_normal_needs_normalize(unnormalized),
        "contact_normal_needs_normalize true for scaled normal");
        fuse::physics::narrowphase::should_normalize_contact_normal_before_friction(unnormalized),
        "should_normalize before friction for scaled normal");

    const auto needsNormalizePreflight =
        fuse::physics::narrowphase::preflight_friction_basis_rebuild(unnormalized);
        needsNormalizePreflight.needsNormalNormalize,
        "friction preflight flags non-unit normal");
        needsNormalizePreflight.needsRebuild,
        "friction preflight needs rebuild without cached basis");
        !needsNormalizePreflight.can_skip_rebuild(),
        "friction preflight cannot skip missing basis");

    fuse::physics::narrowphase::ContactManifold unit{};
    unit.contactNormal = {0.f, 1.f, 0.f};
    unit.addPoint({0.f, 0.f, 0.f}, 0.2f);
    unit.buildFrictionBasis();
        !fuse::physics::narrowphase::should_normalize_contact_normal_before_friction(unit),
        "should_normalize false for unit normal with valid basis");
    const auto reusePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(unit);
    expectTrue(!reusePreflight.needsNormalNormalize, "friction preflight skips normalize for unit normal");
    expectTrue(!allSeparated.pruneIfEmpty(), "pruneIfEmpty returns false when all points separate");
    expectTrue(allSeparated.empty(), "pruneIfEmpty clears separated-only manifold");

    fuse::physics::narrowphase::ContactManifold mixed{};
    mixed.contactNormal = {0.f, 1.f, 0.f};
    mixed.addPoint({0.f, 0.f, 0.f}, 0.4f);
    mixed.addPoint({1.f, 0.f, 0.f}, -0.2f);
    mixed.addPoint({2.f, 0.f, 0.f}, 0.00001f);
    mixed.addPoint({2.00001f, 0.f, 0.f}, 0.5f);
    expectTrue(mixed.pruneIfEmpty(), "pruneIfEmpty returns true when penetrating points remain");
    expectTrue(mixed.pointCount == 2u, "pruneIfEmpty chains separation and duplicate pruning");
    expectNear(mixed.maxPenetration(), 0.5f, 1e-4f, "pruneIfEmpty keeps deepest penetrating point");

void testCachedFrictionBasisHelpers() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
        !fuse::physics::narrowphase::hasCachedFrictionBasis(manifold),
        "hasCachedFrictionBasis false before build");
        fuse::physics::narrowphase::should_rebuild_friction_tangents(manifold),
        "should_rebuild true when basis is missing");

    fuse::physics::narrowphase::ensureFrictionBasis(manifold);
        fuse::physics::narrowphase::hasCachedFrictionBasis(manifold),
        "ensureFrictionBasis builds orthonormal cache");
        !fuse::physics::narrowphase::should_rebuild_friction_tangents(manifold),
        "should_rebuild false after valid cache");

    const auto cachedBasis = manifold.frictionBasis;
        manifold.frictionBasis.tangent1.x,
        cachedBasis.tangent1.x,
        "ensureFrictionBasis early-outs with cached basis");

    manifold.invalidateFrictionBasis();
        "invalidateFrictionBasis clears cached frame");
        "should_rebuild true after invalidate");

    manifold.contactNormal = {1.f, 0.f, 0.f};
    manifold.buildFrictionBasis();
        "rebuilt basis matches new normal");
        fuse::physics::narrowphase::buildTangentBasisForManifold(manifold).tangent1.x ==
        "buildTangentBasisForManifold reuses cached basis");

void testIsValidContactManifoldGuards() {
        fuse::physics::narrowphase::is_empty_contact_manifold(empty),
        "is_empty_contact_manifold flags zero points");
        !fuse::physics::narrowphase::is_valid_contact_manifold(empty),
        "is_valid_contact_manifold rejects empty manifold");

    fuse::physics::narrowphase::ContactManifold invalid{};
    invalid.valid = true;
    invalid.contactNormal = {};
    invalid.addPoint({0.f, 0.f, 0.f}, 0.2f);
        !fuse::physics::narrowphase::is_valid_contact_manifold(invalid),
        "is_valid_contact_manifold rejects zero-length normal");

    fuse::physics::narrowphase::ContactManifold valid =
    expectTrue(fuse::physics::narrowphase::generate_contact_manifold(valid), "finalize produces valid manifold");
        fuse::physics::narrowphase::is_valid_contact_manifold(valid),
        "is_valid_contact_manifold accepts finalized contact");
        !fuse::physics::narrowphase::is_empty_contact_manifold(valid),
        "is_empty_contact_manifold false for finalized contact");
}

void testContactPairSleepingKinematicGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 kinematicA = bodies.addBody({4.f, 0.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    const fuse::u32 kinematicB = bodies.addBody({5.f, 0.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, kinematicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, kinematicB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({sleepingA, sleepingB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping,
        "reject reason flags both-sleeping pair");
    expectTrue(
        fuse::physics::narrowphase::is_sleeping_contact_pair({sleepingA, sleepingB}, bodies),
        "sleeping guard detects both-sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::is_sleeping_contact_pair({dynamicA, sleepingA}, bodies),
        "sleeping guard allows dynamic/sleeping mix");

    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({kinematicA, kinematicB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothKinematic,
        "reject reason flags both-kinematic pair");
    expectTrue(
        fuse::physics::narrowphase::is_kinematic_contact_pair({kinematicA, kinematicB}, bodies),
        "kinematic guard detects both-kinematic pair");
    expectTrue(
        !fuse::physics::narrowphase::is_kinematic_contact_pair({dynamicA, kinematicA}, bodies),
        "kinematic guard allows dynamic/kinematic mix");

    const auto sleepingPair =
        fuse::physics::narrowphase::detect_contacts_pair({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingPair.valid, "both-sleeping pair returns invalid manifold");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
            "BothSleeping") == 0,
        "reject reason name resolves BothSleeping");
}

void testManifoldFinalizeGuards() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.contactNormal = {0.f, 2.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.3f);
    manifold.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(manifold.countSeparatedPoints() == 1u, "countSeparatedPoints tracks separated slots");
    expectTrue(!manifold.hasUnitNormal(), "hasUnitNormal false before normalization");
    expectTrue(manifold.canFinalize(), "canFinalize true with penetrating point");

    expectTrue(manifold.normalizeContactNormal(), "normalizeContactNormal succeeds");
    expectTrue(manifold.hasUnitNormal(), "hasUnitNormal true after normalization");
    expectNear(manifold.contactNormal.y, 1.f, 1e-4f, "normalizeContactNormal yields unit Y");

    fuse::physics::narrowphase::ContactManifold separatedOnly{};
    separatedOnly.contactNormal = {0.f, 1.f, 0.f};
    separatedOnly.addPoint({0.f, 0.f, 0.f}, -0.5f);
    expectTrue(!separatedOnly.canFinalize(), "canFinalize false when all points separated");
    expectTrue(
        !separatedOnly.pruneForFinalization(),
        "pruneForFinalization false when only separated points remain");
    expectTrue(separatedOnly.empty(), "pruneForFinalization clears separated manifold");

    fuse::physics::narrowphase::ContactManifold survives{};
    survives.contactNormal = {0.f, 1.f, 0.f};
    survives.addPoint({0.f, 0.f, 0.f}, 0.4f);
    survives.addPoint({0.f, 0.f, 0.f}, 0.5f);
    survives.addPoint({1.f, 0.f, 0.f}, -0.1f);
    expectTrue(survives.pruneForFinalization(), "pruneForFinalization true when penetrating points remain");
    expectTrue(survives.pointCount == 1u, "pruneForFinalization merges duplicates and drops separated");
    expectNear(survives.maxPenetration(), 0.5f, 1e-4f, "pruneForFinalization keeps deepest merged point");
}

void testFrictionBasisRebuildGuards() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::needs_friction_basis_rebuild(manifold),
        "needs rebuild before basis is built");
    expectTrue(
        fuse::physics::narrowphase::should_rebuild_friction_basis(manifold),
        "should_rebuild mirrors needs_friction_basis_rebuild");

    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(manifold),
        "ensure_friction_basis builds orthonormal frame");
    expectTrue(manifold.hasFrictionBasis(), "ensure leaves valid friction basis");
    expectTrue(
        fuse::physics::narrowphase::isValidFrictionBasisForNormal(
            manifold.contactNormal, manifold.frictionBasis),
        "isValidFrictionBasisForNormal accepts built basis");

    const auto cachedTangent1 = manifold.frictionBasis.tangent1;
    expectTrue(
        !fuse::physics::narrowphase::needs_friction_basis_rebuild(manifold),
        "needs rebuild false after ensure");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(manifold),
        "ensure early-outs with cached basis");
    expectNear(
        manifold.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "ensure preserves cached tangent1");

    manifold.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::needs_friction_basis_rebuild(manifold),
        "needs rebuild after normal changes");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(manifold),
        "ensure rebuilds after normal change");
    expectTrue(
        fuse::physics::narrowphase::isValidFrictionBasisForNormal(
            manifold.contactNormal, manifold.frictionBasis),
        "rebuilt basis matches new normal");

    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::ensure_friction_basis(empty),
        "ensure fails on empty manifold");
}

void testContactPairSleepingKinematicGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingA = bodies.addBody({2.f, 0.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({2.5f, 0.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 kinematicA = bodies.addBody({4.f, 0.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    const fuse::u32 kinematicB = bodies.addBody({4.5f, 0.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, kinematicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, kinematicB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({sleepingA, sleepingB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping,
        "reject reason flags both-sleeping pair");
    expectTrue(
        fuse::physics::narrowphase::is_sleeping_contact_pair({sleepingA, sleepingB}, bodies),
        "sleeping guard detects both-sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::is_sleeping_contact_pair({dynamicA, sleepingA}, bodies),
        "sleeping guard allows dynamic/sleeping mix");

    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({kinematicA, kinematicB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothKinematic,
        "reject reason flags both-kinematic pair");
    expectTrue(
        fuse::physics::narrowphase::is_kinematic_contact_pair({kinematicA, kinematicB}, bodies),
        "kinematic guard detects both-kinematic pair");
    expectTrue(
        !fuse::physics::narrowphase::is_kinematic_contact_pair({dynamicA, kinematicA}, bodies),
        "kinematic guard allows dynamic/kinematic mix");

    const auto sleepingPair =
        fuse::physics::narrowphase::detect_contacts_pair({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingPair.valid, "both-sleeping pair returns invalid manifold");

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

void testManifoldFinalizeGuards() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.contactNormal = {0.f, 2.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.3f);
    manifold.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(manifold.countSeparatedPoints() == 1u, "countSeparatedPoints tracks separated slots");
    expectTrue(!manifold.hasUnitNormal(), "hasUnitNormal false before normalization");
    expectTrue(manifold.canFinalize(), "canFinalize true with penetrating point");

    expectTrue(manifold.normalizeContactNormal(), "normalizeContactNormal succeeds");
    expectTrue(manifold.hasUnitNormal(), "hasUnitNormal true after normalization");
    expectNear(manifold.contactNormal.y, 1.f, 1e-4f, "normalizeContactNormal yields unit Y");

    fuse::physics::narrowphase::ContactManifold separatedOnly{};
    separatedOnly.contactNormal = {0.f, 1.f, 0.f};
    separatedOnly.addPoint({0.f, 0.f, 0.f}, -0.5f);
    expectTrue(!separatedOnly.canFinalize(), "canFinalize false when all points separated");
    expectTrue(
        !separatedOnly.pruneForFinalization(),
        "pruneForFinalization false when only separated points remain");
    expectTrue(separatedOnly.empty(), "pruneForFinalization clears separated manifold");

    fuse::physics::narrowphase::ContactManifold survives{};
    survives.contactNormal = {0.f, 1.f, 0.f};
    survives.addPoint({0.f, 0.f, 0.f}, 0.4f);
    survives.addPoint({0.f, 0.f, 0.f}, 0.5f);
    survives.addPoint({1.f, 0.f, 0.f}, -0.1f);
    expectTrue(survives.pruneForFinalization(), "pruneForFinalization true when penetrating points remain");
    expectTrue(survives.pointCount == 1u, "pruneForFinalization merges duplicates and drops separated");
    expectNear(survives.maxPenetration(), 0.5f, 1e-4f, "pruneForFinalization keeps deepest merged point");
}

void testFrictionBasisRebuildGuardOverloads() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::needs_friction_basis_rebuild(manifold),
        "needs rebuild before basis is built");
    expectTrue(
        fuse::physics::narrowphase::should_rebuild_friction_basis(manifold),
        "should_rebuild mirrors needs_friction_basis_rebuild");

    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(manifold),
        "ensure_friction_basis builds orthonormal frame");
    expectTrue(
        fuse::physics::narrowphase::isValidFrictionBasisForNormal(
            manifold.contactNormal, manifold.frictionBasis),
        "isValidFrictionBasisForNormal accepts built basis");

    const auto cachedTangent1 = manifold.frictionBasis.tangent1;
    expectTrue(
        !fuse::physics::narrowphase::needs_friction_basis_rebuild(manifold),
        "needs rebuild false after ensure");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(manifold),
        "ensure early-outs with cached basis");
    expectNear(
        manifold.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "ensure preserves cached tangent1");

    manifold.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::needs_friction_basis_rebuild(manifold),
        "needs rebuild after normal changes");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(manifold),
        "ensure rebuilds after normal change");
    expectTrue(
        fuse::physics::narrowphase::isValidFrictionBasisForNormal(
            manifold.contactNormal, manifold.frictionBasis),
        "rebuilt basis matches new normal");

    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::ensure_friction_basis(empty),
        "ensure fails on empty manifold");
}

void testContactPairSleepingKinematicGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 kinematicA = bodies.addBody({4.f, 0.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    const fuse::u32 kinematicB = bodies.addBody({5.f, 0.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, kinematicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, kinematicB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({sleepingA, sleepingB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping,
        "reject reason flags both-sleeping pair");
    expectTrue(
        fuse::physics::narrowphase::is_sleeping_contact_pair({sleepingA, sleepingB}, bodies),
        "sleeping guard detects both-sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::is_sleeping_contact_pair({dynamicA, sleepingA}, bodies),
        "sleeping guard allows dynamic/sleeping mix");

    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({kinematicA, kinematicB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothKinematic,
        "reject reason flags both-kinematic pair");
    expectTrue(
        fuse::physics::narrowphase::is_kinematic_contact_pair({kinematicA, kinematicB}, bodies),
        "kinematic guard detects both-kinematic pair");
    expectTrue(
        !fuse::physics::narrowphase::is_kinematic_contact_pair({dynamicA, kinematicA}, bodies),
        "kinematic guard allows dynamic/kinematic mix");

    const auto sleepingPair =
        fuse::physics::narrowphase::detect_contacts_pair({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingPair.valid, "both-sleeping pair returns invalid manifold");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
            "BothSleeping") == 0,
        "reject reason name resolves BothSleeping");
}

void testManifoldFinalizeGuards() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.contactNormal = {0.f, 2.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.3f);
    manifold.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(manifold.countSeparatedPoints() == 1u, "countSeparatedPoints tracks separated slots");
    expectTrue(!manifold.hasUnitNormal(), "hasUnitNormal false before normalization");
    expectTrue(manifold.canFinalize(), "canFinalize true with penetrating point");

    expectTrue(manifold.normalizeContactNormal(), "normalizeContactNormal succeeds");
    expectTrue(manifold.hasUnitNormal(), "hasUnitNormal true after normalization");
    expectNear(manifold.contactNormal.y, 1.f, 1e-4f, "normalizeContactNormal yields unit Y");

    fuse::physics::narrowphase::ContactManifold separatedOnly{};
    separatedOnly.contactNormal = {0.f, 1.f, 0.f};
    separatedOnly.addPoint({0.f, 0.f, 0.f}, -0.5f);
    expectTrue(!separatedOnly.canFinalize(), "canFinalize false when all points separated");
    expectTrue(
        !separatedOnly.pruneForFinalization(),
        "pruneForFinalization false when only separated points remain");
    expectTrue(separatedOnly.empty(), "pruneForFinalization clears separated manifold");

    fuse::physics::narrowphase::ContactManifold survives{};
    survives.contactNormal = {0.f, 1.f, 0.f};
    survives.addPoint({0.f, 0.f, 0.f}, 0.4f);
    survives.addPoint({0.f, 0.f, 0.f}, 0.5f);
    survives.addPoint({1.f, 0.f, 0.f}, -0.1f);
    expectTrue(survives.pruneForFinalization(), "pruneForFinalization true when penetrating points remain");
    expectTrue(survives.pointCount == 1u, "pruneForFinalization merges duplicates and drops separated");
    expectNear(survives.maxPenetration(), 0.5f, 1e-4f, "pruneForFinalization keeps deepest merged point");
}

void testFrictionBasisRebuildGuards() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::needs_friction_basis_rebuild(manifold),
        "needs rebuild before basis is built");
    expectTrue(
        fuse::physics::narrowphase::should_rebuild_friction_basis(manifold),
        "should_rebuild mirrors needs_friction_basis_rebuild");

    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(manifold),
        "ensure_friction_basis builds orthonormal frame");
    expectTrue(manifold.hasFrictionBasis(), "ensure leaves valid friction basis");
    expectTrue(
        fuse::physics::narrowphase::isValidFrictionBasisForNormal(
            manifold.contactNormal, manifold.frictionBasis),
        "isValidFrictionBasisForNormal accepts built basis");

    const auto cachedTangent1 = manifold.frictionBasis.tangent1;
    expectTrue(
        !fuse::physics::narrowphase::needs_friction_basis_rebuild(manifold),
        "needs rebuild false after ensure");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(manifold),
        "ensure early-outs with cached basis");
    expectNear(
        manifold.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "ensure preserves cached tangent1");

    manifold.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::needs_friction_basis_rebuild(manifold),
        "needs rebuild after normal changes");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(manifold),
        "ensure rebuilds after normal change");
    expectTrue(
        fuse::physics::narrowphase::isValidFrictionBasisForNormal(
            manifold.contactNormal, manifold.frictionBasis),
        "rebuilt basis matches new normal");

    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::ensure_friction_basis(empty),
        "ensure fails on empty manifold");
}

void testContactPairDispatchPathGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 planeA = bodies.addBody({0.f, 2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 planeB = bodies.addBody({0.f, 3.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 hullBody = bodies.addBody({5.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});
    shapes.addShape(
        fuse::physics::CollisionShapeType::Plane,
        planeA,
        {0.f, 1.f, 0.f},
        0.f);
    shapes.addShape(
        fuse::physics::CollisionShapeType::Plane,
        planeB,
        {0.f, 1.f, 0.f},
        0.f);
    shapes.addShape(fuse::physics::CollisionShapeType::ConvexHull, hullBody, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::should_skip_contact_pair({bodyA, bodyA}, bodies, shapes),
        "should_skip_contact_pair flags self pair");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_contact_pair({bodyA, bodyB}, bodies, shapes),
        "should_skip_contact_pair allows valid pair");
    expectTrue(
        fuse::physics::narrowphase::is_plane_plane_contact_pair({planeA, planeB}, shapes),
        "plane-plane guard flags unsupported plane pair");
    expectTrue(
        !fuse::physics::narrowphase::is_plane_plane_contact_pair({bodyA, planeA}, shapes),
        "plane-plane guard ignores mixed shape pair");
    expectTrue(
        fuse::physics::narrowphase::has_contact_pair_dispatch_path({bodyA, bodyB}, shapes),
        "dispatch path guard accepts sphere pair");
    expectTrue(
        !fuse::physics::narrowphase::has_contact_pair_dispatch_path({bodyA, hullBody}, shapes),
        "dispatch path guard rejects unsupported convex hull pair");
}

void testManifoldPrunePassTwoGuards() {
    fuse::physics::narrowphase::ContactManifold shallow{};
    shallow.contactNormal = {0.f, 1.f, 0.f};
    shallow.addPoint({0.f, 0.f, 0.f}, 0.5f);
    shallow.addPoint({1.f, 0.f, 0.f}, 0.02f);
    shallow.addPoint({2.f, 0.f, 0.f}, -0.1f);
    expectTrue(shallow.hasShallowPenetrations(0.05f), "hasShallowPenetrations flags shallow slot");
    expectTrue(shallow.countSeparatedPoints() == 1u, "countSeparatedPoints tracks separated slots");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.5f);
    clean.addPoint({1.f, 0.f, 0.f}, 0.3f);
    const fuse::u32 beforePrune = clean.pointCount;
    clean.pruneContactPointsIfNeeded();
    expectTrue(clean.pointCount == beforePrune, "pruneContactPointsIfNeeded skips clean manifold");

    fuse::physics::narrowphase::ContactManifold needsPrune = shallow;
    expectTrue(needsPrune.needsPruning(), "separated point triggers needsPruning");
    needsPrune.pruneContactPointsIfNeeded();
    expectTrue(needsPrune.pointCount == 2u, "pruneContactPointsIfNeeded prunes when needed");

    fuse::physics::narrowphase::ContactManifold staleValid{};
    staleValid.valid = true;
    staleValid.contactNormal = {0.f, 1.f, 0.f};
    staleValid.addPoint({0.f, 0.f, 0.f}, -0.2f);
    staleValid.addPoint({1.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        !staleValid.pruneAndInvalidateIfEmpty(),
        "pruneAndInvalidateIfEmpty returns false when all points separated");
    expectTrue(!staleValid.valid, "pruneAndInvalidateIfEmpty invalidates empty manifold");

    fuse::physics::narrowphase::ContactManifold survives{};
    survives.valid = true;
    survives.contactNormal = {0.f, 1.f, 0.f};
    survives.addPoint({0.f, 0.f, 0.f}, 0.3f);
    survives.addPoint({1.f, 0.f, 0.f}, -0.05f);
    expectTrue(survives.pruneAndInvalidateIfEmpty(), "pruneAndInvalidateIfEmpty keeps penetrating manifold");
    expectTrue(survives.valid, "pruneAndInvalidateIfEmpty preserves validity when points remain");
}

void testFrictionBasisRebuildPassTwoGuards() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    manifold.buildFrictionBasis();

    expectTrue(
        !fuse::physics::narrowphase::friction_basis_is_stale(manifold),
        "fresh basis is not stale");

    const auto originalTangent1 = manifold.frictionBasis.tangent1;
    manifold.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_is_stale(manifold),
        "normal change marks cached basis stale");

    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_if_needed(manifold),
        "rebuild_friction_basis_if_needed rebuilds stale basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(manifold),
        "rebuilt basis matches updated normal");
    expectTrue(
        manifold.frictionBasis.tangent1.x != originalTangent1.x ||
            manifold.frictionBasis.tangent1.y != originalTangent1.y ||
            manifold.frictionBasis.tangent1.z != originalTangent1.z,
        "rebuild updates tangent frame after normal change");

    const auto rebuiltTangent1 = manifold.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_if_needed(manifold),
        "rebuild_friction_basis_if_needed reuses valid basis");
    expectNear(
        manifold.frictionBasis.tangent1.x,
        rebuiltTangent1.x,
        1e-4f,
        "rebuild early-out preserves tangent1");

    fuse::physics::narrowphase::compute_friction_tangents(manifold);
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(manifold),
        "compute_friction_tangents keeps orthonormal basis after rebuild");

    fuse::physics::narrowphase::ContactManifold skip{};
    skip.addPoint({0.f, 0.f, 0.f}, 0.1f);
    expectTrue(
        !fuse::physics::narrowphase::rebuild_friction_basis_if_needed(skip),
        "rebuild_friction_basis_if_needed returns false when tangents should be skipped");
}

void testContactPairSleepingKinematicGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 kinematicA = bodies.addBody({4.f, 0.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    const fuse::u32 kinematicB = bodies.addBody({5.f, 0.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, kinematicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, kinematicB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({sleepingA, sleepingB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping,
        "reject reason flags both-sleeping pair");
    expectTrue(
        fuse::physics::narrowphase::is_sleeping_contact_pair({sleepingA, sleepingB}, bodies),
        "sleeping guard detects both-sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::is_sleeping_contact_pair({dynamicA, sleepingA}, bodies),
        "sleeping guard allows dynamic/sleeping mix");

    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({kinematicA, kinematicB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothKinematic,
        "reject reason flags both-kinematic pair");
    expectTrue(
        fuse::physics::narrowphase::is_kinematic_contact_pair({kinematicA, kinematicB}, bodies),
        "kinematic guard detects both-kinematic pair");
    expectTrue(
        !fuse::physics::narrowphase::is_kinematic_contact_pair({dynamicA, kinematicA}, bodies),
        "kinematic guard allows dynamic/kinematic mix");

    const auto sleepingPair =
        fuse::physics::narrowphase::detect_contacts_pair({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingPair.valid, "both-sleeping pair returns invalid manifold");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
            "BothSleeping") == 0,
        "reject reason name resolves BothSleeping");
}

void testManifoldFinalizeGuards() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.contactNormal = {0.f, 2.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.3f);
    manifold.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(manifold.countSeparatedPoints() == 1u, "countSeparatedPoints tracks separated slots");
    expectTrue(!manifold.hasUnitNormal(), "hasUnitNormal false before normalization");
    expectTrue(manifold.canFinalize(), "canFinalize true with penetrating point");

    expectTrue(manifold.normalizeContactNormal(), "normalizeContactNormal succeeds");
    expectTrue(manifold.hasUnitNormal(), "hasUnitNormal true after normalization");
    expectNear(manifold.contactNormal.y, 1.f, 1e-4f, "normalizeContactNormal yields unit Y");

    fuse::physics::narrowphase::ContactManifold separatedOnly{};
    separatedOnly.contactNormal = {0.f, 1.f, 0.f};
    separatedOnly.addPoint({0.f, 0.f, 0.f}, -0.5f);
    expectTrue(!separatedOnly.canFinalize(), "canFinalize false when all points separated");
    expectTrue(
        !separatedOnly.pruneForFinalization(),
        "pruneForFinalization false when only separated points remain");
    expectTrue(separatedOnly.empty(), "pruneForFinalization clears separated manifold");

    fuse::physics::narrowphase::ContactManifold survives{};
    survives.contactNormal = {0.f, 1.f, 0.f};
    survives.addPoint({0.f, 0.f, 0.f}, 0.4f);
    survives.addPoint({0.f, 0.f, 0.f}, 0.5f);
    survives.addPoint({1.f, 0.f, 0.f}, -0.1f);
    expectTrue(survives.pruneForFinalization(), "pruneForFinalization true when penetrating points remain");
    expectTrue(survives.pointCount == 1u, "pruneForFinalization merges duplicates and drops separated");
    expectNear(survives.maxPenetration(), 0.5f, 1e-4f, "pruneForFinalization keeps deepest merged point");
}

void testFrictionBasisRebuildGuards() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::needs_friction_basis_rebuild(manifold),
        "needs rebuild before basis is built");
    expectTrue(
        fuse::physics::narrowphase::should_rebuild_friction_basis(manifold),
        "should_rebuild mirrors needs_friction_basis_rebuild");

    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(manifold),
        "ensure_friction_basis builds orthonormal frame");
    expectTrue(manifold.hasFrictionBasis(), "ensure leaves valid friction basis");
    expectTrue(
        fuse::physics::narrowphase::isValidFrictionBasisForNormal(
            manifold.contactNormal, manifold.frictionBasis),
        "isValidFrictionBasisForNormal accepts built basis");

    const auto cachedTangent1 = manifold.frictionBasis.tangent1;
    expectTrue(
        !fuse::physics::narrowphase::needs_friction_basis_rebuild(manifold),
        "needs rebuild false after ensure");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(manifold),
        "ensure early-outs with cached basis");
    expectNear(
        manifold.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "ensure preserves cached tangent1");

    manifold.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::needs_friction_basis_rebuild(manifold),
        "needs rebuild after normal changes");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(manifold),
        "ensure rebuilds after normal change");
    expectTrue(
        fuse::physics::narrowphase::isValidFrictionBasisForNormal(
            manifold.contactNormal, manifold.frictionBasis),
        "rebuilt basis matches new normal");

    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::ensure_friction_basis(empty),
        "ensure fails on empty manifold");
}

void testContactPairSleepingKinematicGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 kinematicA = bodies.addBody({2.f, 0.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    const fuse::u32 kinematicB = bodies.addBody({2.5f, 0.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, kinematicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, kinematicB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({sleepingA, sleepingB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping,
        "reject reason flags both-sleeping pair");
    expectTrue(
        fuse::physics::narrowphase::is_sleeping_contact_pair({sleepingA, sleepingB}, bodies),
        "sleeping guard detects both-sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::is_sleeping_contact_pair({dynamicA, sleepingA}, bodies),
        "sleeping guard allows dynamic/sleeping mix");

    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({kinematicA, kinematicB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothKinematic,
        "reject reason flags both-kinematic pair");
    expectTrue(
        fuse::physics::narrowphase::is_kinematic_contact_pair({kinematicA, kinematicB}, bodies),
        "kinematic guard detects both-kinematic pair");
    expectTrue(
        !fuse::physics::narrowphase::is_kinematic_contact_pair({dynamicA, kinematicA}, bodies),
        "kinematic guard allows dynamic/kinematic mix");

    const auto sleepingPair =
        fuse::physics::narrowphase::detect_contacts_pair({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingPair.valid, "both-sleeping pair returns invalid manifold");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
            "BothSleeping") == 0,
        "reject reason name resolves BothSleeping");
}

void testManifoldFinalizeGuards() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.contactNormal = {0.f, 2.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.3f);
    manifold.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(manifold.countSeparatedPoints() == 1u, "countSeparatedPoints tracks separated slots");
    expectTrue(!manifold.hasUnitNormal(), "hasUnitNormal false before normalization");
    expectTrue(manifold.canFinalize(), "canFinalize true with penetrating point");

    expectTrue(manifold.normalizeContactNormal(), "normalizeContactNormal succeeds");
    expectTrue(manifold.hasUnitNormal(), "hasUnitNormal true after normalization");
    expectNear(manifold.contactNormal.y, 1.f, 1e-4f, "normalizeContactNormal yields unit Y");

    fuse::physics::narrowphase::ContactManifold separatedOnly{};
    separatedOnly.contactNormal = {0.f, 1.f, 0.f};
    separatedOnly.addPoint({0.f, 0.f, 0.f}, -0.5f);
    expectTrue(!separatedOnly.canFinalize(), "canFinalize false when all points separated");
    expectTrue(
        !separatedOnly.pruneForFinalization(),
        "pruneForFinalization false when only separated points remain");
    expectTrue(separatedOnly.empty(), "pruneForFinalization clears separated manifold");

    fuse::physics::narrowphase::ContactManifold survives{};
    survives.contactNormal = {0.f, 1.f, 0.f};
    survives.addPoint({0.f, 0.f, 0.f}, 0.4f);
    survives.addPoint({0.f, 0.f, 0.f}, 0.5f);
    survives.addPoint({1.f, 0.f, 0.f}, -0.1f);
    expectTrue(survives.pruneForFinalization(), "pruneForFinalization true when penetrating points remain");
    expectTrue(survives.pointCount == 1u, "pruneForFinalization merges duplicates and drops separated");
    expectNear(survives.maxPenetration(), 0.5f, 1e-4f, "pruneForFinalization keeps deepest merged point");
}

void testFrictionBasisRebuildGuards() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::needs_friction_basis_rebuild(manifold),
        "needs rebuild before basis is built");
    expectTrue(
        fuse::physics::narrowphase::should_rebuild_friction_basis(manifold),
        "should_rebuild mirrors needs_friction_basis_rebuild");

    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(manifold),
        "ensure_friction_basis builds orthonormal frame");
    expectTrue(manifold.hasFrictionBasis(), "ensure leaves valid friction basis");
    expectTrue(
        fuse::physics::narrowphase::isValidFrictionBasisForNormal(
            manifold.contactNormal, manifold.frictionBasis),
        "isValidFrictionBasisForNormal accepts built basis");

    const auto cachedTangent1 = manifold.frictionBasis.tangent1;
    expectTrue(
        !fuse::physics::narrowphase::needs_friction_basis_rebuild(manifold),
        "needs rebuild false after ensure");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(manifold),
        "ensure early-outs with cached basis");
    expectNear(
        manifold.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "ensure preserves cached tangent1");

    manifold.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::needs_friction_basis_rebuild(manifold),
        "needs rebuild after normal changes");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis(manifold),
        "ensure rebuilds after normal change");
    expectTrue(
        fuse::physics::narrowphase::isValidFrictionBasisForNormal(
            manifold.contactNormal, manifold.frictionBasis),
        "rebuilt basis matches new normal");

    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::ensure_friction_basis(empty),
        "ensure fails on empty manifold");
}

void testContactPairDispatchGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::can_dispatch_contact_pair({bodyA, bodyB}, bodies, shapes),
        "can_dispatch allows valid pair");
    expectTrue(
        !fuse::physics::narrowphase::can_dispatch_contact_pair({bodyA, bodyA}, bodies, shapes),
        "can_dispatch rejects self pair");

    const auto guarded =
        fuse::physics::narrowphase::detect_contacts_pair_if_valid({bodyA, bodyB}, bodies, shapes);
    expectTrue(guarded.valid, "detect_if_valid returns contact for valid pair");
    expectTrue(guarded.pointCount > 0u, "detect_if_valid populates points");

    const auto rejected =
        fuse::physics::narrowphase::detect_contacts_pair_if_valid({bodyA, bodyA}, bodies, shapes);
    expectTrue(!rejected.valid, "detect_if_valid rejects self pair");

    const auto dispatch =
        fuse::physics::narrowphase::dispatch_contact_pair_if_valid({bodyA, bodyB}, bodies, shapes);
    expectTrue(dispatch.detected, "dispatch_if_valid detects valid pair");
    expectTrue(dispatch.preflight.can_dispatch(), "dispatch_if_valid preflight allows valid pair");

    const auto selfDispatch =
        fuse::physics::narrowphase::dispatch_contact_pair_if_valid({bodyA, bodyA}, bodies, shapes);
    expectTrue(selfDispatch.rejected(), "dispatch_if_valid marks self pair rejected");
    expectTrue(!selfDispatch.detected, "dispatch_if_valid skips detect for rejected pair");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_preflight_reason_name(selfDispatch.preflight),
            "SelfPair") == 0,
        "preflight reason name resolves SelfPair");
}

void testManifoldPrunePreflightExGuards() {
    fuse::physics::narrowphase::ContactManifold shallow{};
    shallow.contactNormal = {0.f, 1.f, 0.f};
    shallow.addPoint({0.f, 0.f, 0.f}, 0.5f);
    shallow.addPoint({1.f, 0.f, 0.f}, 0.01f);

    expectTrue(
        shallow.canSkipPruneShallowPenetrations(0.05f) == false,
        "canSkipPruneShallow false when shallow slots exist");
    expectTrue(
        shallow.pruneShallowPenetrationsIfNeeded(0.05f),
        "pruneShallowIfNeeded keeps deep point");
    expectTrue(shallow.pointCount == 1u, "shallow conditional prune removes shallow slot");

    const auto preflight =
        fuse::physics::narrowphase::preflight_manifold_prune_ex(shallow, 1e-6f, 1e-4f, 0.05f);
    expectTrue(!preflight.hasShallow, "extended preflight marks shallow prune complete");
    expectTrue(preflight.can_skip(), "extended preflight can_skip after shallow prune");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    dirty.addPoint({0.00001f, 0.f, 0.f}, 0.35f);

    const auto dirtyPreflight = fuse::physics::narrowphase::preflight_manifold_prune(dirty);
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_prune(dirtyPreflight) == false,
        "can_skip_manifold_prune false when separated/duplicate slots exist");
    expectTrue(dirty.pruneFromPreflight(dirtyPreflight), "pruneFromPreflight keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "pruneFromPreflight removes separated and duplicate slots");
}

void testFrictionBasisPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_friction_basis(empty);
    expectTrue(emptyPreflight.skipped, "friction preflight skips empty manifold");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_compute(empty),
        "should_skip_friction_basis_compute true for empty manifold");

    fuse::physics::narrowphase::ContactManifold fresh{};
    fresh.contactNormal = {0.f, 1.f, 0.f};
    fresh.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto needsBuild = fuse::physics::narrowphase::preflight_friction_basis(fresh);
    expectTrue(needsBuild.needsRefresh, "friction preflight flags missing basis");
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_from_preflight(fresh, needsBuild),
        "rebuild_from_preflight builds orthonormal frame");
    expectTrue(fresh.hasFrictionBasis(), "rebuild_from_preflight stores basis");

    const auto cachedPreflight = fuse::physics::narrowphase::preflight_friction_basis(fresh);
    expectTrue(cachedPreflight.canSkipRebuild, "friction preflight can skip valid cached basis");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_compute(fresh),
        "should_skip_friction_basis_compute true with valid basis");

    fresh.contactNormal = {1.f, 0.f, 0.f};
    fuse::physics::narrowphase::invalidate_friction_basis_if_stale(fresh);
    expectTrue(
        !fuse::physics::narrowphase::has_cached_friction_basis(fresh),
        "invalidate_if_stale clears stale basis");

    fuse::physics::narrowphase::ContactManifold finalizeReady =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_if_valid(finalizeReady),
        "generate_if_valid finalizes penetrating manifold");
    expectTrue(finalizeReady.hasFrictionBasis(), "generate_if_valid builds friction basis");

    fuse::physics::narrowphase::ContactManifold notReady{};
    notReady.contactNormal = {0.f, 1.f, 0.f};
    notReady.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        !fuse::physics::narrowphase::generate_contact_manifold_if_valid(notReady),
        "generate_if_valid rejects separated manifold");
    expectTrue(notReady.empty(), "generate_if_valid clears rejected manifold");
}

void testContactPairDeepenPreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 planeA = bodies.addBody({0.f, 0.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 planeB = bodies.addBody({0.f, 2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeA, {0.f, 1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeB, {0.f, -1.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::can_dispatch_contact_pair({bodyA, bodyB}, bodies, shapes),
        "can_dispatch allows valid pair");
    expectTrue(
        !fuse::physics::narrowphase::can_dispatch_contact_pair({bodyA, bodyA}, bodies, shapes),
        "can_dispatch rejects self pair");
    expectTrue(
        fuse::physics::narrowphase::is_plane_plane_contact_pair({planeA, planeB}, shapes),
        "plane-plane guard flags unsupported combo");
    expectTrue(
        !fuse::physics::narrowphase::is_plane_plane_contact_pair({bodyA, bodyB}, shapes),
        "plane-plane guard allows sphere pair");

    const auto selfPreflight =
        fuse::physics::narrowphase::preflight_contact_pair({bodyA, bodyA}, bodies, shapes);
    expectTrue(
        fuse::physics::narrowphase::contact_pair_preflight_matches(
            selfPreflight,
            fuse::physics::narrowphase::ContactPairRejectReason::SelfPair),
        "preflight_matches identifies self pair reason");

    const auto planePreflight =
        fuse::physics::narrowphase::preflight_contact_pair({planeA, planeB}, bodies, shapes);
    expectTrue(
        fuse::physics::narrowphase::contact_pair_preflight_matches(
            planePreflight,
            fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedShapePair),
        "preflight_matches identifies plane-plane unsupported reason");
}

void testManifoldFinalizePreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_finalize_contact_manifold(empty);
    expectTrue(emptyPreflight.skipped, "finalize preflight skips empty manifold");
    expectTrue(!emptyPreflight.can_finalize(), "finalize preflight cannot finalize empty");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto noNormalPreflight = fuse::physics::narrowphase::preflight_finalize_contact_manifold(noNormal);
    expectTrue(noNormalPreflight.invalidNormal, "finalize preflight flags invalid normal");
    expectTrue(!noNormalPreflight.can_finalize(), "finalize preflight rejects invalid normal");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    const auto separatedPreflight = fuse::physics::narrowphase::preflight_finalize_contact_manifold(separated);
    expectTrue(separatedPreflight.noPenetratingPoints, "finalize preflight flags no penetrating points");
    expectTrue(separatedPreflight.wouldBeEmptyAfterPrune, "finalize preflight flags empty-after-prune");

    fuse::physics::narrowphase::ContactManifold ready =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    const auto readyPreflight = fuse::physics::narrowphase::preflight_finalize_contact_manifold(ready);
    expectTrue(readyPreflight.can_finalize(), "finalize preflight accepts penetrating manifold");

    expectTrue(
        !fuse::physics::narrowphase::can_skip_finalize_contact_manifold(ready),
        "can_skip_finalize false before generate");
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_if_needed(ready),
        "generate_if_needed finalizes unprepared manifold");
    expectTrue(ready.valid, "generate_if_needed sets validity");
    expectTrue(
        fuse::physics::narrowphase::can_skip_finalize_contact_manifold(ready),
        "can_skip_finalize true after generate");

    const auto cachedNormal = ready.contactNormal;
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_if_needed(ready),
        "generate_if_needed is no-op on finalized manifold");
    expectNear(cachedNormal.x, ready.contactNormal.x, 1e-4f, "generate_if_needed preserves finalized normal");
}

void testManifoldPruneDeepenPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    clean.addPoint({1.f, 0.f, 0.f}, 0.25f);

    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_prune(clean),
        "can_skip_manifold_prune true for clean manifold");
    const auto cleanPreflight = fuse::physics::narrowphase::preflight_manifold_prune(clean);
    expectTrue(cleanPreflight.can_skip_prune(), "prune preflight can_skip_prune on clean manifold");

    fuse::physics::narrowphase::ContactManifold shallow{};
    shallow.contactNormal = {0.f, 1.f, 0.f};
    shallow.addPoint({0.f, 0.f, 0.f}, 0.5f);
    shallow.addPoint({1.f, 0.f, 0.f}, 0.01f);
    const auto shallowPreflight = fuse::physics::narrowphase::preflight_manifold_prune(shallow, 1e-6f, 1e-4f, 0.05f);
    expectTrue(shallowPreflight.hasShallow, "prune preflight flags shallow penetrations");
    expectTrue(shallowPreflight.needs_pruning(), "prune preflight needs pruning with shallow flag");
}

void testFrictionBasisPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(empty);
    expectTrue(emptyPreflight.skipped, "friction preflight skips empty manifold");
    expectTrue(!emptyPreflight.needs_rebuild(), "friction preflight does not rebuild empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto missingPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(missingPreflight.missing, "friction preflight flags missing basis");
    expectTrue(missingPreflight.needs_rebuild(), "friction preflight needs rebuild when missing");

    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_if_needed(needsBuild),
        "ensure_if_needed builds missing basis");
    expectTrue(
        fuse::physics::narrowphase::can_skip_friction_tangents_rebuild(needsBuild),
        "can_skip_friction_tangents_rebuild true after build");

    const auto freshPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(freshPreflight.can_reuse(), "friction preflight can_reuse fresh basis");

    needsBuild.contactNormal = {1.f, 0.f, 0.f};
    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(stalePreflight.stale, "friction preflight flags stale basis after normal change");
    expectTrue(stalePreflight.needs_rebuild(), "friction preflight needs rebuild when stale");

    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_if_needed(needsBuild),
        "ensure_if_needed refreshes stale basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(needsBuild),
        "ensure_if_needed produces basis matching current normal");
}

void testContactPairB45DispatchGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::is_contact_pair_dispatchable({bodyA, bodyB}, bodies, shapes),
        "dispatchable guard allows valid pair");
    expectTrue(
        !fuse::physics::narrowphase::should_reject_contact_pair({bodyA, bodyB}, bodies, shapes),
        "reject guard does not false-positive valid pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason(bodyA, bodyB, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::None,
        "body-index reject reason matches valid pair");

    const auto selfPreflight =
        fuse::physics::narrowphase::preflight_contact_pair({bodyA, bodyA}, bodies, shapes);
    expectTrue(
        fuse::physics::narrowphase::contact_pair_preflight_matches_reason(
            selfPreflight, fuse::physics::narrowphase::ContactPairRejectReason::SelfPair),
        "preflight_matches_reason flags self pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {
        {bodyA, bodyB},
        {bodyA, bodyA},
        {bodyA, 99u},
    };
    expectTrue(
        fuse::physics::narrowphase::count_dispatchable_contact_pairs(pairs, bodies, shapes) == 1u,
        "count_dispatchable tallies valid pairs only");
    expectTrue(
        fuse::physics::narrowphase::count_rejected_contact_pairs(pairs, bodies, shapes) == 2u,
        "count_rejected tallies invalid pairs only");
}

void testManifoldFinalizeB45Guards() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    clean.addPoint({1.f, 0.f, 0.f}, 0.25f);

    expectTrue(
        fuse::physics::narrowphase::should_skip_manifold_prune(clean),
        "skip prune true for clean manifold");

    const auto finalizePreflight =
        fuse::physics::narrowphase::preflight_finalize_contact_manifold(clean);
    expectTrue(finalizePreflight.can_finalize(), "finalize preflight accepts penetrating manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_finalize_contact_manifold(clean),
        "skip finalize false for ready manifold");

    fuse::physics::narrowphase::ContactManifold ready = clean;
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_if_ready(ready),
        "generate_if_ready finalizes valid manifold");
    expectTrue(ready.valid, "generate_if_ready sets validity");
    expectTrue(ready.hasFrictionBasis(), "generate_if_ready builds friction basis");

    fuse::physics::narrowphase::ContactManifold shallow = clean;
    shallow.addPoint({2.f, 0.f, 0.f}, 0.01f);
    expectTrue(
        shallow.hasShallowPenetrations(0.05f),
        "shallow guard flags shallow slot before conditional prune");
    expectTrue(
        fuse::physics::narrowphase::prune_shallow_penetrations_if_needed(shallow, 0.05f),
        "conditional shallow prune keeps deep points");
    expectTrue(shallow.pointCount == 2u, "conditional shallow prune removes shallow slot");

    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::generate_contact_manifold_if_ready(empty),
        "generate_if_ready skips empty manifold");
    const auto emptyPreflight =
        fuse::physics::narrowphase::preflight_finalize_contact_manifold(empty);
    expectTrue(emptyPreflight.skipped, "finalize preflight skips empty manifold");
    expectTrue(emptyPreflight.wouldFail, "finalize preflight marks empty manifold as failure");
}

void testFrictionBasisB45PreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    const auto emptyPreflight =
        fuse::physics::narrowphase::preflight_friction_basis_rebuild(empty);
    expectTrue(emptyPreflight.skipped, "friction preflight skips empty manifold");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild(empty),
        "skip rebuild true for empty manifold");

    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::contact_normal_needs_normalization(unnormalized),
        "normalization guard flags non-unit normal");

    const auto needsPreflight =
        fuse::physics::narrowphase::preflight_friction_basis_rebuild(unnormalized);
    expectTrue(needsPreflight.needs_rebuild(), "preflight rebuild true without cached basis");
    expectTrue(needsPreflight.needsNormalization, "preflight flags non-unit normal");

    const auto rebuildResult =
        fuse::physics::narrowphase::rebuild_friction_basis_guarded(unnormalized);
    expectTrue(rebuildResult.rebuilt, "guarded rebuild builds basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(unnormalized),
        "guarded rebuild produces matching basis");
    expectNear(unnormalized.contactNormal.length(), 1.f, 1e-4f, "guarded rebuild normalizes contact normal");

    fuse::physics::narrowphase::ContactManifold cached = unnormalized;
    const auto cachedPreflight =
        fuse::physics::narrowphase::preflight_friction_basis_rebuild(cached);
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild(cached),
        "skip rebuild true with valid cached basis");
    expectTrue(!cachedPreflight.needs_rebuild(), "preflight rebuild false with valid cached basis");

    const auto cachedResult =
        fuse::physics::narrowphase::rebuild_friction_basis_guarded(cached);
    expectTrue(cachedResult.skipped, "guarded rebuild skips valid cached basis");
    expectTrue(!cachedResult.rebuilt, "guarded rebuild does not rebuild cached basis");
}

void testContactPairRejectPreflightGuards() {
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

    const auto validPreflight =
        fuse::physics::narrowphase::preflight_contact_pair_reject({bodyA, bodyB}, bodies, shapes);
    expectTrue(validPreflight.can_dispatch(), "reject preflight allows valid pair");
    expectTrue(!validPreflight.rejected, "reject preflight does not flag valid pair");
    expectTrue(!validPreflight.selfPair, "reject preflight self flag false for valid pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_has_valid_indices({bodyA, bodyB}, bodies),
        "valid indices guard passes in-range distinct pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_has_shapes({bodyA, bodyB}, shapes),
        "has shapes guard passes for shaped pair");
    expectTrue(
        !fuse::physics::narrowphase::should_reject_contact_pair({bodyA, bodyB}, bodies, shapes),
        "should_reject false for valid pair");

    const auto selfPreflight =
        fuse::physics::narrowphase::preflight_contact_pair_reject({bodyA, bodyA}, bodies, shapes);
    expectTrue(!selfPreflight.can_dispatch(), "reject preflight rejects self pair");
    expectTrue(selfPreflight.selfPair, "reject preflight sets self flag");
    expectTrue(
        fuse::physics::narrowphase::should_reject_contact_pair({bodyA, bodyA}, bodies, shapes),
        "should_reject true for self pair");
    expectTrue(
        !fuse::physics::narrowphase::contact_pair_has_valid_indices({bodyA, bodyA}, bodies),
        "valid indices guard fails for self pair");

    const auto triggerPreflight =
        fuse::physics::narrowphase::preflight_contact_pair_reject({triggerA, triggerB}, bodies, shapes);
    expectTrue(triggerPreflight.bothTriggers, "reject preflight sets both-triggers flag");
    expectTrue(
        triggerPreflight.reason ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothTriggers,
        "reject preflight reports BothTriggers reason");
}

void testManifoldFinalizePreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_finalize_contact_manifold(empty);
    expectTrue(!emptyPreflight.can_finalize(), "finalize preflight rejects empty manifold");
    expectTrue(emptyPreflight.empty, "finalize preflight sets empty flag");
    expectTrue(
        fuse::physics::narrowphase::should_skip_finalize_contact_manifold(empty),
        "should_skip_finalize true for empty manifold");
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_reject_reason(empty) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::Empty,
        "finalize reject reason is Empty");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    const auto readyPreflight = fuse::physics::narrowphase::preflight_finalize_contact_manifold(ready);
    expectTrue(readyPreflight.can_finalize(), "finalize preflight accepts penetrating manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_finalize_contact_manifold(ready),
        "should_skip_finalize false for ready manifold");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_reject_reason(separated) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::NoPenetratingPoints,
        "finalize reject reason flags non-penetrating points");

    fuse::physics::narrowphase::ContactManifold onlySeparated{};
    onlySeparated.contactNormal = {0.f, 1.f, 0.f};
    onlySeparated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    onlySeparated.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::should_skip_prune_contact_manifold(onlySeparated),
        "should_skip_prune true when prune would empty manifold");

    const auto combinedPreflight =
        fuse::physics::narrowphase::preflight_manifold_prune_finalize(ready);
    expectTrue(combinedPreflight.can_finalize_after_prune(), "combined preflight allows finalize on clean manifold");

    fuse::physics::narrowphase::ContactManifold pendingFinalize =
        fuse::physics::narrowphase::collideSphereSphere(
            {0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_if_needed(pendingFinalize),
        "generate_if_needed finalizes valid detected manifold");
    expectTrue(pendingFinalize.hasFrictionBasis(), "generate_if_needed builds friction basis");

    fuse::physics::narrowphase::ContactManifold rejectFinalize{};
    rejectFinalize.contactNormal = {0.f, 1.f, 0.f};
    rejectFinalize.addPoint({0.f, 0.f, 0.f}, -0.05f);
    const bool wasValid = rejectFinalize.valid;
    expectTrue(
        !fuse::physics::narrowphase::generate_contact_manifold_if_needed(rejectFinalize),
        "generate_if_needed rejects without mutation on separated manifold");
    expectTrue(rejectFinalize.valid == wasValid, "generate_if_needed preserves validity on reject");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_finalize_reject_reason_name(
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::PruneWouldEmpty),
            "PruneWouldEmpty") == 0,
        "finalize reject reason name resolves PruneWouldEmpty");
}

void testFrictionBasisRebuildPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(empty);
    expectTrue(emptyPreflight.skipped, "friction preflight skips empty manifold");
    expectTrue(emptyPreflight.can_skip_rebuild(), "friction preflight can skip empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::ensure_friction_basis_if_needed(empty),
        "ensure_if_needed returns false for empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto missingPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(missingPreflight.missing, "friction preflight flags missing basis");
    expectTrue(missingPreflight.needs_rebuild(), "friction preflight needs rebuild without cached basis");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_if_needed(needsBuild),
        "ensure_if_needed builds missing basis");

    const auto cachedTangent1 = needsBuild.frictionBasis.tangent1;
    const auto cachedPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(cachedPreflight.canReuse, "friction preflight can reuse valid basis");
    expectTrue(cachedPreflight.can_skip_rebuild(), "friction preflight skips rebuild for cached basis");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_if_needed(needsBuild),
        "ensure_if_needed succeeds with cached basis");
    expectNear(
        needsBuild.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "ensure_if_needed preserves cached tangent1");

    fuse::physics::narrowphase::ContactManifold stale{};
    stale.contactNormal = {0.f, 1.f, 0.f};
    stale.addPoint({0.f, 0.f, 0.f}, 0.2f);
    stale.buildFrictionBasis();
    stale.contactNormal = {1.f, 0.f, 0.f};
    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(stale);
    expectTrue(stalePreflight.stale, "friction preflight flags stale basis");
    expectTrue(stalePreflight.needs_rebuild(), "friction preflight needs rebuild for stale basis");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_if_needed(stale),
        "ensure_if_needed rebuilds stale basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(stale),
        "ensure_if_needed produces basis matching current normal");
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
        !fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {dynamicA, dynamicB},
        "deepen rejects_for_reason does not false-positive valid pair");

    const fuse::u32 planeA = bodies.addBody({0.f, -1.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 planeB = bodies.addBody({0.f, -2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeA, {0.f, 1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeB, {0.f, 1.f, 0.f});
        fuse::physics::narrowphase::is_plane_plane_contact_pair({planeA, planeB}, shapes),
        "plane-plane guard flags both-plane pair");
        !fuse::physics::narrowphase::is_plane_plane_contact_pair({dynamicA, dynamicB}, shapes),
        "plane-plane guard allows non-plane pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
    };
    const auto batchPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_batch(mixedPairs, bodies, shapes);
    expectTrue(batchPreflight.pairCount == 2u, "batch preflight reports pair count");
    expectTrue(batchPreflight.dispatchableCount == 1u, "batch preflight counts dispatchable pairs");
    expectTrue(batchPreflight.rejectedCount == 1u, "batch preflight counts rejected pairs");
    expectTrue(batchPreflight.can_dispatch(), "batch preflight can dispatch with one valid pair");
        !fuse::physics::narrowphase::narrowphase_batch_rejects_all(mixedPairs, bodies, shapes),
        "batch rejects_all false when one pair dispatchable");
        fuse::physics::narrowphase::narrowphase_batch_rejects_all({{sleepingA, sleepingB}}, bodies, shapes),
        "batch rejects_all true when all pairs deepen-rejected");
}

void testManifoldPruneFinalizeFollowUpGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold),
        "prune rejects_for_reason flags empty manifold");
        std::strcmp(
            fuse::physics::narrowphase::manifold_prune_reject_reason_name(
                fuse::physics::narrowphase::ManifoldPruneRejectReason::AllSeparated),
            "AllSeparated") == 0,
        "prune reject reason name resolves AllSeparated");
void testContactPairEmptyInputGuards() {
    fuse::physics::RigidBodySoA emptyBodies;
    fuse::physics::CollisionShapeSoA emptyShapes;

        fuse::physics::narrowphase::is_empty_narrowphase_input(emptyBodies, emptyShapes),
        "empty-set guard flags no bodies and no shapes");
        fuse::physics::narrowphase::can_skip_narrowphase_for_empty_input(emptyBodies, emptyShapes),
        "skip guard early-outs on empty narrowphase input");

    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
        fuse::physics::narrowphase::is_empty_narrowphase_input(bodies, emptyShapes),
        "empty-set guard flags bodies without shapes");
        fuse::physics::narrowphase::can_skip_narrowphase_for_empty_input(bodies, emptyShapes),
        "skip guard early-outs when shapes are missing");

    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
        !fuse::physics::narrowphase::can_skip_narrowphase_for_empty_input(bodies, shapes),
        "skip guard allows dispatch when bodies and shapes exist");

    const auto rejectedPreflight =
        fuse::physics::narrowphase::preflight_contact_pair({bodyA, bodyA}, bodies, shapes);
        fuse::physics::narrowphase::contact_pair_was_rejected(rejectedPreflight),
        "contact_pair_was_rejected true for rejected preflight");

    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});
    const auto validPreflight =
        fuse::physics::narrowphase::preflight_contact_pair({bodyA, bodyB}, bodies, shapes);
        !fuse::physics::narrowphase::contact_pair_was_rejected(validPreflight),
        "contact_pair_was_rejected false for valid preflight");

void testManifoldFinalizePreflightGuards() {
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_finalize_contact_manifold(empty);
    expectTrue(emptyPreflight.skipped, "finalize preflight skips empty manifold");
    expectTrue(emptyPreflight.empty, "finalize preflight flags empty manifold");
    expectTrue(!emptyPreflight.can_finalize(), "finalize preflight rejects empty manifold");
        fuse::physics::narrowphase::should_skip_finalize_contact_manifold(empty),
        "skip finalize true for empty manifold");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto noNormalPreflight =
        fuse::physics::narrowphase::preflight_finalize_contact_manifold(noNormal);
    expectTrue(noNormalPreflight.invalidNormal, "finalize preflight flags zero-length normal");
    expectTrue(!noNormalPreflight.can_finalize(), "finalize preflight rejects invalid normal");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
            separated, fuse::physics::narrowphase::ManifoldPruneRejectReason::AllSeparated),
        "prune rejects_for_reason flags all-separated manifold");
        !fuse::physics::narrowphase::prune_contact_manifold_with_preflight(separated),
        "prune_with_preflight clears all-separated manifold");
    expectTrue(separated.empty(), "prune_with_preflight clears separated slots");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
        fuse::physics::narrowphase::prune_contact_manifold_with_preflight(dirty),
        "prune_with_preflight keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "prune_with_preflight removes separated slot");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            noNormal, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal),
        "finalize rejects_for_reason flags invalid normal");
        !fuse::physics::narrowphase::finalize_contact_manifold_with_preflight(noNormal),
        "finalize_with_preflight no-ops on invalid normal");

    fuse::physics::narrowphase::ContactManifold ready =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
        fuse::physics::narrowphase::finalize_contact_manifold_with_preflight(ready),
        "finalize_with_preflight finalizes valid manifold");
    expectTrue(ready.valid, "finalize_with_preflight sets validity");
    expectTrue(ready.hasFrictionBasis(), "finalize_with_preflight builds friction basis");

    const auto finalizePreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
        finalizePreflight.reason == fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
        "finalize preflight reports None for finalized manifold");
            fuse::physics::narrowphase::manifold_finalize_reject_reason_name(
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::NoPenetratingPoints),
            "NoPenetratingPoints") == 0,
        "finalize reject reason name resolves NoPenetratingPoints");

void testContactPairDeepenPassDispatchGuards() {

        fuse::physics::narrowphase::should_run_contact_pair_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "should_run_contact_pair_dispatch allows valid pair");
        !fuse::physics::narrowphase::should_run_contact_pair_dispatch({dynamicA, dynamicA}, bodies, shapes),
        "should_run_contact_pair_dispatch rejects self pair");
        fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "should_run_contact_pair_deepen_dispatch allows valid pair");
        !fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({sleepingA, sleepingB}, bodies, shapes),
        "should_run_contact_pair_deepen_dispatch rejects both-sleeping pair");

        fuse::physics::narrowphase::should_run_narrowphase_batch(mixedPairs, bodies, shapes),
        "should_run_narrowphase_batch true when one pair dispatchable");
        !fuse::physics::narrowphase::should_run_narrowphase_batch({{sleepingA, sleepingB}}, bodies, shapes),
        "should_run_narrowphase_batch false when all pairs deepen-rejected");

void testManifoldPruneFinalizeDeepenPassGuards() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
        !fuse::physics::narrowphase::should_run_manifold_prune(clean),
        "should_run_manifold_prune false for clean manifold");
        fuse::physics::narrowphase::should_run_manifold_finalize(clean),
        "should_run_manifold_finalize true for penetrating manifold");
        !fuse::physics::narrowphase::can_prune_manifold_in_place(clean),
        "can_prune_manifold_in_place false when prune is no-op");

        fuse::physics::narrowphase::should_run_manifold_prune(dirty),
        "should_run_manifold_prune true when separated slots exist");
        fuse::physics::narrowphase::can_prune_manifold_in_place(dirty),
        "can_prune_manifold_in_place true when prune keeps penetrating slots");

        !fuse::physics::narrowphase::should_run_manifold_finalize(empty),
        "should_run_manifold_finalize false for empty manifold");

void testFrictionBasisDeepenPassGuards() {
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(empty),
        "should_run_friction_basis_rebuild false for empty manifold");
    const auto separatedPreflight =
        fuse::physics::narrowphase::preflight_finalize_contact_manifold(separated);
    expectTrue(separatedPreflight.allSeparated, "finalize preflight flags all-separated points");
    expectTrue(!separatedPreflight.can_finalize(), "finalize preflight rejects separated manifold");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    const auto readyPreflight = fuse::physics::narrowphase::preflight_finalize_contact_manifold(ready);
    expectTrue(readyPreflight.can_finalize(), "finalize preflight accepts penetrating manifold");
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_guarded(ready),
        "guarded finalize succeeds for ready manifold");
    expectTrue(ready.valid, "guarded finalize sets validity");
    expectTrue(ready.hasFrictionBasis(), "guarded finalize builds friction basis");

    fuse::physics::narrowphase::ContactManifold guardedFail = separated;
        !fuse::physics::narrowphase::generate_contact_manifold_guarded(guardedFail),
        "guarded finalize fails for separated manifold");
    expectTrue(!guardedFail.valid, "guarded finalize clears validity on failure");
}

void testManifoldPruneGuardedDispatch() {
        fuse::physics::narrowphase::should_skip_manifold_prune(clean),
        "should_skip_manifold_prune true for clean manifold");
        fuse::physics::narrowphase::prune_contact_points_guarded(clean),
        "guarded prune preserves clean manifold");
    expectTrue(clean.pointCount == 1u, "guarded prune leaves clean slots untouched");

    fuse::physics::narrowphase::ContactManifold allSeparated{};
    allSeparated.contactNormal = {0.f, 1.f, 0.f};
    allSeparated.addPoint({0.f, 0.f, 0.f}, -0.2f);
        !fuse::physics::narrowphase::prune_contact_points_guarded(allSeparated),
        "guarded prune clears all-separated manifold");
    expectTrue(allSeparated.empty(), "guarded prune empties all-separated manifold");

void testFrictionBasisPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(empty);
    expectTrue(emptyPreflight.skipped, "friction preflight skips empty manifold");
    expectTrue(emptyPreflight.shouldSkipTangents, "friction preflight flags skip-tangents");
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild_preflight(empty),
        "skip preflight true for empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
        fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run_friction_basis_rebuild true without cached basis");

    fuse::physics::narrowphase::compute_friction_tangents(needsBuild);
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run_friction_basis_rebuild false after valid basis built");

void testContactBufferDeepenPassGuards() {
    fuse::physics::narrowphase::ContactBufferSoA empty{};
        fuse::physics::narrowphase::contact_buffer_compaction_rejects_for_reason(
            empty, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "compaction reject reason flags empty buffer");
        fuse::physics::narrowphase::can_skip_contact_buffer_compaction(empty),
        "can_skip_contact_buffer_compaction on empty buffer");
        fuse::physics::narrowphase::contact_buffer_friction_basis_rejects_for_reason(
            empty, fuse::physics::narrowphase::ContactBufferFrictionBasisRejectReason::EmptyBuffer),
        "friction-basis reject reason flags empty buffer");
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
        compactionPreflight.reason == fuse::physics::narrowphase::ContactBufferCompactionRejectReason::NoWork,
        "compaction preflight no-ops after compact");
        fuse::physics::narrowphase::can_skip_contact_buffer_compaction(buffer),
        "can_skip_contact_buffer_compaction after compact");

    const auto frictionPreflight = fuse::physics::narrowphase::preflight_contact_buffer_friction_basis(buffer);
    expectTrue(frictionPreflight.needsRebuild(), "friction-basis preflight allows rebuild with valid slot");
        fuse::physics::narrowphase::should_run_contact_buffer_friction_basis(buffer),
        "should_run_contact_buffer_friction_basis with valid manifold");
    buffer.buildFrictionTangentBases();
    const auto rebuiltBasis = buffer.tangentBasisAt(0u);
        fuse::physics::narrowphase::isOrthonormalTangentBasis({0.f, 1.f, 0.f}, rebuiltBasis),
        "buildFrictionTangentBases stores orthonormal basis through gate");

    fuse::physics::narrowphase::ContactBufferSoA staleValidFlags;
    staleValidFlags.preparePairSlots(2u);
    staleValidFlags.activeCount = 1u;
            staleValidFlags,
            fuse::physics::narrowphase::ContactBufferFrictionBasisRejectReason::NoValidManifolds),
        "friction-basis reject reason flags active count without valid slots");

            fuse::physics::narrowphase::contact_buffer_compaction_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferCompactionRejectReason::NoWork),
            "NoWork") == 0,
        "compaction reject reason name resolves NoWork");
            fuse::physics::narrowphase::contact_buffer_friction_basis_reject_reason_name(
            "NoValidManifolds") == 0,
        "friction-basis reject reason name resolves NoValidManifolds");

void testRunNarrowphaseSkipsDeepenRejectedPairs() {

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {

    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(pairs, bodies, shapes, buffer);
    expectTrue(buffer.activeCount == 1u, "narrowphase skips deepen-rejected sleeping pair");
    const auto restored = buffer.manifoldAt(0u);
    expectTrue(restored.bodyA == dynamicA && restored.bodyB == dynamicB, "narrowphase keeps dispatchable pair");
    expectTrue(restored.hasFrictionBasis(), "narrowphase finalizes friction basis for dispatchable pair");

void testFrictionBasisFollowUpRejectGuards() {
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            empty, fuse::physics::narrowphase::FrictionBasisRejectReason::EmptyManifold),
        "friction rejects_for_reason flags empty manifold");
            fuse::physics::narrowphase::friction_basis_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRejectReason::InvalidNormal),
            "InvalidNormal") == 0,
        "friction reject reason name resolves InvalidNormal");

            noNormal, fuse::physics::narrowphase::FrictionBasisRejectReason::InvalidNormal),
        "friction rejects_for_reason flags invalid normal");
        !fuse::physics::narrowphase::rebuild_friction_basis_with_preflight(noNormal),
        "rebuild_with_preflight skips invalid normal");

        fuse::physics::narrowphase::rebuild_friction_basis_with_preflight(needsBuild),
        "rebuild_with_preflight builds missing basis");
    expectTrue(needsBuild.hasFrictionBasis(), "rebuild_with_preflight stores orthonormal basis");

    const auto cachedTangent1 = needsBuild.frictionBasis.tangent1;
        "rebuild_with_preflight reuses valid basis");
    const auto needsBuildPreflight =
        fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(needsBuildPreflight.missingBasis, "friction preflight flags missing basis");
    expectTrue(needsBuildPreflight.needs_rebuild(), "friction preflight requests rebuild");
    expectTrue(!needsBuildPreflight.can_skip_rebuild(), "friction preflight cannot skip rebuild");

    fuse::physics::narrowphase::rebuild_friction_basis_guarded(needsBuild);
    expectTrue(needsBuild.hasFrictionBasis(), "guarded rebuild produces orthonormal basis");

    const auto cachedPreflight =
    expectTrue(cachedPreflight.can_skip_rebuild(), "friction preflight skips valid cached basis");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild_preflight(needsBuild),
        "skip preflight true for valid cached basis");

    expectNear(
        needsBuild.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "rebuild_with_preflight preserves cached tangent1");

    const auto preflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
        preflight.reason == fuse::physics::narrowphase::FrictionBasisRejectReason::None,
        "friction preflight reports None for valid basis");
    expectTrue(preflight.can_skip_rebuild(), "friction preflight can skip valid basis");
void testManifoldNeedsPruneGuard() {
    clean.addPoint({0.f, 0.f, 0.f}, 0.2f);
    clean.addPoint({1.f, 0.f, 0.f}, 0.15f);
        !fuse::physics::narrowphase::manifold_needs_prune(clean),
        "clean manifold does not need prune");

    separated.addPoint({0.f, 0.f, 0.f}, 0.2f);
    separated.addPoint({1.f, 0.f, 0.f}, -0.05f);
        fuse::physics::narrowphase::manifold_needs_prune(separated),
        "manifold_needs_prune flags separated points");

    fuse::physics::narrowphase::ContactManifold duplicates{};
    duplicates.contactNormal = {0.f, 1.f, 0.f};
    duplicates.addPoint({0.f, 0.f, 0.f}, 0.2f);
    duplicates.addPoint({0.00001f, 0.f, 0.f}, 0.35f);
        fuse::physics::narrowphase::manifold_needs_prune(duplicates),
        "manifold_needs_prune flags duplicate points");

void testManifoldShallowPenetrationAndWarmStartClear() {
    manifold.warmNormalImpulse = 2.f;
    manifold.warmTangentImpulse = {0.5f, -0.25f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.4f);
    manifold.addPoint({1.f, 0.f, 0.f}, 1e-7f);
    manifold.addPoint({2.f, 0.f, 0.f}, 0.1f);

    manifold.pruneShallowPenetrationPoints(1e-6f);
    expectTrue(manifold.pointCount == 2u, "shallow prune drops near-zero penetration points");
    expectNear(manifold.maxPenetration(), 0.4f, 1e-4f, "shallow prune keeps deep points");

    fuse::physics::narrowphase::ContactManifold emptyAfterPrune{};
    emptyAfterPrune.valid = true;
    emptyAfterPrune.contactNormal = {0.f, 1.f, 0.f};
    emptyAfterPrune.warmNormalImpulse = 1.5f;
    emptyAfterPrune.addPoint({0.f, 0.f, 0.f}, -0.2f);
        !emptyAfterPrune.pruneAndRetainPenetrating(),
        "pruneAndRetainPenetrating returns false when all points separate");
    expectTrue(emptyAfterPrune.empty(), "pruneAndRetainPenetrating empties separated manifold");
    expectNear(emptyAfterPrune.warmNormalImpulse, 0.f, 1e-4f, "prune clears warm normal impulse");
    expectNear(emptyAfterPrune.warmTangentImpulse.x, 0.f, 1e-4f, "prune clears warm tangent impulse");

void testFrictionForManifoldCompositeGuard() {
        fuse::physics::narrowphase::should_skip_friction_for_manifold(empty, 0.5f, 0.3f, 1.f),
        "composite friction guard skips empty manifold");

    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
        fuse::physics::narrowphase::should_skip_friction_for_manifold(manifold, 0.f, 0.f, 1.f),
        "composite friction guard skips zero coefficients");
        !fuse::physics::narrowphase::should_skip_friction_for_manifold(manifold, 0.5f, 0.3f, 0.25f),
        "composite friction guard allows valid manifold and coefficients");

    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.f, 0.f, 0.f}, 1.f);
    bodies.frictionStatic[bodyA] = 0.36f;
    bodies.frictionStatic[bodyB] = 0.64f;
    bodies.frictionDynamic[bodyA] = 0.16f;
    bodies.frictionDynamic[bodyB] = 0.25f;
    const fuse::physics::vec2 combined =
        fuse::physics::narrowphase::combine_body_friction_coefficients(bodies, bodyA, bodyB);
    expectNear(combined.x, 0.48f, 1e-4f, "combine_body_friction_coefficients geometric mean static");
    expectNear(combined.y, 0.2f, 1e-4f, "combine_body_friction_coefficients geometric mean dynamic");

        !fuse::physics::narrowphase::should_rebuild_friction_basis({0.f, 1.f, 0.f}, {0.f, 1.f, 0.f}),
        "rebuild guard skips unchanged normal");
        fuse::physics::narrowphase::should_rebuild_friction_basis({0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}),
        "rebuild guard flags orthogonal normal change");

void testContactPairStaticSleepingRejectGuards() {
    const fuse::u32 staticA = bodies.addBody({0.f, 0.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 staticB = bodies.addBody({1.f, 0.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 sleepingA = bodies.addBody({2.f, 0.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({3.f, 0.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 dynamicA = bodies.addBody({4.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({5.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, staticA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, staticB, {1.f, 0.f, 0.f});

        fuse::physics::narrowphase::is_static_static_pair({staticA, staticB}, bodies),
        "static guard detects both-static pair");
        fuse::physics::narrowphase::is_both_sleeping_pair({sleepingA, sleepingB}, bodies),
        "sleeping guard detects both-sleeping pair");
        fuse::physics::narrowphase::contact_pair_reject_reason({staticA, staticB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothStatic,
        "reject reason flags both-static pair");
        fuse::physics::narrowphase::contact_pair_reject_reason({sleepingA, sleepingB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping,
        "reject reason flags both-sleeping pair");
            fuse::physics::narrowphase::contact_pair_reject_reason_label(
                fuse::physics::narrowphase::ContactPairRejectReason::BothStatic),
            "both_static") == 0,
        "reject reason label for both-static pair");
        fuse::physics::narrowphase::contact_pair_should_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "dispatch guard allows dynamic pair");
        !fuse::physics::narrowphase::contact_pair_should_dispatch({staticA, staticB}, bodies, shapes),
        "dispatch guard rejects both-static pair");

    const auto staticPair =
        fuse::physics::narrowphase::detect_contacts_pair({staticA, staticB}, bodies, shapes);
    expectTrue(!staticPair.valid, "both-static pair returns invalid manifold");
    expectTrue(staticPair.empty(), "both-static pair has no contact points");
        "guarded rebuild preserves valid cached basis");

    fuse::physics::narrowphase::ContactManifold stale{};
    stale.contactNormal = {0.f, 1.f, 0.f};
    stale.addPoint({0.f, 0.f, 0.f}, 0.2f);
    stale.buildFrictionBasis();
    stale.contactNormal = {1.f, 0.f, 0.f};
    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(stale);
    expectTrue(stalePreflight.staleBasis, "friction preflight flags stale basis");
    expectTrue(stalePreflight.needs_rebuild(), "friction preflight requests stale rebuild");

    fuse::physics::narrowphase::rebuild_friction_basis_guarded(stale);
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(stale),
        "guarded rebuild refreshes stale basis");
}

void testDetectContactsPairGuarded() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    const auto selfPair = fuse::physics::narrowphase::detect_contacts_pair_guarded(
        {bodyA, bodyA}, bodies, shapes);
    expectTrue(!selfPair.valid, "guarded detect rejects self pair");
    expectTrue(selfPair.empty(), "guarded detect returns empty manifold for self pair");

    const auto overlap = fuse::physics::narrowphase::detect_contacts_pair_guarded(
        {bodyA, bodyB}, bodies, shapes);
    expectTrue(overlap.valid, "guarded detect accepts valid pair");
    expectTrue(overlap.pointCount > 0u, "guarded detect populates contact points");
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
    testContactManifoldFinalizePreflightGuards();
    testNarrowphaseDispatchPreflightWiring();
    testFrictionBasisGuardHelpers();
    testContactPairRejectBreakdownGuards();
    testManifoldFinalizePreflightGuards();
    testFrictionBasisPreflightGuards();
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
    testContactPairRejectReasonNames();
    testPenetratingPointCounts();
    testManifoldPruneIfEmpty();
    testCachedFrictionBasisHelpers();
    testIsValidContactManifoldGuards();
    testContactPairSleepingKinematicGuards();
    testManifoldFinalizeGuards();
    testFrictionBasisRebuildGuards();
    testFrictionBasisRebuildGuardOverloads();
    testContactPairDispatchPathGuards();
    testManifoldPrunePassTwoGuards();
    testFrictionBasisRebuildPassTwoGuards();
    testContactPairDispatchGuards();
    testManifoldPrunePreflightExGuards();
    testContactPairDeepenPreflightGuards();
    testManifoldPruneDeepenPreflightGuards();
    testContactPairEmptyInputGuards();
    testManifoldPruneGuardedDispatch();
    testDetectContactsPairGuarded();
    testContactPairB45DispatchGuards();
    testManifoldFinalizeB45Guards();
    testFrictionBasisB45PreflightGuards();
    testContactPairRejectPreflightGuards();
    testFrictionBasisRebuildPreflightGuards();
    testGjkSupportAndEpaStub();
    testContactPairDeepenFollowUpRejectGuards();
    testManifoldPruneFinalizeFollowUpGuards();
    testFrictionBasisFollowUpRejectGuards();
    testContactPairDeepenPassDispatchGuards();
    testManifoldPruneFinalizeDeepenPassGuards();
    testFrictionBasisDeepenPassGuards();
    testContactBufferDeepenPassGuards();
    testRunNarrowphaseSkipsDeepenRejectedPairs();
    testManifoldNeedsPruneGuard();
    testManifoldShallowPenetrationAndWarmStartClear();
    testFrictionForManifoldCompositeGuard();
    testContactPairStaticSleepingRejectGuards();

    if (g_failures == 0) {
        std::printf("fuse_physics_narrowphase_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_narrowphase_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
