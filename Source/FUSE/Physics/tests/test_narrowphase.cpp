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

void expectEq(fuse::u32 actual, fuse::u32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %u, got %u)\n", message, expected, actual);
        std::fprintf(stderr, "FAIL: %s (got %u expected %u)\n", message, actual, expected);
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

void testContactPairPreflightDeepenGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyNoShape = bodies.addBody({2.f, 0.f, 0.f}, 1.f);
    const fuse::u32 triggerA = bodies.addBody({3.f, 0.f, 0.f}, 0.f, fuse::physics::RB_TRIGGER);
    const fuse::u32 triggerB = bodies.addBody({3.5f, 0.f, 0.f}, 0.f, fuse::physics::RB_TRIGGER);
    const fuse::u32 staticA = bodies.addBody({0.f, 2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 staticB = bodies.addBody({0.f, 3.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, triggerA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, triggerB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, staticA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, staticB, {1.f, 0.f, 0.f});

    const auto validPreflight =
        fuse::physics::narrowphase::preflight_contact_pair({bodyA, bodyB}, bodies, shapes);
    expectTrue(validPreflight.can_dispatch(), "deepen preflight allows valid pair");
    expectTrue(
        fuse::physics::narrowphase::can_dispatch_contact_pair(validPreflight),
        "can_dispatch_contact_pair mirrors can_dispatch");
    expectTrue(!validPreflight.isSelfPair, "valid preflight has no self flag");
    expectTrue(!validPreflight.isOutOfRange, "valid preflight has no OOB flag");
    expectTrue(!validPreflight.isMissingShape, "valid preflight has no missing-shape flag");

    const auto missingShapePreflight =
        fuse::physics::narrowphase::preflight_contact_pair({bodyA, bodyNoShape}, bodies, shapes);
    expectTrue(!missingShapePreflight.can_dispatch(), "deepen preflight rejects missing shape");
    expectTrue(missingShapePreflight.isMissingShape, "deepen preflight flags missing shape");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_preflight_rejects_for_reason(
            missingShapePreflight,
            fuse::physics::narrowphase::ContactPairRejectReason::MissingShape),
        "preflight_rejects_for_reason matches missing shape");

    const auto triggerPreflight =
        fuse::physics::narrowphase::preflight_contact_pair({triggerA, triggerB}, bodies, shapes);
    expectTrue(triggerPreflight.isBothTriggers, "deepen preflight flags both-trigger pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_preflight_matches_reason(
            triggerPreflight,
            fuse::physics::narrowphase::ContactPairRejectReason::BothTriggers),
        "preflight_matches_reason for both-trigger");

    const auto staticPreflight =
        fuse::physics::narrowphase::preflight_contact_pair({staticA, staticB}, bodies, shapes);
    expectTrue(staticPreflight.isBothStatic, "deepen preflight flags both-static pair");

    const fuse::u32 zeroRadiusBody = bodies.addBody({5.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, zeroRadiusBody, {0.f, 0.f, 0.f});
    const auto degeneratePreflight =
        fuse::physics::narrowphase::preflight_contact_pair({bodyA, zeroRadiusBody}, bodies, shapes);
    expectTrue(degeneratePreflight.isDegenerateShape, "deepen preflight flags degenerate shape");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_preflight_rejects_for_reason(
            degeneratePreflight,
            fuse::physics::narrowphase::ContactPairRejectReason::DegenerateShape),
        "preflight_rejects_for_reason matches degenerate shape");
}

void testManifoldFinalizePreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(empty);
    expectTrue(emptyPreflight.skipped, "finalize preflight skips empty manifold");
    expectTrue(emptyPreflight.isEmpty, "finalize preflight flags empty");
    expectTrue(!emptyPreflight.can_finalize(), "finalize preflight cannot finalize empty");
    expectTrue(
        fuse::physics::narrowphase::should_skip_manifold_finalize(empty),
        "should_skip_manifold_finalize true for empty");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto noNormalPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(noNormal);
    expectTrue(noNormalPreflight.hasInvalidNormal, "finalize preflight flags invalid normal");
    expectTrue(!noNormalPreflight.can_finalize(), "finalize preflight rejects invalid normal");
    expectTrue(
        !fuse::physics::narrowphase::can_finalize_with_preflight(noNormalPreflight),
        "can_finalize_with_preflight rejects invalid normal");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    const auto separatedPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(separated);
    expectTrue(separatedPreflight.hasNoPenetratingPoints, "finalize preflight flags all-separated");
    expectTrue(
        fuse::physics::narrowphase::should_skip_manifold_finalize(separated),
        "should_skip_manifold_finalize true for separated");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    const auto readyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(readyPreflight.can_finalize(), "finalize preflight accepts penetrating manifold");
    expectTrue(
        fuse::physics::narrowphase::can_finalize_with_preflight(readyPreflight),
        "can_finalize_with_preflight accepts ready manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_manifold_finalize(ready),
        "should_skip_manifold_finalize false for ready manifold");
    expectTrue(
        fuse::physics::narrowphase::can_finalize_contact_manifold(ready) ==
            readyPreflight.can_finalize(),
        "finalize preflight agrees with can_finalize_contact_manifold");
}

void testManifoldPruneShallowPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold shallow{};
    shallow.contactNormal = {0.f, 1.f, 0.f};
    shallow.addPoint({0.f, 0.f, 0.f}, 0.5f);
    shallow.addPoint({1.f, 0.f, 0.f}, 0.01f);

    expectTrue(!shallow.canSkipPruneShallowPenetrations(0.05f), "canSkipPruneShallow false with shallow slot");
    expectTrue(shallow.canSkipPruneShallowPenetrations(0.f), "canSkipPruneShallow true when minDepth is zero");

    const auto shallowPreflight =
        fuse::physics::narrowphase::preflight_manifold_prune(shallow, 1e-6f, 1e-4f, 0.05f);
    expectTrue(shallowPreflight.hasShallowPenetrations, "prune preflight flags shallow penetrations");
    expectTrue(shallowPreflight.needs_pruning(), "prune preflight needs pruning with shallow slot");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    clean.addPoint({1.f, 0.f, 0.f}, 0.25f);
    const auto cleanPreflight =
        fuse::physics::narrowphase::preflight_manifold_prune(clean, 1e-6f, 1e-4f, 0.05f);
    expectTrue(!cleanPreflight.hasShallowPenetrations, "prune preflight clean without shallow slots");
    expectTrue(!cleanPreflight.needs_pruning(), "prune preflight skips clean manifold with shallow check");
}

void testFrictionBasisPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(empty);
    expectTrue(emptyPreflight.skipped, "friction preflight skips empty manifold");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild_preflight(emptyPreflight),
        "skip preflight true for empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto needsBuildPreflight =
        fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(needsBuildPreflight.needs_rebuild(), "friction preflight needs rebuild without cache");
    expectTrue(!needsBuildPreflight.can_reuse(), "friction preflight cannot reuse missing basis");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_friction_basis_rebuild_preflight(needsBuildPreflight),
        "skip preflight false when rebuild needed");

    needsBuild.buildFrictionBasis();
    const auto cachedPreflight =
        fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(cachedPreflight.hasCachedBasis, "friction preflight detects cached basis");
    expectTrue(cachedPreflight.can_reuse(), "friction preflight can reuse valid basis");
    expectTrue(
        fuse::physics::narrowphase::can_reuse_friction_basis(cachedPreflight),
        "can_reuse_friction_basis mirrors can_reuse");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild_preflight(cachedPreflight),
        "skip preflight true for reusable basis");

    needsBuild.contactNormal = {1.f, 0.f, 0.f};
    const auto stalePreflight =
        fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(stalePreflight.stale, "friction preflight flags stale basis");
    expectTrue(stalePreflight.wouldRebuild, "friction preflight would rebuild stale basis");
    expectTrue(stalePreflight.needs_rebuild(), "friction preflight needs rebuild when stale");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_friction_basis_rebuild_preflight(stalePreflight),
        "skip preflight false for stale basis");
}

void testContactPairKinematicSleepingRejectGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 kinA = bodies.addBody({0.f, 0.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    const fuse::u32 kinB = bodies.addBody({1.5f, 0.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    const fuse::u32 sleepA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepB = bodies.addBody({1.5f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, kinA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, kinB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::is_kinematic_contact_pair({kinA, kinB}, bodies),
        "kinematic guard detects both-kinematic pair");
    expectTrue(
        fuse::physics::narrowphase::is_sleeping_contact_pair({sleepA, sleepB}, bodies),
        "sleeping guard detects both-sleeping pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({kinA, kinB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothKinematic,
        "reject reason flags both-kinematic pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({sleepA, sleepB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping,
        "reject reason flags both-sleeping pair");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::BothKinematic),
            "BothKinematic") == 0,
        "reject reason name resolves BothKinematic");

    const auto kinPair =
        fuse::physics::narrowphase::detect_contacts_pair({kinA, kinB}, bodies, shapes);
    expectTrue(!kinPair.valid, "both-kinematic pair returns invalid manifold");
}

void testContactPairDispatchPreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    const auto validPreflight =
        fuse::physics::narrowphase::preflight_contact_pair_dispatch({bodyA, bodyB}, bodies, shapes);
    expectTrue(validPreflight.can_dispatch(), "dispatch preflight allows valid pair");
    expectTrue(validPreflight.has_valid_bodies, "dispatch preflight marks valid bodies");
    expectTrue(validPreflight.has_valid_shapes, "dispatch preflight marks valid shapes");
    expectTrue(
        !fuse::physics::narrowphase::contact_pair_has_reject_reason({bodyA, bodyB}, bodies, shapes),
        "valid pair has no reject reason");
    expectTrue(
        !fuse::physics::narrowphase::should_reject_contact_pair({bodyA, bodyB}, bodies, shapes),
        "should_reject false for valid pair");

    const auto selfPreflight =
        fuse::physics::narrowphase::preflight_contact_pair_dispatch({bodyA, bodyA}, bodies, shapes);
    expectTrue(!selfPreflight.can_dispatch(), "dispatch preflight rejects self pair");
    expectTrue(!selfPreflight.has_valid_bodies, "self pair fails body validity");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_has_reject_reason({bodyA, bodyA}, bodies, shapes),
        "self pair has reject reason");

    const auto dispatch =
        fuse::physics::narrowphase::dispatch_contact_pair({bodyA, bodyB}, bodies, shapes);
    expectTrue(dispatch.dispatched, "dispatch_contact_pair runs shape dispatch");
    expectTrue(dispatch.manifold.valid, "dispatch_contact_pair returns valid manifold");
    expectTrue(dispatch.preflight.can_dispatch(), "dispatch result preserves valid preflight");

    const auto rejected =
        fuse::physics::narrowphase::dispatch_contact_pair({bodyA, bodyA}, bodies, shapes);
    expectTrue(!rejected.dispatched, "dispatch_contact_pair skips rejected pair");
    expectTrue(!rejected.manifold.valid, "rejected dispatch leaves invalid manifold");
}

void testManifoldFinalizePreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(empty);
    expectTrue(emptyPreflight.skipped, "finalize preflight skips empty manifold");
    expectTrue(!emptyPreflight.can_finalize(), "empty manifold cannot finalize");
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_finalize(empty),
        "can_skip_manifold_finalize true for empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::generate_contact_manifold_guarded(empty),
        "guarded finalize returns false without clearing empty manifold");
    expectTrue(empty.empty(), "guarded finalize leaves empty manifold untouched");

    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.3f);
    const auto unnormalizedPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(unnormalized);
    expectTrue(unnormalizedPreflight.needsNormalization, "finalize preflight flags non-unit normal");
    expectTrue(
        fuse::physics::narrowphase::needs_normal_normalization(unnormalized),
        "needs_normal_normalization flags scaled normal");
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_guarded(unnormalized),
        "guarded finalize succeeds for penetrating manifold");
    expectTrue(unnormalized.hasFrictionBasis(), "guarded finalize builds friction basis");
    expectNear(unnormalized.contactNormal.length(), 1.f, 1e-4f, "guarded finalize normalizes contact normal");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.2f);
    const auto separatedPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(separated);
    expectTrue(separatedPreflight.noPenetrating, "finalize preflight flags all-separated points");
    expectTrue(
        !fuse::physics::narrowphase::generate_contact_manifold_guarded(separated),
        "guarded finalize rejects separated manifold");
    expectTrue(separated.pointCount == 1u, "guarded finalize leaves separated slots untouched");
}

void testManifoldShallowPruneSkipGuards() {
    fuse::physics::narrowphase::ContactManifold shallow{};
    shallow.contactNormal = {0.f, 1.f, 0.f};
    shallow.addPoint({0.f, 0.f, 0.f}, 0.5f);
    shallow.addPoint({1.f, 0.f, 0.f}, 0.01f);
    expectTrue(!shallow.canSkipPruneShallowPenetrations(0.05f), "shallow skip false when shallow slot exists");
    expectTrue(shallow.pruneShallowPenetrationsIfNeeded(0.05f), "conditional shallow prune keeps deep slot");
    expectTrue(shallow.pointCount == 1u, "conditional shallow prune removes shallow slot");

    fuse::physics::narrowphase::ContactManifold deepOnly{};
    deepOnly.contactNormal = {0.f, 1.f, 0.f};
    deepOnly.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(deepOnly.canSkipPruneShallowPenetrations(0.05f), "shallow skip true without shallow slots");
    expectTrue(deepOnly.pruneShallowPenetrationsIfNeeded(0.05f), "conditional shallow prune preserves deep-only manifold");
    expectTrue(deepOnly.pointCount == 1u, "conditional shallow prune leaves deep-only slots untouched");
}

void testFrictionBasisRebuildPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(empty);
    expectTrue(emptyPreflight.skipped, "friction rebuild preflight skips empty manifold");
    expectTrue(emptyPreflight.skipTangents, "friction rebuild preflight marks skip tangents");
    expectTrue(
        !fuse::physics::narrowphase::compute_friction_tangents_guarded(empty),
        "guarded friction compute returns false for empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto needsPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(needsPreflight.needs_rebuild(), "rebuild preflight true without cached basis");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_guarded(needsBuild),
        "guarded ensure builds orthonormal basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(needsBuild),
        "guarded ensure produces matching basis");

    const auto cachedPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(cachedPreflight.can_skip_rebuild(), "rebuild preflight skips valid cached basis");
    const auto cachedTangent1 = needsBuild.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::compute_friction_tangents_guarded(needsBuild),
        "guarded compute reuses cached basis");
    expectNear(
        needsBuild.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "guarded compute preserves cached tangent1");

    fuse::physics::narrowphase::ContactManifold stale{};
    stale.contactNormal = {0.f, 1.f, 0.f};
    stale.addPoint({0.f, 0.f, 0.f}, 0.2f);
    stale.buildFrictionBasis();
    stale.contactNormal = {1.f, 0.f, 0.f};
    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(stale);
    expectTrue(stalePreflight.isStale, "rebuild preflight flags stale cached basis");
    expectTrue(stalePreflight.needs_rebuild(), "rebuild preflight requires refresh for stale basis");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_guarded(stale),
        "guarded ensure refreshes stale basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(stale),
        "guarded ensure refreshes basis to current normal");
}

void testContactPairRejectPreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 staticA = bodies.addBody({0.f, 2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 staticB = bodies.addBody({0.f, 3.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, staticA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, staticB, {1.f, 0.f, 0.f});

    const auto validReject =
        fuse::physics::narrowphase::preflight_contact_pair_reject({bodyA, bodyB}, bodies, shapes);
    expectTrue(!validReject.rejected, "reject preflight allows valid pair");
    expectTrue(validReject.can_dispatch(), "reject preflight can_dispatch for valid pair");
    expectTrue(!validReject.selfPair, "valid pair is not self pair");

    const auto selfReject =
        fuse::physics::narrowphase::preflight_contact_pair_reject({bodyA, bodyA}, bodies, shapes);
    expectTrue(selfReject.rejected, "reject preflight marks self pair");
    expectTrue(selfReject.selfPair, "reject preflight flags selfPair");
    expectTrue(
        fuse::physics::narrowphase::should_reject_contact_pair({bodyA, bodyA}, bodies, shapes),
        "should_reject matches self pair");

    const auto staticReject =
        fuse::physics::narrowphase::preflight_contact_pair_reject({staticA, staticB}, bodies, shapes);
    expectTrue(staticReject.rejected, "reject preflight marks both-static pair");
    expectTrue(staticReject.bothStatic, "reject preflight flags bothStatic");

    const auto dispatchPreflight =
        fuse::physics::narrowphase::preflight_contact_pair_dispatch({bodyA, bodyB}, bodies, shapes);
    expectTrue(dispatchPreflight.can_dispatch(), "dispatch preflight allows valid pair");
    expectTrue(!dispatchPreflight.skipped, "dispatch preflight does not skip valid pair");

    const auto skippedDispatch =
        fuse::physics::narrowphase::preflight_contact_pair_dispatch({bodyA, bodyA}, bodies, shapes);
    expectTrue(skippedDispatch.skipped, "dispatch preflight skips rejected pair");
    expectTrue(!skippedDispatch.can_dispatch(), "dispatch preflight cannot dispatch self pair");
}

void testManifoldFinalizePreflightGuards() {
    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);

    const auto readyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(readyPreflight.can_finalize(), "finalize preflight accepts penetrating manifold");
    expectTrue(readyPreflight.hasValidNormal, "finalize preflight sees valid normal");
    expectTrue(readyPreflight.hasPenetrating, "finalize preflight sees penetrating points");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_manifold_finalize(ready),
        "should_skip false for ready manifold");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_manifold_finalize(ready),
        "can_skip false for ready manifold");

    fuse::physics::narrowphase::ContactManifold separated = ready;
    separated.points[0].penetration = -0.1f;
    const auto separatedPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(separated);
    expectTrue(!separatedPreflight.can_finalize(), "finalize preflight rejects separated points");
    expectTrue(
        fuse::physics::narrowphase::should_skip_manifold_finalize(separated),
        "should_skip true for separated manifold");

    fuse::physics::narrowphase::ContactManifold manifold =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(manifold.valid, "detected sphere pair valid before conditional finalize");
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_if_valid(manifold),
        "conditional finalize succeeds for valid manifold");
    expectTrue(manifold.hasFrictionBasis(), "conditional finalize builds friction basis");

    fuse::physics::narrowphase::ContactManifold invalid{};
    invalid.contactNormal = {0.f, 1.f, 0.f};
    invalid.addPoint({0.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        !fuse::physics::narrowphase::generate_contact_manifold_if_valid(invalid),
        "conditional finalize rejects invalid manifold");
}

void testFrictionBasisRebuildPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold fresh{};
    fresh.contactNormal = {0.f, 1.f, 0.f};
    fresh.addPoint({0.f, 0.f, 0.f}, 0.2f);

    const auto missingPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(fresh);
    expectTrue(missingPreflight.missing, "rebuild preflight flags missing basis");
    expectTrue(missingPreflight.needs_rebuild(), "rebuild preflight needs build when missing");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_friction_basis_rebuild(fresh),
        "should_skip false when basis missing");

    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_from_preflight(fresh),
        "rebuild from preflight builds basis");
    expectTrue(fresh.hasFrictionBasis(), "rebuild from preflight stores orthonormal basis");

    const auto cachedPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(fresh);
    expectTrue(cachedPreflight.canReuse, "rebuild preflight canReuse valid cached basis");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_rebuild(fresh),
        "should_skip true when basis cached");

    const auto cachedTangent1 = fresh.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_from_preflight(fresh),
        "rebuild from preflight reuses cached basis");
    expectNear(
        fresh.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "rebuild from preflight preserves cached tangent1");

    fuse::physics::narrowphase::ContactManifold stale = fresh;
    stale.contactNormal = {1.f, 0.f, 0.f};
    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(stale);
    expectTrue(stalePreflight.stale, "rebuild preflight flags stale basis");
    expectTrue(stalePreflight.needs_rebuild(), "rebuild preflight needs rebuild when stale");
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_from_preflight(stale),
        "rebuild from preflight refreshes stale basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(stale),
        "rebuild from preflight matches current normal");

    fuse::physics::narrowphase::ContactManifold empty{};
    const auto skippedPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(empty);
    expectTrue(skippedPreflight.skipped, "rebuild preflight skips empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::rebuild_friction_basis_from_preflight(empty),
        "rebuild from preflight returns false for empty manifold");
}

void testRunNarrowphaseDeepenDispatchGuards() {
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

    const std::vector<fuse::physics::broadphase::CandidatePair> deepenRejectedPairs = {
        {sleepingA, sleepingB},
        {kinematicA, kinematicB},
    };

    fuse::physics::narrowphase::ContactBufferSoA rejectedBuffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(deepenRejectedPairs, bodies, shapes, rejectedBuffer);
    expectTrue(rejectedBuffer.isEmpty(), "deepen dispatch skips sleeping and kinematic pairs");
    expectTrue(
        fuse::physics::narrowphase::can_skip_narrowphase(deepenRejectedPairs, bodies, shapes),
        "can_skip_narrowphase true for all deepen-rejected pairs");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {dynamicA, dynamicB},
    };

    fuse::physics::narrowphase::ContactBufferSoA mixedBuffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(mixedPairs, bodies, shapes, mixedBuffer);
    expectTrue(mixedBuffer.activeCount == 1u, "mixed pair list keeps only dispatchable contact");
    const auto contact = mixedBuffer.manifoldAt(0u);
    expectTrue(contact.valid, "dispatchable pair still finalizes in hot path");
    expectTrue(contact.hasFrictionBasis(), "dispatchable pair still builds friction basis");
}

void testBuildFrictionTangentBasesReuseGuard() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(1u);

    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    manifold.buildFrictionBasis();
    buffer.writeSlot(0u, manifold);
    expectTrue(buffer.compact() == 1u, "reuse guard test compacts one manifold");

    const auto cachedTangent1 = buffer.tangentBasisAt(0u).tangent1;
    buffer.buildFrictionTangentBases();
    expectNear(
        buffer.tangentBasisAt(0u).tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "buildFrictionTangentBases preserves valid cached basis");

    fuse::physics::narrowphase::ContactManifold stale = buffer.manifoldAt(0u);
    stale.contactNormal = {1.f, 0.f, 0.f};
    buffer.writeSlot(0u, stale);
    buffer.compact();
    buffer.buildFrictionTangentBases();
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis(
            {1.f, 0.f, 0.f}, buffer.tangentBasisAt(0u)),
        "buildFrictionTangentBases refreshes stale basis");
}

void testNarrowphaseRejectReasonGuards() {
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
        fuse::physics::narrowphase::narrowphase_rejects_for_reason(
            {}, bodies, shapes, fuse::physics::narrowphase::NarrowphaseRejectReason::EmptyPairList),
        "empty pair list rejects for EmptyPairList");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::narrowphase_reject_reason_name(
                fuse::physics::narrowphase::NarrowphaseRejectReason::AllPairsRejected),
            "AllPairsRejected") == 0,
        "AllPairsRejected narrowphase reject reason has stable label");
    expectTrue(
        fuse::physics::narrowphase::narrowphase_rejects_for_reason(
            {{sleepingA, sleepingB}}, bodies, shapes,
            fuse::physics::narrowphase::NarrowphaseRejectReason::AllPairsRejected),
        "all deepen-rejected pairs reject for AllPairsRejected");
    expectTrue(
        fuse::physics::narrowphase::narrowphase_rejects_for_reason(
            {{bodyA, bodyB}}, bodies, shapes, fuse::physics::narrowphase::NarrowphaseRejectReason::None),
        "dispatchable pair reports None narrowphase reject reason");
    expectTrue(
        fuse::physics::narrowphase::can_skip_narrowphase({{sleepingA, sleepingB}}, bodies, shapes),
        "can_skip_narrowphase when all pairs deepen-rejected");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_narrowphase({{bodyA, bodyB}}, bodies, shapes),
        "can_skip_narrowphase false when dispatchable pair exists");
}

void testContactPairDeepenRejectsForReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
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
            {dynamicA, sleepingA},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejects_for_reason does not false-positive mixed pair");
}

void testNarrowphasePairListPreflightGuards() {
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

    const auto emptyPreflight = fuse::physics::narrowphase::preflight_narrowphase_pair_list({}, bodies, shapes);
    expectTrue(!emptyPreflight.can_dispatch(), "empty pair-list preflight cannot dispatch");
    expectTrue(emptyPreflight.emptyPairList, "empty pair-list preflight marks empty list");

    const auto rejectedPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_pair_list({{sleepingA, sleepingB}}, bodies, shapes);
    expectTrue(!rejectedPreflight.can_dispatch(), "all-rejected pair-list preflight cannot dispatch");
    expectTrue(rejectedPreflight.allPairsRejected, "all-rejected pair-list preflight marks all rejected");

    const auto mixedPreflight = fuse::physics::narrowphase::preflight_narrowphase_pair_list(
        {{sleepingA, sleepingB}, {bodyA, bodyB}}, bodies, shapes);
    expectTrue(mixedPreflight.can_dispatch(), "mixed pair-list preflight can dispatch");
    expectTrue(mixedPreflight.dispatchablePairCount == 1u, "mixed pair-list preflight counts dispatchable pairs");
}

void testManifoldPruneRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold),
        "empty manifold rejects for EmptyManifold prune reason");
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_prune(empty),
        "can_skip_manifold_prune on empty manifold");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::NoPruningNeeded),
        "clean manifold rejects for NoPruningNeeded prune reason");
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_prune(clean),
        "can_skip_manifold_prune on clean manifold");
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(clean),
        "prune_contact_manifold_if_needed no-ops on clean manifold");
    expectTrue(clean.pointCount == 1u, "prune_contact_manifold_if_needed preserves clean slots");

    fuse::physics::narrowphase::ContactManifold allSeparated{};
    allSeparated.contactNormal = {0.f, 1.f, 0.f};
    allSeparated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            allSeparated, fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmptyAfterPrune),
        "all-separated manifold rejects for WouldBeEmptyAfterPrune");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_prune_reject_reason_name(
                fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmptyAfterPrune),
            "WouldBeEmptyAfterPrune") == 0,
        "WouldBeEmptyAfterPrune prune reject reason has stable label");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            dirty, fuse::physics::narrowphase::ManifoldPruneRejectReason::None),
        "dirty manifold reports None prune reject reason");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_manifold_prune(dirty),
        "can_skip_manifold_prune false when in-place prune may proceed");
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(dirty),
        "prune_contact_manifold_if_needed keeps penetrating slot");
    expectTrue(dirty.pointCount == 1u, "prune_contact_manifold_if_needed removes separated slot");

    const auto dirtyPreflight = fuse::physics::narrowphase::preflight_manifold_prune(dirty);
    expectTrue(
        dirtyPreflight.reason == fuse::physics::narrowphase::ManifoldPruneRejectReason::NoPruningNeeded,
        "pruned manifold preflight carries NoPruningNeeded reason");
}

void testManifoldFinalizeRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold),
        "empty manifold rejects for EmptyManifold finalize reason");
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_finalize(empty),
        "can_skip_manifold_finalize on empty manifold");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            noNormal, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal),
        "zero-length normal rejects for InvalidNormal finalize reason");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            ready, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None),
        "ready manifold reports None finalize reject reason");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_manifold_finalize(ready),
        "can_skip_manifold_finalize false for ready manifold");

    const auto readyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(readyPreflight.can_finalize(), "finalize preflight accepts ready manifold with reason None");
    expectTrue(
        readyPreflight.reason == fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
        "finalize preflight carries reject reason");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            separated, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune),
        "separated manifold rejects for WouldBeEmptyAfterPrune finalize reason");
}

void testFrictionBasisRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            empty, fuse::physics::narrowphase::FrictionBasisRejectReason::SkippedManifold),
        "empty manifold rejects for SkippedManifold friction reason");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_preflight(empty),
        "should_skip_friction_basis_preflight on empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            needsBuild, fuse::physics::narrowphase::FrictionBasisRejectReason::None),
        "missing basis reports None friction reject reason");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_friction_basis_preflight(needsBuild),
        "should_skip_friction_basis_preflight false when rebuild may proceed");

    needsBuild.buildFrictionBasis();
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            needsBuild, fuse::physics::narrowphase::FrictionBasisRejectReason::ValidCachedBasis),
        "valid cached basis rejects for ValidCachedBasis friction reason");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::friction_basis_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRejectReason::ValidCachedBasis),
            "ValidCachedBasis") == 0,
        "ValidCachedBasis friction reject reason has stable label");

    const auto reusePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(reusePreflight.can_skip_rebuild(), "friction preflight can skip valid cached basis");
    expectTrue(
        reusePreflight.reason == fuse::physics::narrowphase::FrictionBasisRejectReason::ValidCachedBasis,
        "friction preflight carries reject reason");

    needsBuild.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            needsBuild, fuse::physics::narrowphase::FrictionBasisRejectReason::None),
        "stale basis reports None friction reject reason");
    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(stalePreflight.needsRebuild, "stale friction preflight needs rebuild");
    expectTrue(!stalePreflight.can_skip_rebuild(), "stale friction preflight cannot skip rebuild");
}

void testContactPairDeepenRejectsForReasonGuards() {
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
}

void testNarrowphaseBatchPreflightGuards() {
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

    const auto emptyPreflight =
        fuse::physics::narrowphase::preflight_narrowphase({}, bodies, shapes);
    expectTrue(!emptyPreflight.can_run(), "batch preflight cannot run on empty pair list");
    expectTrue(emptyPreflight.totalPairs == 0u, "batch preflight reports zero total pairs");

    const std::vector<fuse::physics::broadphase::CandidatePair> allRejected = {
        {sleepingA, sleepingB},
    };
    const auto rejectedPreflight =
        fuse::physics::narrowphase::preflight_narrowphase(allRejected, bodies, shapes);
    expectTrue(!rejectedPreflight.can_run(), "batch preflight cannot run when all pairs rejected");
    expectTrue(rejectedPreflight.rejectedPairs == 1u, "batch preflight counts rejected pair");
    expectTrue(
        fuse::physics::narrowphase::count_dispatchable_contact_pairs(allRejected, bodies, shapes) == 0u,
        "count_dispatchable returns zero for all-rejected list");
    expectTrue(
        !fuse::physics::narrowphase::has_dispatchable_contact_pair(allRejected, bodies, shapes),
        "has_dispatchable false when all pairs rejected");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixed = {
        {sleepingA, sleepingB},
        {bodyA, bodyB},
    };
    const auto mixedPreflight = fuse::physics::narrowphase::preflight_narrowphase(mixed, bodies, shapes);
    expectTrue(mixedPreflight.can_run(), "batch preflight can run with dispatchable pair");
    expectTrue(mixedPreflight.dispatchablePairs == 1u, "batch preflight counts one dispatchable pair");
    expectTrue(mixedPreflight.rejectedPairs == 1u, "batch preflight counts one rejected pair");
    expectTrue(
        fuse::physics::narrowphase::has_dispatchable_contact_pair(mixed, bodies, shapes),
        "has_dispatchable true when mixed list has dispatchable pair");
}

void testManifoldPruneRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_reject_reason(empty) ==
            fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold,
        "prune reject reason flags empty manifold");
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold),
        "prune rejects_for_reason matches empty manifold");
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_prune(empty),
        "can_skip_manifold_prune on empty manifold");

    fuse::physics::narrowphase::ContactManifold allSeparated{};
    allSeparated.contactNormal = {0.f, 1.f, 0.f};
    allSeparated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    allSeparated.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_reject_reason(allSeparated) ==
            fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmptyAfterPrune,
        "prune reject reason flags would-be-empty-after-prune");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_prune_reject_reason_name(
                fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmptyAfterPrune),
            "WouldBeEmptyAfterPrune") == 0,
        "prune reject reason name resolves WouldBeEmptyAfterPrune");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        fuse::physics::narrowphase::should_skip_manifold_prune(clean),
        "should_skip_manifold_prune on clean manifold");
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_prune(clean),
        "can_skip_manifold_prune on clean manifold");

    const auto cleanPreflight = fuse::physics::narrowphase::preflight_manifold_prune(clean);
    expectTrue(cleanPreflight.can_skip_prune(), "prune preflight can skip clean manifold");
    expectTrue(
        cleanPreflight.reason == fuse::physics::narrowphase::ManifoldPruneRejectReason::None,
        "prune preflight reports None for clean manifold");
}

void testManifoldFinalizeRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_reject_reason(empty) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold,
        "finalize reject reason flags empty manifold");
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold),
        "finalize rejects_for_reason matches empty manifold");
    expectTrue(
        fuse::physics::narrowphase::should_skip_manifold_finalize(empty),
        "should_skip_manifold_finalize on empty manifold");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_reject_reason(noNormal) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal,
        "finalize reject reason flags invalid normal");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_reject_reason(ready) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
        "finalize reject reason None for ready manifold");
    const auto readyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(readyPreflight.can_finalize(), "finalize preflight can finalize ready manifold");
    expectTrue(
        readyPreflight.reason == fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
        "finalize preflight reports None for ready manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_manifold_finalize(ready),
        "should_skip_manifold_finalize false for ready manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_finalize_reject_reason_name(
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal),
            "InvalidNormal") == 0,
        "finalize reject reason name resolves InvalidNormal");
}

void testFrictionBasisRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_reject_reason(empty) ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::SkippedManifold,
        "friction reject reason flags skipped empty manifold");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            empty, fuse::physics::narrowphase::FrictionBasisRejectReason::SkippedManifold),
        "friction rejects_for_reason matches skipped manifold");

    fuse::physics::narrowphase::ContactManifold withBasis{};
    withBasis.contactNormal = {0.f, 1.f, 0.f};
    withBasis.addPoint({0.f, 0.f, 0.f}, 0.2f);
    withBasis.buildFrictionBasis();
    expectTrue(
        fuse::physics::narrowphase::friction_basis_reject_reason(withBasis) ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::BasisCurrent,
        "friction reject reason flags current basis");
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_with_preflight(withBasis),
        "rebuild_with_preflight reuses current basis");
    const auto cachedTangent1 = withBasis.frictionBasis.tangent1;
    fuse::physics::narrowphase::compute_friction_tangents_with_preflight(withBasis);
    expectNear(
        withBasis.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "compute_with_preflight preserves current basis");

    withBasis.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_reject_reason(withBasis) ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::None,
        "friction reject reason None when rebuild needed");
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_with_preflight(withBasis),
        "rebuild_with_preflight refreshes stale basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(withBasis),
        "rebuild_with_preflight produces matching basis");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::friction_basis_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRejectReason::BasisCurrent),
            "BasisCurrent") == 0,
        "friction reject reason name resolves BasisCurrent");
}

void testContactPairDeepenRejectsForReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
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
            {dynamicA, sleepingA},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejects_for_reason does not false-positive mixed pair");

    const fuse::u32 planeA = bodies.addBody({0.f, 4.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 planeB = bodies.addBody({0.f, 5.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeA, {0.f, 1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeB, {0.f, 1.f, 0.f});
    expectTrue(
        fuse::physics::narrowphase::is_plane_plane_contact_pair({planeA, planeB}, shapes),
        "plane-plane guard detects both-plane pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {planeA, planeB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedShapePair),
        "plane-plane pair is rejected as unsupported in deepen path");
}

void testZeroFrictionAndBatchPreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 frictionA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 frictionB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    bodies.frictionStatic[frictionA] = 0.5f;
    bodies.frictionDynamic[frictionA] = 0.3f;
    bodies.frictionStatic[frictionB] = 0.4f;
    bodies.frictionDynamic[frictionB] = 0.2f;
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, frictionA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, frictionB, {1.f, 0.f, 0.f});

    const fuse::u32 zeroA = bodies.addBody({2.f, 0.f, 0.f}, 1.f);
    const fuse::u32 zeroB = bodies.addBody({3.f, 0.f, 0.f}, 1.f);
    bodies.frictionStatic[zeroA] = 0.f;
    bodies.frictionDynamic[zeroA] = 0.f;
    bodies.frictionStatic[zeroB] = 0.f;
    bodies.frictionDynamic[zeroB] = 0.f;
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, zeroA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, zeroB, {1.f, 0.f, 0.f});

    expectTrue(
        !fuse::physics::narrowphase::is_zero_friction_contact_pair({frictionA, frictionB}, bodies),
        "zero-friction guard allows mixed-friction pair");
    expectTrue(
        fuse::physics::narrowphase::is_zero_friction_contact_pair({zeroA, zeroB}, bodies),
        "zero-friction guard detects both-zero pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({zeroA, zeroB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::None,
        "zero-friction pair remains dispatchable in deepen path");

    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingB, {1.f, 0.f, 0.f});

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {frictionA, frictionB},
    };
    expectTrue(
        fuse::physics::narrowphase::count_dispatchable_contact_pairs(mixedPairs, bodies, shapes) == 1u,
        "count_dispatchable excludes deepen-rejected pairs");
    expectTrue(
        fuse::physics::narrowphase::should_run_narrowphase(mixedPairs, bodies, shapes),
        "should_run_narrowphase true when dispatchable pair exists");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_narrowphase(mixedPairs, bodies, shapes),
        "can_skip_narrowphase inverse of should_run");

    const auto batchPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_batch(mixedPairs, bodies, shapes);
    expectTrue(batchPreflight.totalPairs == 2u, "batch preflight counts total pairs");
    expectTrue(batchPreflight.rejectedCount == 1u, "batch preflight counts rejected pairs");
    expectTrue(batchPreflight.dispatchableCount == 1u, "batch preflight counts dispatchable pairs");
    expectTrue(batchPreflight.can_dispatch(), "batch preflight can dispatch with mixed list");
    expectTrue(!batchPreflight.can_skip(), "batch preflight cannot skip mixed list");
}

void testManifoldPruneAndFinalizePreflightHelpers() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    clean.addPoint({1.f, 0.f, 0.f}, 0.25f);

    const auto cleanPrunePreflight = fuse::physics::narrowphase::preflight_manifold_prune(clean);
    expectTrue(cleanPrunePreflight.can_skip_prune(), "clean manifold prune preflight can skip");
    expectTrue(
        fuse::physics::narrowphase::should_skip_manifold_prune(clean),
        "should_skip_manifold_prune true for clean manifold");
    expectTrue(
        fuse::physics::narrowphase::prune_manifold_if_needed(clean),
        "prune_manifold_if_needed preserves clean manifold");
    expectTrue(clean.pointCount == 2u, "prune_manifold_if_needed leaves clean slots untouched");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    dirty.addPoint({0.00001f, 0.f, 0.f}, 0.35f);
    expectTrue(!dirty.canSkipPruneContactPoints(), "dirty manifold still needs pruning");
    expectTrue(
        fuse::physics::narrowphase::prune_manifold_if_needed(dirty),
        "prune_manifold_if_needed keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "prune_manifold_if_needed removes separated and duplicate slots");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    const auto readyFinalizePreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(!readyFinalizePreflight.can_skip_finalize(), "ready manifold finalize preflight cannot skip");
    expectTrue(
        fuse::physics::narrowphase::finalize_manifold_if_needed(ready),
        "finalize_manifold_if_needed finalizes ready manifold");
    expectTrue(ready.valid, "finalize_manifold_if_needed sets validity");
    expectTrue(ready.hasFrictionBasis(), "finalize_manifold_if_needed builds friction basis");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.valid = true;
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    const auto separatedFinalizePreflight =
        fuse::physics::narrowphase::preflight_manifold_finalize(separated);
    expectTrue(separatedFinalizePreflight.can_skip_finalize(), "separated manifold finalize preflight can skip");
    expectTrue(
        !fuse::physics::narrowphase::finalize_manifold_if_needed(separated),
        "finalize_manifold_if_needed no-ops on separated manifold");
    expectTrue(separated.valid, "finalize_manifold_if_needed leaves separated validity unchanged");
}

void testFrictionBasisPreflightRebuildHelpers() {
    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);

    const auto needsPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(needsPreflight.needs_work(), "friction preflight needs work without cached basis");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_from_preflight(needsBuild),
        "ensure_from_preflight builds missing basis");
    expectTrue(needsBuild.hasFrictionBasis(), "ensure_from_preflight stores orthonormal basis");

    const auto cachedTangent1 = needsBuild.frictionBasis.tangent1;
    const auto reusePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(!reusePreflight.needs_work(), "friction preflight has no work after build");
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_from_preflight(needsBuild),
        "rebuild_from_preflight reuses valid basis");
    expectNear(
        needsBuild.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "rebuild_from_preflight preserves cached tangent1");

    needsBuild.contactNormal = {1.f, 0.f, 0.f};
    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(stalePreflight.needs_work(), "friction preflight needs work for stale basis");
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_from_preflight(needsBuild),
        "rebuild_from_preflight refreshes stale basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(needsBuild),
        "rebuild_from_preflight matches current normal");
}

void testContactPairDeepenPassGuards() {
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
        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {sleepingA, sleepingB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejects_for_reason matches both-sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {bodyA, bodyB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejects_for_reason does not false-positive valid pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {bodyA, bodyB},
    };
    expectTrue(
        fuse::physics::narrowphase::count_dispatchable_contact_pairs_deepen(mixedPairs, bodies, shapes) == 1u,
        "count_dispatchable counts only deepen-dispatchable pairs");

    const auto listPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_pair_list(mixedPairs, bodies, shapes);
    expectTrue(listPreflight.totalPairs == 2u, "pair-list preflight reports total pairs");
    expectTrue(listPreflight.dispatchableCount == 1u, "pair-list preflight reports dispatchable count");
    expectTrue(listPreflight.rejectedCount == 1u, "pair-list preflight reports rejected count");
    expectTrue(listPreflight.can_dispatch(), "pair-list preflight can dispatch mixed list");
    expectTrue(!listPreflight.can_skip(), "pair-list preflight cannot skip mixed list");

    expectTrue(
        fuse::physics::narrowphase::should_run_narrowphase_dispatch(mixedPairs, bodies, shapes),
        "should_run_narrowphase_dispatch true when dispatchable pair exists");
    expectTrue(
        !fuse::physics::narrowphase::should_run_narrowphase_dispatch(
            {{sleepingA, sleepingB}}, bodies, shapes),
        "should_run_narrowphase_dispatch false when all pairs rejected");
}

void testManifoldPruneDeepenPassGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_reject_reason(empty) ==
            fuse::physics::narrowphase::ManifoldPruneRejectReason::Empty,
        "prune reject reason flags empty manifold");
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldPruneRejectReason::Empty),
        "prune rejects_for_reason matches empty manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_prune_reject_reason_name(
                fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmpty),
            "WouldBeEmpty") == 0,
        "prune reject reason name resolves WouldBeEmpty");
    expectTrue(
        fuse::physics::narrowphase::should_skip_manifold_prune(empty),
        "should_skip_manifold_prune on empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_prune(empty),
        "should_run_manifold_prune false on empty manifold");

    fuse::physics::narrowphase::ContactManifold allSeparated{};
    allSeparated.contactNormal = {0.f, 1.f, 0.f};
    allSeparated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    allSeparated.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_reject_reason(allSeparated) ==
            fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmpty,
        "prune reject reason flags would-be-empty manifold");
    expectTrue(
        fuse::physics::narrowphase::should_skip_manifold_prune(allSeparated),
        "should_skip_manifold_prune when prune would leave no points");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_prune(dirty),
        "should_run_manifold_prune when separated slots exist");
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(dirty),
        "prune_contact_manifold_if_needed keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "prune_contact_manifold_if_needed removes separated slots");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_finalize(ready),
        "should_run_manifold_finalize true for ready manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_finalize(empty),
        "should_run_manifold_finalize false for empty manifold");

    const auto dirtyPreflight = fuse::physics::narrowphase::preflight_manifold_prune(dirty);
    expectTrue(dirtyPreflight.can_skip_prune(), "can_skip_prune true after prune_if_needed");
}

void testFrictionBasisDeepenPassGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::should_rebuild_friction_basis(empty),
        "should_rebuild_friction_basis false for empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::rebuild_friction_basis_from_preflight(empty),
        "rebuild_from_preflight false for empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::should_rebuild_friction_basis(needsBuild),
        "should_rebuild_friction_basis true without cached basis");
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_from_preflight(needsBuild),
        "rebuild_from_preflight builds missing basis");
    expectTrue(needsBuild.hasFrictionBasis(), "rebuild_from_preflight stores orthonormal basis");

    const auto cachedTangent1 = needsBuild.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_from_preflight(needsBuild),
        "rebuild_from_preflight reuses valid cached basis");
    expectNear(
        needsBuild.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "rebuild_from_preflight preserves cached tangent1");
    expectTrue(
        !fuse::physics::narrowphase::should_rebuild_friction_basis(needsBuild),
        "should_rebuild_friction_basis false after valid rebuild");

    needsBuild.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::should_rebuild_friction_basis(needsBuild),
        "should_rebuild_friction_basis true for stale basis");
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_from_preflight(needsBuild),
        "rebuild_from_preflight refreshes stale basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(needsBuild),
        "rebuild_from_preflight produces basis matching current normal");
}

void testContactPairDeepen2RejectGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 zeroMassA = bodies.addBody({0.f, 2.f, 0.f}, 0.f);
    const fuse::u32 zeroMassB = bodies.addBody({0.f, 3.f, 0.f}, 0.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, zeroMassA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, zeroMassB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::is_zero_inv_mass_contact_pair({zeroMassA, zeroMassB}, bodies),
        "zero-inv-mass guard detects both zero-mass pair");
    expectTrue(
        !fuse::physics::narrowphase::is_zero_inv_mass_contact_pair({dynamicA, zeroMassA}, bodies),
        "zero-inv-mass guard allows mixed mass pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen2_reject_reason({zeroMassA, zeroMassB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothZeroInvMass,
        "deepen2 reject reason flags both zero-inv-mass pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({zeroMassA, zeroMassB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::None,
        "first deepen reject reason unchanged for zero-inv-mass pair");

    const fuse::u32 capsuleA = bodies.addBody({4.f, 0.f, 0.f}, 1.f);
    const fuse::u32 capsuleB = bodies.addBody({5.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Capsule, capsuleA, {0.5f, 1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Capsule, capsuleB, {0.5f, 1.f, 0.f});
    expectTrue(
        fuse::physics::narrowphase::is_undispatched_shape_pair({capsuleA, capsuleB}, shapes),
        "undispatched guard flags capsule-capsule pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen2_reject_reason({capsuleA, capsuleB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::NoColliderDispatch,
        "deepen2 reject reason flags undispatched capsule-capsule pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({capsuleA, capsuleB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::None,
        "first deepen reject reason unchanged for undispatched pair");

    const auto zeroMassPreflight =
        fuse::physics::narrowphase::preflight_contact_pair_deepen2({zeroMassA, zeroMassB}, bodies, shapes);
    expectTrue(!zeroMassPreflight.can_dispatch(), "deepen2 preflight rejects both zero-inv-mass pair");
    expectTrue(
        zeroMassPreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::BothZeroInvMass,
        "deepen2 preflight reports BothZeroInvMass");
    expectTrue(
        fuse::physics::narrowphase::should_skip_contact_pair_deepen2_dispatch({zeroMassA, zeroMassB}, bodies, shapes),
        "deepen2 skip guard rejects both zero-inv-mass pair");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_contact_pair_deepen2_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "deepen2 skip guard allows valid pair");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::BothZeroInvMass),
            "BothZeroInvMass") == 0,
        "reject reason name resolves BothZeroInvMass");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::NoColliderDispatch),
            "NoColliderDispatch") == 0,
        "reject reason name resolves NoColliderDispatch");
}

void testCanSkipNarrowphaseDeepen2Guards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 zeroMassA = bodies.addBody({0.f, 2.f, 0.f}, 0.f);
    const fuse::u32 zeroMassB = bodies.addBody({0.f, 3.f, 0.f}, 0.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, zeroMassA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, zeroMassB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::can_skip_narrowphase_deepen2({}, bodies, shapes),
        "can_skip_narrowphase_deepen2 on empty pair list");
    expectTrue(
        fuse::physics::narrowphase::can_skip_narrowphase_deepen2({{zeroMassA, zeroMassB}}, bodies, shapes),
        "can_skip_narrowphase_deepen2 when all pairs are deepen2-rejected");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_narrowphase_deepen2({{bodyA, bodyB}}, bodies, shapes),
        "can_skip_narrowphase_deepen2 false when dispatchable pair exists");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_narrowphase_deepen2(
            {{zeroMassA, zeroMassB}, {bodyA, bodyB}}, bodies, shapes),
        "can_skip_narrowphase_deepen2 false when mixed rejected and dispatchable pairs");
}

void testManifoldNormalizeAndFinalizeDeepenGuards() {
    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.25f);
    expectTrue(unnormalized.hasUnnormalizedNormal(), "hasUnnormalizedNormal flags non-unit normal");
    expectTrue(!unnormalized.canSkipNormalizeContactNormal(), "canSkipNormalize false for non-unit normal");
    unnormalized.normalizeContactNormalIfNeeded();
    expectNear(unnormalized.contactNormal.length(), 1.f, 1e-4f, "normalizeContactNormalIfNeeded unitizes normal");
    expectTrue(unnormalized.canSkipNormalizeContactNormal(), "canSkipNormalize true after normalization");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    const auto readyDeepenPreflight = fuse::physics::narrowphase::preflight_manifold_finalize_deepen(ready);
    expectTrue(readyDeepenPreflight.can_finalize(), "deepen finalize preflight can finalize ready manifold");
    expectTrue(!readyDeepenPreflight.needsNormalNormalization, "deepen finalize preflight no normal fixup needed");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_manifold_finalize_deepen(ready),
        "can_skip_manifold_finalize_deepen false for ready manifold");

    fuse::physics::narrowphase::ContactManifold manifold =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    manifold.contactNormal = manifold.contactNormal * 2.f;
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_deepen_if_needed(manifold),
        "generate_deepen_if_needed finalizes manifold with unnormalized normal");
    expectTrue(manifold.valid, "generate_deepen_if_needed sets validity on success");
    expectNear(manifold.contactNormal.length(), 1.f, 1e-4f, "generate_deepen_if_needed normalizes contact normal");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_prune(clean),
        "can_skip_manifold_prune true for clean manifold");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_manifold_prune(
            fuse::physics::narrowphase::ContactManifold{}),
        "can_skip_manifold_prune false for empty manifold");
}

void testFrictionBasisDeepenPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold partial{};
    partial.contactNormal = {0.f, 1.f, 0.f};
    partial.addPoint({0.f, 0.f, 0.f}, 0.2f);
    partial.frictionBasis.tangent1 = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::has_partial_friction_basis(partial),
        "partial guard flags single-axis basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_needs_completion(partial),
        "needs_completion true for partial basis");

    const auto partialPreflight =
        fuse::physics::narrowphase::preflight_friction_basis_rebuild_deepen(partial);
    expectTrue(partialPreflight.partial, "deepen friction preflight flags partial basis");
    expectTrue(partialPreflight.needsRebuild, "deepen friction preflight needs rebuild for partial basis");
    expectTrue(!partialPreflight.can_skip_rebuild(), "deepen friction preflight cannot skip partial basis");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_friction_basis_deepen_preflight(partial),
        "should_skip_deepen_preflight false for partial basis");

    fuse::physics::narrowphase::compute_friction_tangents_deepen_if_needed(partial);
    expectTrue(partial.hasFrictionBasis(), "compute_deepen_if_needed completes partial basis");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_deepen_preflight(partial),
        "should_skip_deepen_preflight true after completion");

    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {2.f, 0.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto unnormalizedPreflight =
        fuse::physics::narrowphase::preflight_friction_basis_rebuild_deepen(unnormalized);
    expectTrue(
        unnormalizedPreflight.needsNormalNormalization,
        "deepen friction preflight flags unnormalized normal");
    fuse::physics::narrowphase::compute_friction_tangents_deepen_if_needed(unnormalized);
    expectNear(unnormalized.contactNormal.length(), 1.f, 1e-4f, "compute_deepen_if_needed normalizes contact normal");
    expectTrue(unnormalized.hasFrictionBasis(), "compute_deepen_if_needed builds basis after normalization");
}

void testContactPairBatchPreflightGuards() {
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

    const auto emptyBatch =
        fuse::physics::narrowphase::preflight_contact_pair_batch({}, bodies, shapes);
    expectTrue(emptyBatch.skipped, "batch preflight skips empty pair list");
    expectTrue(!emptyBatch.can_dispatch(), "batch preflight cannot dispatch empty list");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {bodyA, bodyB},
    };
    const auto mixedBatch =
        fuse::physics::narrowphase::preflight_contact_pair_batch(mixedPairs, bodies, shapes);
    expectTrue(!mixedBatch.skipped, "batch preflight does not skip mixed list");
    expectTrue(mixedBatch.totalPairs == 2u, "batch preflight counts total pairs");
    expectTrue(mixedBatch.rejectedCount == 1u, "batch preflight counts rejected pairs");
    expectTrue(mixedBatch.dispatchableCount == 1u, "batch preflight counts dispatchable pairs");
    expectTrue(mixedBatch.can_dispatch(), "batch preflight can dispatch mixed list");
    expectTrue(
        fuse::physics::narrowphase::has_dispatchable_contact_pairs(mixedPairs, bodies, shapes),
        "has_dispatchable true for mixed list");

    const std::vector<fuse::physics::broadphase::CandidatePair> rejectedPairs = {{sleepingA, sleepingB}};
    expectTrue(
        !fuse::physics::narrowphase::has_dispatchable_contact_pairs(rejectedPairs, bodies, shapes),
        "has_dispatchable false when all pairs rejected");
}

void testContactPairPlanePlaneAndDeepenDispatchGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 planeA = bodies.addBody({0.f, 0.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 planeB = bodies.addBody({0.f, 1.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 dynamicA = bodies.addBody({0.f, 2.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeA, {0.f, 1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeB, {0.f, -1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::is_plane_plane_pair({planeA, planeB}, shapes),
        "plane-plane guard detects plane pair");
    expectTrue(
        !fuse::physics::narrowphase::is_plane_plane_pair({dynamicA, planeA}, shapes),
        "plane-plane guard false for sphere-plane pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({planeA, planeB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedShapePair,
        "base reject reason unchanged for plane-plane pair");

    const fuse::u32 sleepingA = bodies.addBody({0.f, 4.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 5.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingB, {1.f, 0.f, 0.f});

    const auto sleepingDeepen =
        fuse::physics::narrowphase::detect_contacts_pair_deepen({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingDeepen.valid, "deepen dispatch rejects both-sleeping pair");
    expectTrue(sleepingDeepen.empty(), "deepen dispatch returns empty manifold for rejected pair");

    const fuse::u32 dynamicB = bodies.addBody({1.5f, 2.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});
    const auto overlapDeepen =
        fuse::physics::narrowphase::detect_contacts_pair_deepen({dynamicA, dynamicB}, bodies, shapes);
    expectTrue(overlapDeepen.valid, "deepen dispatch allows valid pair");
    expectTrue(overlapDeepen.pointCount > 0u, "deepen dispatch populates contact points");
}

void testManifoldFinalizeDeepenGuards() {
    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.25f);
    expectTrue(!unnormalized.hasUnitNormal(), "hasUnitNormal false for non-unit normal");
    expectTrue(unnormalized.hasValidNormal(), "hasValidNormal true for non-unit normal");

    const auto unnormalizedPreflight =
        fuse::physics::narrowphase::preflight_manifold_finalize(unnormalized);
    expectTrue(unnormalizedPreflight.needsNormalNormalization, "finalize preflight flags non-unit normal");
    expectTrue(
        fuse::physics::narrowphase::finalize_contact_manifold_if_needed(unnormalized),
        "finalize_if_needed accepts manifold with non-unit normal");
    expectTrue(unnormalized.hasUnitNormal(), "finalize_if_needed normalizes contact normal");
    expectTrue(unnormalized.hasFrictionBasis(), "finalize_if_needed builds friction basis");

    fuse::physics::narrowphase::ContactManifold shallow{};
    shallow.contactNormal = {0.f, 1.f, 0.f};
    shallow.addPoint({0.f, 0.f, 0.f}, 0.5f);
    shallow.addPoint({1.f, 0.f, 0.f}, 0.01f);
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_deepen(shallow, 0.05f),
        "generate_deepen finalizes after shallow prune");
    expectTrue(shallow.pointCount == 1u, "generate_deepen removes shallow slot");
    expectTrue(shallow.valid, "generate_deepen sets validity on success");
    expectTrue(shallow.hasFrictionBasis(), "generate_deepen builds friction basis");
}

void testContactBufferPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    const auto validWrite = fuse::physics::narrowphase::preflight_contact_buffer_write(valid);
    expectTrue(validWrite.canWrite(), "write preflight allows valid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    const auto selfWrite = fuse::physics::narrowphase::preflight_contact_buffer_write(selfPair);
    expectTrue(!selfWrite.canWrite(), "write preflight rejects self pair");
    expectTrue(selfWrite.selfPair, "write preflight flags self pair");

    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, valid);
    expectTrue(buffer.compact() == 1u, "buffer compacts one valid slot");

    const auto compactionPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(!compactionPreflight.needsCompaction(), "compaction preflight false after compact");

    buffer.setMaxCapacity(1u);
    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold shallow = valid;
    shallow.penetrationDepth = 0.1f;
    shallow.points[0].penetration = 0.1f;
    fuse::physics::narrowphase::ContactManifold deep = valid;
    deep.bodyB = 2u;
    deep.penetrationDepth = 0.9f;
    deep.points[0].penetration = 0.9f;
    buffer.writeSlot(0u, shallow);
    buffer.writeSlot(1u, deep);
    buffer.compact();
    const auto clampPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(buffer);
    expectTrue(clampPreflight.needsClamp(), "clamp preflight true when over capacity");
    expectTrue(buffer.canApplyMaxCapacityClamp(), "canApplyMaxCapacityClamp true when over capacity");
    expectTrue(!buffer.canSkipMaxCapacityClamp(), "canSkipMaxCapacityClamp false when over capacity");
}

void testContactBufferFrictionRebuildPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold first{};
    first.valid = true;
    first.bodyA = 0u;
    first.bodyB = 1u;
    first.contactNormal = {0.f, 0.f, 1.f};
    first.addPoint({0.f, 0.f, 0.f}, 0.1f);
    first.buildFrictionBasis();
    buffer.writeSlot(0u, first);

    fuse::physics::narrowphase::ContactManifold second{};
    second.valid = true;
    second.bodyA = 2u;
    second.bodyB = 3u;
    second.contactNormal = {1.f, 0.f, 0.f};
    second.addPoint({0.f, 0.f, 0.f}, 0.2f);
    buffer.writeSlot(1u, second);
    expectTrue(buffer.compact() == 2u, "friction preflight buffer compacts two slots");
    buffer.contactNormals[1u] = {0.f, 1.f, 0.f};

    const auto initialPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_friction_rebuild(buffer);
    expectTrue(!initialPreflight.can_skip_rebuild(), "friction preflight needs rebuild for stale basis");
    expectTrue(initialPreflight.staleSlotCount == 1u, "friction preflight counts one stale slot");
    expectTrue(initialPreflight.rebuildSlotCount == 1u, "friction preflight counts one rebuild slot");

    buffer.buildFrictionTangentBasesIfNeeded();
    const auto rebuiltPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_friction_rebuild(buffer);
    expectTrue(rebuiltPreflight.can_skip_rebuild(), "friction preflight can skip after rebuild");
    expectTrue(
        fuse::physics::narrowphase::can_skip_build_friction_tangent_bases(buffer),
        "can_skip_build_friction_tangent_bases after rebuild");

    const auto firstBasis = buffer.tangentBasisAt(0u);
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis({0.f, 0.f, 1.f}, firstBasis),
        "if-needed rebuild preserves orthonormal slot zero basis");

    buffer.buildFrictionTangentBasesIfNeeded();
    const auto cachedBasis = buffer.tangentBasisAt(0u);
    expectNear(cachedBasis.tangent1.x, firstBasis.tangent1.x, 1e-4f,
        "if-needed rebuild no-ops on valid cached basis");
}

void testWarmStartFrictionPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::should_skip_warm_start_friction(empty),
        "should_skip_warm_start on empty manifold");

    fuse::physics::narrowphase::ContactManifold noImpulse{};
    noImpulse.contactNormal = {0.f, 1.f, 0.f};
    noImpulse.addPoint({0.f, 0.f, 0.f}, 0.2f);
    noImpulse.buildFrictionBasis();
    const auto noImpulsePreflight =
        fuse::physics::narrowphase::preflight_warm_start_friction(noImpulse);
    expectTrue(!noImpulsePreflight.can_warm_start(), "warm-start preflight false without impulses");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.warmNormalImpulse = 2.5f;
    ready.warmTangentImpulse = {0.1f, -0.05f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.2f);
    ready.buildFrictionBasis();
    const auto readyPreflight = fuse::physics::narrowphase::preflight_warm_start_friction(ready);
    expectTrue(readyPreflight.hasWarmImpulse, "warm-start preflight detects warm impulses");
    expectTrue(readyPreflight.hasValidBasis, "warm-start preflight accepts valid basis");
    expectTrue(readyPreflight.can_warm_start(), "warm-start preflight can warm-start ready manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_warm_start_friction(ready),
        "should_skip false for warm-startable manifold");

    ready.contactNormal = {1.f, 0.f, 0.f};
    const auto stalePreflight = fuse::physics::narrowphase::preflight_warm_start_friction(ready);
    expectTrue(!stalePreflight.hasValidBasis, "warm-start preflight rejects stale basis");
    expectTrue(
        fuse::physics::narrowphase::should_skip_warm_start_friction(ready),
        "should_skip true when basis is stale");
}

void testContactPairDeepenRejectsForReasonGuards() {
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
        "deepenRejectsForReason matches both-sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {dynamicA, dynamicB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepenRejectsForReason does not false-positive valid pair");

    const auto sleepingPair =
        fuse::physics::narrowphase::detect_contacts_pair_deepen({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingPair.valid, "detect_contacts_pair_deepen rejects sleeping pair");
    const auto validPair =
        fuse::physics::narrowphase::detect_contacts_pair_deepen({dynamicA, dynamicB}, bodies, shapes);
    expectTrue(validPair.valid, "detect_contacts_pair_deepen allows valid pair");

    const auto ifValidPair =
        fuse::physics::narrowphase::detect_contacts_pair_if_valid({dynamicA, dynamicB}, bodies, shapes);
    expectTrue(ifValidPair.valid, "detect_contacts_pair_if_valid allows valid pair");
    const auto ifValidSelf =
        fuse::physics::narrowphase::detect_contacts_pair_if_valid({dynamicA, dynamicA}, bodies, shapes);
    expectTrue(!ifValidSelf.valid, "detect_contacts_pair_if_valid rejects self pair");
}

void testNarrowphaseBatchPreflightGuards() {
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

    const auto emptyBatch = fuse::physics::narrowphase::preflight_narrowphase_batch({}, bodies, shapes);
    expectTrue(emptyBatch.skipped, "batch preflight skips empty pair list");
    expectTrue(!emptyBatch.can_run(), "batch preflight cannot run on empty list");

    const std::vector<fuse::physics::broadphase::CandidatePair> allRejected = {{sleepingA, sleepingB}};
    const auto rejectedBatch =
        fuse::physics::narrowphase::preflight_narrowphase_batch(allRejected, bodies, shapes);
    expectTrue(rejectedBatch.rejectedCount == 1u, "batch preflight counts rejected pairs");
    expectTrue(rejectedBatch.dispatchableCount == 0u, "batch preflight reports zero dispatchable");
    expectTrue(!rejectedBatch.can_run(), "batch preflight cannot run when all rejected");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixed = {
        {sleepingA, sleepingB},
        {bodyA, bodyB},
    };
    const auto mixedBatch = fuse::physics::narrowphase::preflight_narrowphase_batch(mixed, bodies, shapes);
    expectTrue(mixedBatch.pairCount == 2u, "batch preflight reports pair count");
    expectTrue(mixedBatch.rejectedCount == 1u, "batch preflight counts one rejected in mixed list");
    expectTrue(mixedBatch.dispatchableCount == 1u, "batch preflight counts one dispatchable in mixed list");
    expectTrue(mixedBatch.can_run(), "batch preflight can run with dispatchable pair");
    expectTrue(
        fuse::physics::narrowphase::can_run_narrowphase(mixed, bodies, shapes),
        "can_run_narrowphase true when dispatchable pair exists");
    expectTrue(
        !fuse::physics::narrowphase::can_run_narrowphase(allRejected, bodies, shapes),
        "can_run_narrowphase false when all rejected");
}

void testManifoldPruneRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold),
        "manifoldPruneRejectsForReason matches empty manifold");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    separated.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            separated, fuse::physics::narrowphase::ManifoldPruneRejectReason::AllSeparated),
        "manifoldPruneRejectsForReason matches all-separated manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_prune_reject_reason_name(
                fuse::physics::narrowphase::ManifoldPruneRejectReason::AllSeparated),
            "AllSeparated") == 0,
        "manifold prune reject reason name resolves AllSeparated");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::prune_manifold_if_needed(dirty),
        "prune_manifold_if_needed keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "prune_manifold_if_needed removes separated slots");

    fuse::physics::narrowphase::ContactManifold allSeparatedPrune = separated;
    expectTrue(
        !fuse::physics::narrowphase::prune_manifold_if_needed(allSeparatedPrune),
        "prune_manifold_if_needed returns false for all-separated manifold");
    expectTrue(allSeparatedPrune.empty(), "prune_manifold_if_needed clears all-separated manifold");
}

void testManifoldFinalizeRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold),
        "manifoldFinalizeRejectsForReason matches empty manifold");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            noNormal, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal),
        "manifoldFinalizeRejectsForReason matches invalid normal");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            separated,
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune),
        "manifoldFinalizeRejectsForReason matches would-be-empty-after-prune");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    const auto readyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(
        readyPreflight.reason == fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
        "finalize preflight reports None for ready manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_finalize_reject_reason_name(
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::NoPenetratingPoints),
            "NoPenetratingPoints") == 0,
        "manifold finalize reject reason name resolves NoPenetratingPoints");

    fuse::physics::narrowphase::ContactManifold manifold =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::finalize_contact_manifold_if_needed(manifold),
        "finalize_contact_manifold_if_needed finalizes valid manifold");
    expectTrue(manifold.valid, "finalize_contact_manifold_if_needed sets validity");
    expectTrue(manifold.hasFrictionBasis(), "finalize_contact_manifold_if_needed builds friction basis");

    fuse::physics::narrowphase::ContactManifold skipFinalize = separated;
    skipFinalize.valid = true;
    expectTrue(
        !fuse::physics::narrowphase::finalize_contact_manifold_if_needed(skipFinalize),
        "finalize_contact_manifold_if_needed no-ops on non-finalizable manifold");
    expectTrue(skipFinalize.valid, "finalize_contact_manifold_if_needed leaves validity unchanged on skip");
}

void testFrictionBasisRejectReasonGuards() {
void testContactPairDeepenDispatchGuards() {
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

    const auto sleepingManifold = fuse::physics::narrowphase::detect_contacts_pair_deepen(
        {sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingManifold.valid, "deepen dispatch rejects both-sleeping pair");
    expectTrue(sleepingManifold.empty(), "deepen dispatch returns empty manifold for rejected pair");

    const auto validManifold = fuse::physics::narrowphase::detect_contacts_pair_deepen(
        {dynamicA, dynamicB}, bodies, shapes);
    expectTrue(validManifold.valid, "deepen dispatch detects valid overlapping pair");
    expectTrue(validManifold.pointCount > 0u, "deepen dispatch populates contact points");

    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_preflight_rejects_for_reason(
            {sleepingA, sleepingB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen preflight rejects_for_reason matches both-sleeping pair");
        !fuse::physics::narrowphase::contact_pair_deepen_preflight_rejects_for_reason(
            {dynamicA, dynamicB},
        "deepen preflight rejects_for_reason does not false-positive valid pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
    };
        fuse::physics::narrowphase::first_dispatchable_contact_pair_index(mixedPairs, bodies, shapes) == 1u,
        "first_dispatchable returns index of valid pair");
        fuse::physics::narrowphase::first_dispatchable_contact_pair_index(
            {{sleepingA, sleepingB}}, bodies, shapes) == 1u,
        "first_dispatchable returns pair count when none dispatchable");
}

void testManifoldPruneFinalizeIfNeededGuards() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
        fuse::physics::narrowphase::manifold_prune_preflight_skips(clean),
        "prune preflight skips clean manifold");
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(clean),
        "prune if needed preserves clean manifold");
    expectTrue(clean.pointCount == 1u, "prune if needed leaves clean slots untouched");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
        !fuse::physics::narrowphase::manifold_prune_preflight_skips(dirty),
        "prune preflight does not skip dirty manifold");
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(dirty),
        "prune if needed keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "prune if needed removes separated slot");

    fuse::physics::narrowphase::ContactManifold ready =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
        !fuse::physics::narrowphase::manifold_finalize_preflight_skips(ready),
        "finalize preflight does not skip ready manifold");
        fuse::physics::narrowphase::finalize_contact_manifold_if_needed(ready),
        "finalize if needed finalizes valid manifold");
    expectTrue(ready.valid, "finalize if needed sets validity");
    expectTrue(ready.hasFrictionBasis(), "finalize if needed builds friction basis");

    fuse::physics::narrowphase::ContactManifold empty{};
        fuse::physics::narrowphase::manifold_finalize_preflight_skips(empty),
        "finalize preflight skips empty manifold");
        !fuse::physics::narrowphase::finalize_contact_manifold_if_needed(empty),
        "finalize if needed no-ops on empty manifold");

void testFrictionBasisDeepenPassPreflightWrappers() {
        fuse::physics::narrowphase::friction_basis_preflight_skips(empty),
        "friction preflight skips empty manifold");

    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.2f);
        fuse::physics::narrowphase::normalize_contact_normal_if_needed(unnormalized),
        "normalize if needed scales non-unit normal");
    expectNear(unnormalized.contactNormal.y, 1.f, 1e-4f, "normalize if needed produces unit normal");

    fuse::physics::narrowphase::ContactManifold unit{};
    unit.contactNormal = {0.f, 1.f, 0.f};
    unit.addPoint({0.f, 0.f, 0.f}, 0.2f);
        !fuse::physics::narrowphase::normalize_contact_normal_if_needed(unit),
        "normalize if needed no-ops on unit normal");

        fuse::physics::narrowphase::compute_friction_tangents_with_preflight(unit),
        "compute with preflight builds missing basis");
    expectTrue(unit.hasFrictionBasis(), "compute with preflight stores orthonormal basis");

    const auto cachedTangent1 = unit.frictionBasis.tangent1;
        fuse::physics::narrowphase::friction_basis_preflight_skips(unit),
        "friction preflight skips valid basis");
        "compute with preflight reuses valid basis");
    expectNear(
        unit.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "compute with preflight preserves cached tangent1");

void testContactBufferPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(buffer.canSkipSoAIteration(), "empty contact buffer skips SoA iteration");
    expectTrue(buffer.canSkipCompaction(), "empty contact buffer skips compaction");
    expectTrue(buffer.countValidSlots() == 0u, "countValidSlots early-outs when empty");

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.penetrationDepth = 0.5f;
    valid.addPoint({0.f, 0.f, 0.f}, 0.5f);

    const auto invalidSlotPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 0u, valid);
        invalidSlotPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidSlot,
        "write preflight rejects out-of-range slot");

    buffer.preparePairSlots(2u);
    const auto validWritePreflight =
    expectTrue(validWritePreflight.canWrite(), "write preflight accepts valid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
        fuse::physics::narrowphase::contact_buffer_write_rejects_for_reason(
            buffer,
            1u,
            selfPair,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write rejects_for_reason flags self pair");

        fuse::physics::narrowphase::write_contact_buffer_slot_with_preflight(buffer, 0u, valid),
        "write_with_preflight stores valid contact");
    expectTrue(buffer.slotIsValid(0u), "write_with_preflight marks slot valid");

    fuse::physics::narrowphase::ContactManifold invalid{};
    invalid.bodyA = 0u;
    invalid.bodyB = 2u;
        !fuse::physics::narrowphase::write_contact_buffer_slot_with_preflight(buffer, 1u, invalid),
        "write_with_preflight skips invalid manifold");

    fuse::physics::narrowphase::ContactBufferSoA packed;
    packed.preparePairSlots(1u);
    fuse::physics::narrowphase::write_contact_buffer_slot_with_preflight(packed, 0u, valid);
    const auto compactionPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compaction(packed);
    expectTrue(compactionPreflight.allValid, "compaction preflight marks all-valid slots");
        !fuse::physics::narrowphase::should_run_contact_buffer_compaction(packed),
        "compaction preflight skips when all slots valid");

void testContactBufferCompactionClampPreflights() {
    buffer.preparePairSlots(3u);
void testContactBufferWritePreflightGuards() {
void testContactBufferWriteRejectReasonGuards() {

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;

        fuse::physics::narrowphase::contact_buffer_write_rejects_for_reason(
    valid.penetrationDepth = 0.25f;
    valid.addPoint({0.f, 0.f, 0.f}, 0.25f);

    const auto validPreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, valid);
    expectTrue(validPreflight.canWrite(), "write preflight accepts valid manifold");
    expectTrue(
        validPreflight.reason == fuse::physics::narrowphase::ContactBufferWriteRejectReason::None,
        "write preflight reports None for valid manifold");

    fuse::physics::narrowphase::ContactManifold invalid = valid;
    invalid.valid = false;
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer,
            0u,
            invalid,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "write rejects_for_reason flags invalid manifold");
        fuse::physics::narrowphase::shouldSkipContactBufferWrite(buffer, 0u, invalid),
        "shouldSkipContactBufferWrite on invalid manifold");

            selfPair,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write rejects_for_reason flags self pair");
            99u,
            valid,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidSlot),
        "write rejects_for_reason flags invalid slot");

    const auto invalidPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 0u, selfPair);
    expectTrue(!invalidPreflight.canWrite(), "write preflight rejects self pair");
    expectTrue(!buffer.writeSlotWithPreflight(0u, selfPair), "write with preflight skips self pair");

    expectTrue(buffer.writeSlotWithPreflight(0u, valid), "write with preflight accepts valid manifold");
    expectTrue(buffer.slotIsValid(0u), "write with preflight marks slot valid");

    const auto writePreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 0u, valid);
    expectTrue(writePreflight.can_write(), "contact-buffer write preflight allows valid manifold");
        !fuse::physics::narrowphase::should_skip_contact_buffer_write(buffer, 0u, valid),
        "contact-buffer write skip guard allows valid manifold");

            buffer, 1u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "contact-buffer write rejects self pair");
        std::strcmp(
            fuse::physics::narrowphase::contact_buffer_write_reject_reason_name(
            "InvalidManifold") == 0,
        "write reject reason name resolves InvalidManifold");

    fuse::physics::narrowphase::ContactManifold second = valid;
    second.bodyB = 2u;
    fuse::physics::narrowphase::ContactManifold third = valid;
    third.bodyB = 3u;
    buffer.writeSlot(1u, second);
    buffer.writeSlot(2u, third);
        fuse::physics::narrowphase::contact_buffer_compaction_rejects_for_reason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "compaction rejects_for_reason flags all-valid slots");
        fuse::physics::narrowphase::can_skip_contact_buffer_compaction(buffer),
        "can_skip compaction true when all slots valid");

    buffer.preparePairSlots(2u);
    valid.bodyB = 2u;
    buffer.writeSlot(1u, valid);
        !fuse::physics::narrowphase::can_skip_contact_buffer_compaction(buffer),
        "can_skip compaction false with sparse valid slots");
    expectTrue(buffer.compact() == 1u, "compact removes invalid slot");

    buffer.setMaxCapacity(1u);
    valid.bodyB = 3u;
    buffer.writeSlot(0u, valid);
    valid.bodyB = 4u;
    valid.penetrationDepth = 0.5f;
    valid.points[0].penetration = 0.5f;
    buffer.compact();
        !fuse::physics::narrowphase::can_skip_contact_buffer_clamp(buffer),
        "can_skip clamp false when over capacity");
    expectTrue(buffer.compactAndClamp() == 1u, "compactAndClamp respects max capacity");
        fuse::physics::narrowphase::contact_buffer_clamp_rejects_for_reason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
        "clamp rejects_for_reason flags within-capacity buffer");

void testRunNarrowphaseDeepenPreflightDispatch() {

    const std::vector<fuse::physics::broadphase::CandidatePair> allRejected = {{sleepingA, sleepingB}};
        !fuse::physics::narrowphase::runNarrowphaseIntoBufferIfNeeded(allRejected, bodies, shapes, buffer),
        "run if needed false when all pairs deepen-rejected");
    expectTrue(buffer.isEmpty(), "run if needed clears buffer when skipping dispatch");

    fuse::physics::narrowphase::runNarrowphaseIntoBufferWithDeepenPreflight(mixedPairs, bodies, shapes, buffer);
    expectTrue(buffer.activeCount == 1u, "deepen dispatch produces one contact from mixed pairs");
    const auto restored = buffer.manifoldAt(0u);
    expectTrue(restored.valid, "deepen dispatch finalizes valid manifold");
    expectTrue(restored.hasFrictionBasis(), "deepen dispatch stores friction basis");

        fuse::physics::narrowphase::runNarrowphaseIntoBufferIfNeeded(mixedPairs, bodies, shapes, buffer),
        "run if needed true when dispatchable pair exists");
    expectTrue(buffer.hasValidContacts(), "run if needed leaves valid contacts in buffer");
    valid.addPoint({0.f, 0.f, 0.f}, 0.5f);

    fuse::physics::narrowphase::write_contact_buffer_slot_with_preflight(buffer, 0u, valid);
    fuse::physics::narrowphase::write_contact_buffer_slot_with_preflight(buffer, 2u, valid);

        "compaction preflight requests work with invalid middle slot");
        fuse::physics::narrowphase::compact_contact_buffer_with_preflight(buffer) == 2u,
        "compact_with_preflight keeps valid slots");
    expectTrue(buffer.activeCount == 2u, "compact_with_preflight updates active count");

    const auto clampPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(buffer);
    expectTrue(clampPreflight.needsClamp(), "clamp preflight requests overflow truncation");
        fuse::physics::narrowphase::compact_and_clamp_contact_buffer_with_preflight(buffer) == 1u,
        "compact_and_clamp_with_preflight enforces max capacity");
    expectTrue(buffer.droppedCount == 1u, "compact_and_clamp_with_preflight tracks dropped contacts");

    fuse::physics::narrowphase::ContactBufferSoA packed;
    packed.preparePairSlots(1u);
    fuse::physics::narrowphase::write_contact_buffer_slot_with_preflight(packed, 0u, valid);
    packed.activeCount = 1u;
    const auto noWorkPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compact_and_clamp(packed);
        noWorkPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork,
        "compact_and_clamp preflight reports NoWork for packed buffer");
            fuse::physics::narrowphase::contact_buffer_compact_and_clamp_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork),
            "NoWork") == 0,
        "compact_and_clamp reject reason name resolves NoWork");
}

void testNarrowphaseRunPreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});

    const std::vector<fuse::physics::broadphase::CandidatePair> emptyPairs;
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_run_narrowphase(emptyPairs, bodies, shapes);
    expectTrue(emptyPreflight.emptyInput, "run preflight marks empty input");
    expectTrue(emptyPreflight.canSkip, "run preflight can skip empty input");
        fuse::physics::narrowphase::can_skip_narrowphase_run(emptyPairs, bodies, shapes),
        "can_skip_narrowphase_run on empty pairs");
        fuse::physics::narrowphase::canSkipNarrowphaseRun(emptyPairs, bodies, shapes),
        "canSkipNarrowphaseRun dispatch wrapper on empty pairs");

    const std::vector<fuse::physics::broadphase::CandidatePair> validPairs = {{dynamicA, dynamicB}};
    const auto validPreflight =
        fuse::physics::narrowphase::preflightNarrowphaseRun(validPairs, bodies, shapes);
    expectTrue(validPreflight.can_run(), "run preflight can run with valid pairs");
    expectTrue(validPreflight.batch.dispatchableCount == 1u, "run preflight carries batch dispatchable count");

    fuse::physics::narrowphase::ContactBufferSoA buffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(emptyPairs, bodies, shapes, buffer);
    expectTrue(buffer.isEmpty(), "runNarrowphaseIntoBuffer early-outs on empty input");

    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingB, {1.f, 0.f, 0.f});

    const auto deepenRejected =
        fuse::physics::narrowphase::detect_contacts_pair_with_deepen_preflight(
            {sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!deepenRejected.valid, "deepen preflight dispatch rejects both-sleeping pair");

    const auto baseAccepted = fuse::physics::narrowphase::detect_contacts_pair(
    expectTrue(baseAccepted.valid, "base dispatch unchanged for sleeping pair on valid path");

void testManifoldShallowPrunePreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
        fuse::physics::narrowphase::manifold_shallow_prune_rejects_for_reason(
            empty,
            fuse::physics::narrowphase::ManifoldShallowPruneRejectReason::EmptyManifold,
            0.05f),
        "shallow prune rejects_for_reason flags empty manifold");

    fuse::physics::narrowphase::ContactManifold deepOnly{};
    deepOnly.contactNormal = {0.f, 1.f, 0.f};
    deepOnly.addPoint({0.f, 0.f, 0.f}, 0.5f);
        fuse::physics::narrowphase::manifold_shallow_prune_reject_reason(deepOnly, 0.05f) ==
            fuse::physics::narrowphase::ManifoldShallowPruneRejectReason::NoShallowPoints,
        "shallow prune reject reason flags no shallow points");

    fuse::physics::narrowphase::ContactManifold mixed = deepOnly;
    mixed.addPoint({1.f, 0.f, 0.f}, 0.01f);
        fuse::physics::narrowphase::prune_shallow_penetrations_with_preflight(mixed, 0.05f),
        "shallow prune_with_preflight keeps deep point");
    expectTrue(mixed.pointCount == 1u, "shallow prune_with_preflight removes shallow slot");
            fuse::physics::narrowphase::manifold_shallow_prune_reject_reason_name(
                fuse::physics::narrowphase::ManifoldShallowPruneRejectReason::NoShallowPoints),
            "NoShallowPoints") == 0,
        "shallow prune reject reason name resolves NoShallowPoints");

void testFrictionComputeTangentsPreflightGuard() {
    fuse::physics::narrowphase::compute_friction_tangents_with_preflight(empty);
    expectTrue(!empty.hasFrictionBasis(), "compute_tangents_with_preflight skips empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    fuse::physics::narrowphase::compute_friction_tangents_with_preflight(needsBuild);
    expectTrue(needsBuild.hasFrictionBasis(), "compute_tangents_with_preflight builds basis");

    const auto cachedTangent1 = needsBuild.frictionBasis.tangent1;
    expectNear(
        needsBuild.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "compute_tangents_with_preflight reuses valid basis");
        "contact-buffer write reject name resolves InvalidManifold");

    expectTrue(buffer.slotIsValid(0u), "contact-buffer slotIsValid true after write");
    expectTrue(buffer.countValidSlots() == 2u, "contact-buffer countValidSlots after two writes");
    expectTrue(buffer.canSkipCompaction(), "contact-buffer canSkipCompaction when all slots valid");

    const auto compactionPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
        compactionPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid,
        "contact-buffer compaction preflight all-valid with filled slots");
        "contact-buffer can skip compaction when all slots valid");

    expectTrue(buffer.compact() == 2u, "contact-buffer compact sets active count from valid slots");
        "contact-buffer compaction rejects_for_reason all-valid");

    const auto validWrite =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, valid);
    expectTrue(validWrite.canWrite(), "write preflight accepts valid manifold in range");

    const auto selfWrite =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, selfPair);
    expectTrue(selfWrite.selfPair, "write preflight marks self-pair manifold");
    expectTrue(!selfWrite.canWrite(), "write preflight rejects self-pair manifold");

    fuse::physics::narrowphase::ContactManifold invalid{};
    const auto invalidWrite =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, invalid);
    expectTrue(invalidWrite.invalidManifold, "write preflight marks invalid manifold");
    expectTrue(!invalidWrite.canWrite(), "write preflight rejects invalid manifold");

    const auto outOfRangeWrite =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 4u, valid);
    expectTrue(outOfRangeWrite.outOfRangeSlot, "write preflight marks out-of-range slot");
    expectTrue(!outOfRangeWrite.canWrite(), "write preflight rejects out-of-range slot");

        fuse::physics::narrowphase::writeContactBufferSlotWithPreflight(buffer, 0u, valid),
        "guarded write stores valid manifold");
        !fuse::physics::narrowphase::writeContactBufferSlotWithPreflight(buffer, 1u, selfPair),
        "guarded write skips self-pair manifold");

    const auto compactionPreflight =
        fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(compactionPreflight.needsCompaction(),
        "compaction preflight requests work when invalid slots exist");

    const auto skipCompaction =
    expectTrue(!skipCompaction.needsCompaction(),
        "compaction preflight skips when all slots are valid");
    expectTrue(buffer.canSkipCompaction(), "canSkipCompaction true when all slots valid");

    fuse::physics::narrowphase::ContactBufferSoA clampBuffer;
    clampBuffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold shallow = valid;
    shallow.penetrationDepth = 0.1f;
    shallow.points[0].penetration = 0.1f;
    fuse::physics::narrowphase::ContactManifold deep = valid;
    deep.bodyB = 2u;
    deep.penetrationDepth = 0.9f;
    deep.points[0].penetration = 0.9f;
    clampBuffer.setMaxCapacity(1u);
    clampBuffer.writeSlot(0u, valid);
    clampBuffer.writeSlot(1u, deep);
    clampBuffer.compact();
        !fuse::physics::narrowphase::can_skip_contact_buffer_clamp(clampBuffer),
        "contact-buffer cannot skip clamp when over capacity");
    const auto clampPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(clampBuffer);
    expectTrue(clampPreflight.needs_clamp(), "contact-buffer clamp preflight needs clamp over capacity");

    fuse::physics::narrowphase::ContactBufferSoA emptyBuffer;
        fuse::physics::narrowphase::contact_buffer_compact_and_clamp_rejects_for_reason(
            emptyBuffer, fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer),
        "contact-buffer compact-and-clamp rejects empty buffer");
        fuse::physics::narrowphase::can_skip_contact_buffer_compact_and_clamp(emptyBuffer),
        "contact-buffer can skip compact-and-clamp on empty buffer");

void testNarrowphaseIntoBufferPreflightGuards() {
        fuse::physics::narrowphase::write_contact_buffer_slot_with_preflight(buffer, 0u, valid),
        "write_with_preflight stores valid manifold");

        "write reject reason flags self pair");
        !fuse::physics::narrowphase::write_contact_buffer_slot_with_preflight(buffer, 1u, selfPair),
        "write_with_preflight rejects self pair");

            buffer, 1u, invalid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "write reject reason flags invalid manifold");
        fuse::physics::narrowphase::can_skip_contact_buffer_write(buffer, 1u, invalid),
        "can_skip_write true for invalid manifold");

                fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
            "OutOfRangeSlot") == 0,
        "write reject reason name resolves OutOfRangeSlot");

void testContactBufferCompactionRejectReasonGuards() {
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "empty buffer rejects for EmptyBuffer compaction reason");
        "can_skip_compaction on empty buffer");

    valid.penetrationDepth = 0.2f;



    expectEq(
        static_cast<fuse::u32>(
            fuse::physics::narrowphase::contact_buffer_write_reject_reason(buffer, 99u, valid)),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
        "out-of-range slot reports OutOfRangeSlot write reject reason");
            fuse::physics::narrowphase::contact_buffer_write_reject_reason(buffer, 0u, invalid)),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "invalid manifold reports InvalidManifold write reject reason");
            fuse::physics::narrowphase::contact_buffer_write_reject_reason(buffer, 0u, selfPair)),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "self pair reports SelfPair write reject reason");
            "SelfPair") == 0,
        "SelfPair write reject reason has stable label");

    const fuse::physics::narrowphase::ContactBufferWritePreflight preflight =
    expectTrue(preflight.can_write(), "write preflight accepts valid manifold");
        "should_skip false for valid manifold");

    expectTrue(buffer.slotIsValid(0u), "writeSlot stores valid slot");
    buffer.writeSlot(1u, selfPair);
    expectTrue(!buffer.slotIsValid(1u), "writeSlot rejects self pair");

        static_cast<fuse::u32>(fuse::physics::narrowphase::contact_buffer_compaction_reject_reason(buffer)),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "empty buffer reports EmptyBuffer compaction reject reason");
        "can_skip compaction on empty buffer");
        !fuse::physics::narrowphase::should_run_contact_buffer_compaction(buffer),
        "should_run compaction false on empty buffer");

    expectTrue(validPreflight.canWrite(), "write preflight allows valid manifold");
        !fuse::physics::narrowphase::shouldSkipContactBufferWrite(buffer, 0u, valid),
        "shouldSkipContactBufferWrite false for valid manifold");

            buffer, 0u, invalid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
            buffer, 0u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
        "write reject reason name resolves SelfPair");

            buffer, 9u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
        "write rejects_for_reason flags out-of-range slot");

    expectTrue(!buffer.slotIsValid(1u), "writeSlot rejects self pair via preflight");

void testContactBufferCompactionPreflightGuards() {
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipContactBufferCompaction on empty buffer");
    expectTrue(buffer.compact() == 0u, "compact early-outs via preflight on empty buffer");

    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(1u, manifold);
    manifold.bodyB = 2u;
        "all-valid slots reject for AllValid compaction reason");
        "should_run_compaction false when all slots valid");

    buffer.validFlags[1u] = 0u;
        fuse::physics::narrowphase::should_run_contact_buffer_compaction(buffer),
        "should_run_compaction true when invalid slot exists");
    expectTrue(buffer.compact() == 1u, "compaction keeps valid slot");

void testContactBufferClampRejectReasonGuards() {
    buffer.setMaxCapacity(2u);
    buffer.preparePairSlots(3u);

        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer,
            4u,
        "write rejects_for_reason flags out-of-range slot");
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write reject reason name resolves SelfPair");

    buffer.invalidateSlot(0u);
    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot clears valid flag");

void testContactBufferCompactionPreflightGuards() {
    expectTrue(buffer.canSkipSoAIteration(), "empty buffer skips SoA iteration");
    expectTrue(buffer.canSkipCompaction(), "empty buffer skips compaction");
    expectEq(buffer.countValidSlots(), 0u, "countValidSlots early-outs when empty");

    const auto emptyPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(emptyPreflight.emptyBuffer, "compaction preflight marks empty buffer");
    expectTrue(!emptyPreflight.needsCompaction(), "compaction preflight skips empty buffer");
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
        "compaction rejects_for_reason flags empty buffer");

    expectTrue(!buffer.canSkipSoAIteration(), "non-empty slot storage does not skip iteration");

    fuse::physics::narrowphase::ContactManifold contact{};
    contact.valid = true;
    contact.bodyA = 0u;
    contact.bodyB = 1u;
    contact.contactNormal = {0.f, 1.f, 0.f};
    contact.penetrationDepth = 0.2f;
    contact.addPoint({0.f, 0.f, 0.f}, 0.2f);
    buffer.writeSlot(0u, contact);
    buffer.writeSlot(2u, contact);

    expectEq(buffer.countValidSlots(), 2u, "countValidSlots counts prepared valid slots");
    expectTrue(!buffer.canSkipCompaction(), "sparse valid slots need compaction");
        fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRunContactBufferCompaction on sparse slots");
    expectEq(buffer.compact(), 2u, "compact gathers sparse valid slots");
    expectTrue(buffer.canSkipCompaction(), "compacted all-valid buffer skips compaction");

    const auto allValidPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(allValidPreflight.allValid, "compaction preflight marks all-valid buffer");
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipContactBufferCompaction on all-valid buffer");

void testContactBufferClampPreflightGuards() {
    buffer.setMaxCapacity(4u);

    manifold.penetrationDepth = 0.2f;
    expectTrue(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "all-valid prepared slots report AllValid compaction reject reason");
    expectTrue(buffer.countValidSlots() == 2u, "countValidSlots reports prepared valid slots");

    buffer.invalidateSlot(1u);
        "shouldRunContactBufferCompaction true when invalid slots exist");
    expectTrue(buffer.compact() == 1u, "compact gathers surviving valid slot");
    expectTrue(buffer.activeCount == 1u, "compact updates active count");
}

    fuse::physics::narrowphase::ContactBufferSoA buffer;
        fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::EmptyBuffer),
        "empty buffer reports EmptyBuffer clamp reject reason");
        fuse::physics::narrowphase::canSkipContactBufferClamp(buffer),
        "canSkipContactBufferClamp on empty buffer");

    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold shallow{};
    shallow.valid = true;
    shallow.bodyA = 0u;
    shallow.bodyB = 1u;
    shallow.contactNormal = {0.f, 1.f, 0.f};
    shallow.penetrationDepth = 0.1f;
    shallow.addPoint({0.f, 0.f, 0.f}, 0.1f);

    fuse::physics::narrowphase::ContactManifold deep = shallow;

    fuse::physics::narrowphase::ContactManifold medium = shallow;
    medium.bodyB = 3u;
    medium.penetrationDepth = 0.5f;
    medium.points[0].penetration = 0.5f;

    buffer.writeSlot(0u, shallow);
    buffer.writeSlot(1u, deep);
    buffer.writeSlot(2u, medium);

        fuse::physics::narrowphase::should_run_contact_buffer_clamp(buffer),
        "should_run_clamp true when active exceeds max capacity");

    expectTrue(buffer.applyMaxCapacityClamp() == 2u, "clamp truncates to max capacity");
    expectTrue(buffer.droppedCount == 1u, "clamp tracks dropped contacts");

    const auto withinPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(buffer);
        withinPreflight.withinCapacity,
        "clamp preflight marks within-capacity buffer");
        fuse::physics::narrowphase::can_skip_contact_buffer_clamp(buffer),
        "can_skip_clamp true after truncation");
            fuse::physics::narrowphase::contact_buffer_clamp_reject_reason_name(
    buffer.writeSlot(1u, shallow);
    buffer.compact();
    expectTrue(
        fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
        "within-capacity buffer reports WithinCapacity clamp reject reason");
        std::strcmp(
            fuse::physics::narrowphase::contactBufferClampRejectReasonName(
                fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
            "WithinCapacity") == 0,
        "clamp reject reason name resolves WithinCapacity");

void testContactBufferCompactAndClampPreflightGuards() {
            buffer, fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer),
        "empty buffer rejects for EmptyBuffer compact-and-clamp reason");
        fuse::physics::narrowphase::can_skip_contact_buffer_compact_and_clamp(buffer),
        "can_skip_compact_and_clamp on empty buffer");

    buffer.preparePairSlots(4u);




    fuse::physics::narrowphase::ContactManifold selfPair = shallow;

    buffer.writeSlot(3u, selfPair);

        fuse::physics::narrowphase::compact_and_clamp_contact_buffer_with_preflight(buffer) == 2u,
        "compact_and_clamp_with_preflight ignores self pair and truncates");
    expectNear(buffer.manifoldAt(0u).penetrationDepth, 0.9f, 1e-4f, "preflight compact-and-clamp keeps deepest");

void testContactPairPlanePlaneDeepenRejectGuards() {
    const fuse::u32 planeA = bodies.addBody({0.f, -1.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 planeB = bodies.addBody({0.f, -2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeA, {0.f, 1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeB, {0.f, 1.f, 0.f});

        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({planeA, planeB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::PlanePlane,
        "deepen reject reason flags plane-plane pair");
        fuse::physics::narrowphase::contact_pair_reject_reason({planeA, planeB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedShapePair,
        "base reject reason unchanged for plane-plane pair");
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::PlanePlane),
            "PlanePlane") == 0,
        "reject reason name resolves PlanePlane");

void testDetectContactsPairWithPreflightGuard() {
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{bodyA, bodyB}};
    const auto preflight =
        fuse::physics::narrowphase::preflight_narrowphase_into_buffer(pairs, bodies, shapes, buffer);
    expectTrue(preflight.can_dispatch(), "narrowphase into-buffer preflight can dispatch valid pair");
    expectTrue(preflight.dispatchableCount == 1u, "narrowphase into-buffer preflight counts dispatchable pair");
        !fuse::physics::narrowphase::can_skip_narrowphase_into_buffer(pairs, bodies, shapes),
        "narrowphase into-buffer skip false for valid pair");

    const auto slotPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_pair_slot({bodyA, bodyB}, bodies, shapes);
    expectTrue(slotPreflight.can_dispatch(), "narrowphase pair-slot preflight allows valid pair");
    expectTrue(slotPreflight.canWriteSlot, "narrowphase pair-slot preflight can write slot");
        !fuse::physics::narrowphase::should_skip_narrowphase_pair_slot({bodyA, bodyB}, bodies, shapes),
        "narrowphase pair-slot skip guard allows valid pair");
        fuse::physics::narrowphase::should_skip_narrowphase_pair_slot({bodyA, bodyA}, bodies, shapes),
        "narrowphase pair-slot skip guard rejects self pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> sleepingPairs = {{sleepingA, sleepingB}};
        fuse::physics::narrowphase::count_contact_pairs_rejected_for_reason(
            sleepingPairs,
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping) == 1u,
        "count rejected pairs for BothSleeping reason");
        !fuse::physics::narrowphase::can_skip_narrowphase_into_buffer(sleepingPairs, bodies, shapes),
        "narrowphase into-buffer skip false for sleeping pair on base dispatch path");

void testManifoldGenerateWithPreflightGuards() {
        fuse::physics::narrowphase::can_skip_generate_contact_manifold(empty),
        "can_skip_generate true for empty manifold");
        !fuse::physics::narrowphase::generate_contact_manifold_with_preflight(empty),
        "generate_with_preflight no-ops on empty manifold");

    fuse::physics::narrowphase::ContactManifold manifold =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
        !fuse::physics::narrowphase::can_skip_generate_contact_manifold(manifold),
        "can_skip_generate false for detected sphere pair");
    clampBuffer.writeSlot(0u, shallow);

    const auto clampPreflight =
        fuse::physics::narrowphase::preflightContactBufferClamp(clampBuffer);
    expectTrue(clampPreflight.needsClamp(), "clamp preflight requests overflow truncation");
    expectTrue(clampBuffer.canApplyMaxCapacityClamp(), "canApplyMaxCapacityClamp true when over max");

    fuse::physics::narrowphase::ContactBufferSoA withinBuffer;
    withinBuffer.preparePairSlots(1u);
    withinBuffer.writeSlot(0u, valid);
    withinBuffer.compact();
    withinBuffer.setMaxCapacity(2u);
    const auto withinPreflight =
        fuse::physics::narrowphase::preflightContactBufferClamp(withinBuffer);
    expectTrue(!withinPreflight.needsClamp(), "clamp preflight skips when within capacity");

    fuse::physics::narrowphase::ContactBufferSoA compactClampBuffer;
    compactClampBuffer.preparePairSlots(2u);
    compactClampBuffer.writeSlot(0u, valid);
    const auto compactClampPreflight =
        fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(compactClampBuffer);
    expectTrue(compactClampPreflight.needsCompactAndClamp(),
        "compact-and-clamp preflight requests work with invalid slot present");

    compactClampBuffer.writeSlot(1u, second);
    compactClampBuffer.compact();
    compactClampBuffer.setMaxCapacity(4u);
    const auto noWorkPreflight =
    expectTrue(noWorkPreflight.noWork, "compact-and-clamp preflight skips when already compact");
    expectTrue(compactClampBuffer.canSkipCompactAndClamp(),
        "canSkipCompactAndClamp true when no work remains");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write reject reason name resolves SelfPair");
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            compactClampBuffer,
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "compaction rejects_for_reason matches all-valid buffer");
}

void testDeepenFollowUpPreflightWrappers() {
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

    const auto sleepingPair =
        fuse::physics::narrowphase::detect_contacts_pair_deepen({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingPair.valid, "deepen detect rejects both-sleeping pair");
    expectTrue(sleepingPair.empty(), "deepen detect leaves sleeping pair empty");

    const auto validPair =
        fuse::physics::narrowphase::detect_contacts_pair_deepen({dynamicA, dynamicB}, bodies, shapes);
    expectTrue(validPair.valid, "deepen detect accepts valid pair");

        fuse::physics::narrowphase::generate_contact_manifold_with_preflight(manifold),
        "generate_with_preflight finalizes valid manifold");
    expectTrue(manifold.hasFrictionBasis(), "generate_with_preflight builds friction basis");

    fuse::physics::narrowphase::ContactManifold overCap{};
    overCap.contactNormal = {0.f, 1.f, 0.f};
    overCap.pointCount = fuse::physics::narrowphase::kMaxContactPointsPerManifold + 1u;
    overCap.points[0].penetration = 0.2f;
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            overCap, fuse::physics::narrowphase::ManifoldPruneRejectReason::ExceedsMaxPoints),
        "prune rejects_for_reason flags exceeds-max-points manifold");
            fuse::physics::narrowphase::manifold_prune_reject_reason_name(
                fuse::physics::narrowphase::ManifoldPruneRejectReason::ExceedsMaxPoints),
            "ExceedsMaxPoints") == 0,
        "prune reject name resolves ExceedsMaxPoints");

void testFrictionBasisDeepenPassGuards() {
    fuse::physics::narrowphase::ContactManifold stale{};
    stale.contactNormal = {0.f, 1.f, 0.f};
    stale.addPoint({0.f, 0.f, 0.f}, 0.2f);
    stale.buildFrictionBasis();
    stale.contactNormal = {1.f, 0.f, 0.f};

        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            stale, fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis),
        "friction rejects_for_reason flags stale basis");
            fuse::physics::narrowphase::friction_basis_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis),
            "StaleBasis") == 0,
        "friction reject name resolves StaleBasis");

    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.2f);
    fuse::physics::narrowphase::normalize_contact_normal_for_friction(unnormalized);
    expectNear(unnormalized.contactNormal.y, 1.f, 1e-4f, "normalize_contact_normal_for_friction unitizes normal");

        fuse::physics::narrowphase::rebuild_friction_basis_with_normalize_preflight(unnormalized),
        "rebuild_with_normalize_preflight builds basis for unnormalized normal");
    expectTrue(unnormalized.hasFrictionBasis(), "rebuild_with_normalize_preflight stores orthonormal basis");
    const auto valid =
        fuse::physics::narrowphase::detect_contacts_pair_with_preflight({bodyA, bodyB}, bodies, shapes);
    expectTrue(valid.valid, "detect_with_preflight accepts valid pair");
    expectTrue(valid.pointCount > 0u, "detect_with_preflight populates contact points");

    const auto selfPair =
        fuse::physics::narrowphase::detect_contacts_pair_with_preflight({bodyA, bodyA}, bodies, shapes);
    expectTrue(!selfPair.valid, "detect_with_preflight rejects self pair");
    expectTrue(selfPair.empty(), "detect_with_preflight self pair has no points");

void testNormalizeContactNormalIfNeededGuard() {
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.contactNormal = {0.f, 2.f, 0.f};
        fuse::physics::narrowphase::normalize_contact_normal_if_needed(manifold),
        "normalize_if_needed scales non-unit normal");
    expectNear(manifold.contactNormal.length(), 1.f, 1e-4f, "normalize_if_needed produces unit normal");

    fuse::physics::narrowphase::ContactManifold unit{};
    unit.contactNormal = {0.f, 1.f, 0.f};
    unit.addPoint({0.f, 0.f, 0.f}, 0.1f);
        !fuse::physics::narrowphase::normalize_contact_normal_if_needed(unit),
        "normalize_if_needed no-ops on unit normal");

void testNarrowphaseBufferFinalizePreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_narrowphase_buffer_finalize(buffer);
    expectTrue(emptyPreflight.skipped, "buffer finalize preflight skips empty buffer");
        fuse::physics::narrowphase::can_skip_narrowphase_buffer_finalize(buffer),
        "can_skip_buffer_finalize on empty buffer");

    buffer.preparePairSlots(2u);
    manifold.penetrationDepth = 0.2f;
    expectEq(
        static_cast<fuse::u32>(fuse::physics::narrowphase::contact_buffer_compaction_reject_reason(buffer)),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "all-valid slots report AllValid compaction reject reason");
        fuse::physics::narrowphase::contact_buffer_compaction_rejects_for_reason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "all-valid slots reject for AllValid");

    buffer.invalidateSlot(1u);
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferCompactionRejectReason::None),
        "invalid slots report None compaction reject reason");
        "should_run compaction true when invalid slots exist");

        static_cast<fuse::u32>(fuse::physics::narrowphase::contact_buffer_clamp_reject_reason(buffer)),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferClampRejectReason::EmptyBuffer),
        "empty buffer reports EmptyBuffer clamp reject reason");
        "can_skip clamp on empty buffer");

    buffer.preparePairSlots(1u);
    buffer.setMaxCapacity(1u);
    expectTrue(buffer.canApplyMaxCapacityClamp(), "canApplyMaxCapacityClamp true when over max");
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRunContactBufferClamp true when over max");
    expectTrue(buffer.applyMaxCapacityClamp() == 1u, "applyMaxCapacityClamp truncates via preflight gate");
    expectTrue(buffer.hasDroppedContacts(), "clamp sets dropped count");

        fuse::physics::narrowphase::contactBufferCompactAndClampRejectsForReason(
            buffer,
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer),
        "empty buffer reports EmptyBuffer compact-and-clamp reject reason");
        fuse::physics::narrowphase::canSkipContactBufferCompactAndClamp(buffer),
        "canSkipContactBufferCompactAndClamp on empty buffer");
    expectTrue(buffer.compactAndClamp() == 0u, "compactAndClamp early-outs via preflight on empty buffer");

    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    buffer.writeSlot(0u, manifold);
    buffer.validFlags[1u] = 0u;

    const auto dirtyPreflight = fuse::physics::narrowphase::preflight_narrowphase_buffer_finalize(buffer);
    expectTrue(!dirtyPreflight.skipped, "buffer finalize preflight does not skip prepared slots");
    expectTrue(dirtyPreflight.needsCompaction, "buffer finalize preflight needs compaction");
        !fuse::physics::narrowphase::can_skip_narrowphase_buffer_finalize(buffer),
        "can_skip_buffer_finalize false when compaction needed");

void testFrictionBasisNormalizeBeforeRebuildGuard() {
        fuse::physics::narrowphase::rebuild_friction_basis_with_preflight(manifold),
        "rebuild_with_preflight normalizes and builds basis");
    expectNear(manifold.contactNormal.length(), 1.f, 1e-4f, "rebuild_with_preflight stores unit normal");
        fuse::physics::narrowphase::isOrthonormalTangentBasis(
            manifold.contactNormal, manifold.frictionBasis),
        "rebuild_with_preflight stores orthonormal basis");
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    buffer.compact();
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
        "within-capacity buffer reports WithinCapacity clamp reject reason");
        !fuse::physics::narrowphase::should_run_contact_buffer_clamp(buffer),
        "should_run clamp false when within capacity");

    buffer.setMaxCapacity(1u);
    manifold.penetrationDepth = 0.9f;
    manifold.points[0].penetration = 0.9f;
    buffer.writeSlot(1u, manifold);
        static_cast<fuse::u32>(fuse::physics::narrowphase::ContactBufferClampRejectReason::None),
        "overflow buffer reports None clamp reject reason");
        "should_run clamp true when overflow exists");

        static_cast<fuse::u32>(
            fuse::physics::narrowphase::contact_buffer_compact_and_clamp_reject_reason(buffer)),
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer),
        "empty buffer reports EmptyBuffer compact-and-clamp reject reason");
        "can_skip compact-and-clamp on empty buffer");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp early-outs via preflight on empty buffer");


            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork),
        "synced within-capacity buffer reports NoWork compact-and-clamp reject reason");
        fuse::physics::narrowphase::contact_buffer_compact_and_clamp_rejects_for_reason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork),
        "synced buffer rejects for NoWork");
    expectEq(buffer.compactAndClamp(), 1u, "compactAndClamp no-op returns synced active count");

            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::None),
        "prepared slots with stale activeCount report None compact-and-clamp reject reason");

        fuse::physics::narrowphase::should_run_contact_buffer_compact_and_clamp(buffer),
        "should_run compact-and-clamp true when invalid slots exist");
    manifold.penetrationDepth = 0.3f;
    manifold.addPoint({0.f, 0.f, 0.f}, 0.3f);
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactAndClampRejectsForReason(
            buffer,
    expectTrue(buffer.compactAndClamp() == 2u, "compactAndClamp no-op returns synced active count");

    fuse::physics::narrowphase::ContactBufferSoA clampBuffer;
    clampBuffer.setMaxCapacity(1u);
    clampBuffer.preparePairSlots(2u);
    clampBuffer.writeSlot(0u, manifold);
    clampBuffer.writeSlot(1u, manifold);
    expectEq(clampBuffer.compactAndClamp(), 1u, "compactAndClamp gathers then clamps via preflight gate");
    expectNear(clampBuffer.manifoldAt(0u).penetrationDepth, 0.9f, 1e-4f, "clamp keeps deepest penetration");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
        !fuse::physics::narrowphase::generate_contact_manifold_with_preflight(noNormal),
        "generate_with_preflight no-ops on invalid normal");

    fuse::physics::narrowphase::ContactManifold frictionTarget{};
    frictionTarget.contactNormal = {0.f, 1.f, 0.f};
    frictionTarget.addPoint({0.f, 0.f, 0.f}, 0.2f);
        fuse::physics::narrowphase::compute_friction_tangents_with_preflight(frictionTarget),
        "compute_friction_tangents_with_preflight builds basis");
    expectTrue(frictionTarget.hasFrictionBasis(), "preflight friction tangents store orthonormal basis");

    const auto cachedTangent1 = frictionTarget.frictionBasis.tangent1;
        "compute_friction_tangents_with_preflight reuses cached basis");
    expectNear(
        frictionTarget.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "preflight friction tangents preserve cached tangent1");
    deep.bodyB = 2u;
    deep.penetrationDepth = 0.9f;
    deep.points[0].penetration = 0.9f;


    const auto withinPreflight = fuse::physics::narrowphase::preflightContactBufferClamp(buffer);
    expectTrue(withinPreflight.withinCapacity, "clamp preflight marks within-capacity buffer");
        fuse::physics::narrowphase::canSkipContactBufferClamp(buffer),
        "canSkipContactBufferClamp before overflow");

    expectTrue(buffer.canApplyMaxCapacityClamp(), "canApplyMaxCapacityClamp when over capacity");
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRunContactBufferClamp when over capacity");
        !fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
        "clamp rejects_for_reason does not false-positive overflow buffer");
    expectEq(buffer.applyMaxCapacityClamp(), 1u, "applyMaxCapacityClamp truncates via preflight gate");
    expectTrue(buffer.hasDroppedContacts(), "hasDroppedContacts after clamp");

        buffer.compactAndClamp(),
        0u,
        "compactAndClamp early-outs via preflight on empty buffer");
        fuse::physics::narrowphase::contactBufferCompactAndClampRejectsForReason(
            buffer,
        "compactAndClamp rejects_for_reason flags empty buffer");

    fuse::physics::narrowphase::ContactManifold contact{};
    contact.valid = true;
    contact.bodyA = 0u;
    contact.bodyB = 1u;
    contact.contactNormal = {0.f, 1.f, 0.f};
    contact.penetrationDepth = 0.3f;
    contact.addPoint({0.f, 0.f, 0.f}, 0.3f);
    buffer.writeSlot(0u, contact);
    buffer.writeSlot(1u, contact);

    const auto noWorkPreflight = fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(buffer);
    expectTrue(noWorkPreflight.noWork, "compactAndClamp preflight marks no-work after compact");
        fuse::physics::narrowphase::canSkipContactBufferCompactAndClamp(buffer),
        "canSkipContactBufferCompactAndClamp after compact");

    buffer.clear();
    fuse::physics::narrowphase::ContactManifold deeper = contact;
    deeper.bodyB = 2u;
    deeper.penetrationDepth = 0.8f;
    deeper.points[0].penetration = 0.8f;
    buffer.writeSlot(1u, deeper);
        1u,
        "compactAndClamp gathers then clamps via preflight gate");
    expectNear(buffer.manifoldAt(0u).penetrationDepth, 0.8f, 1e-4f, "compactAndClamp keeps deepest contact");
            fuse::physics::narrowphase::contactBufferCompactAndClampRejectReasonName(
            "NoWork") == 0,
        "compactAndClamp reject reason name resolves NoWork");
    fuse::physics::narrowphase::ContactManifold deeper = manifold;
    deeper.penetrationDepth = 0.9f;
    deeper.points[0].penetration = 0.9f;
    clampBuffer.writeSlot(1u, deeper);
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferCompactAndClamp(clampBuffer),
        "shouldRunContactBufferCompactAndClamp true when clamp needed");
    expectTrue(clampBuffer.compactAndClamp() == 1u, "compactAndClamp gathers then clamps via preflight gate");
    expectNear(clampBuffer.manifoldAt(0u).penetrationDepth, 0.9f, 1e-4f, "compactAndClamp keeps deepest penetration");
}

void testContactBufferWritePreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    const auto validPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 0u, valid);
    expectTrue(validPreflight.canWrite(), "write preflight allows valid manifold");
    expectTrue(
        buffer.writeSlotWithPreflight(0u, valid),
        "writeSlotWithPreflight stores valid manifold");
    expectTrue(buffer.slotIsValid(0u), "writeSlotWithPreflight marks slot valid");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_rejects_for_reason(
            buffer, 1u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write reject reason flags self pair");
    expectTrue(
        !buffer.writeSlotWithPreflight(1u, selfPair),
        "writeSlotWithPreflight rejects self pair");

    fuse::physics::narrowphase::ContactManifold invalid = valid;
    invalid.valid = false;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_rejects_for_reason(
            buffer, 1u, invalid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "write reject reason flags invalid manifold");
    expectTrue(
        fuse::physics::narrowphase::should_skip_contact_buffer_write(buffer, 1u, invalid),
        "should_skip rejects invalid manifold");

    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_rejects_for_reason(
            buffer, 9u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
        "write reject reason flags out-of-range slot");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_buffer_write_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
            "SelfPair") == 0,
        "write reject reason name resolves SelfPair");
}

void testContactBufferCompactionPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(emptyPreflight.emptyBuffer, "compaction preflight marks empty buffer");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_compaction(buffer),
        "can_skip compaction on empty buffer");

    buffer.preparePairSlots(3u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(2u, manifold);

    const auto needsPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(needsPreflight.needsCompaction(), "compaction preflight needs work with hole");
    expectTrue(
        fuse::physics::narrowphase::should_run_contact_buffer_compaction(buffer),
        "should_run compaction when invalid slot exists");
    expectTrue(buffer.compact() == 2u, "compact gathers valid slots");
    expectTrue(buffer.countValidSlots() == 2u, "countValidSlots after compact");

    const auto allValidPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(allValidPreflight.allValid, "compaction preflight all-valid after compact");
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_compaction_rejects_for_reason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "compaction reject reason flags all-valid buffer");
}

void testContactBufferClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA withinBuffer;
    withinBuffer.setMaxCapacity(2u);
    withinBuffer.preparePairSlots(2u);

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

    withinBuffer.writeSlot(0u, shallow);
    withinBuffer.writeSlot(1u, deep);
    withinBuffer.compact();

    const auto withinPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(withinBuffer);
    expectTrue(withinPreflight.withinCapacity, "clamp preflight within capacity before overflow");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_clamp(withinBuffer),
        "can_skip clamp when within capacity");

    fuse::physics::narrowphase::ContactBufferSoA overflowBuffer;
    overflowBuffer.setMaxCapacity(2u);
    overflowBuffer.preparePairSlots(4u);
    overflowBuffer.writeSlot(0u, shallow);
    overflowBuffer.writeSlot(1u, deep);
    overflowBuffer.writeSlot(2u, shallow);
    overflowBuffer.compact();
    expectTrue(overflowBuffer.activeCount == 3u, "buffer has three contacts before clamp");

    const auto needsClampPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(overflowBuffer);
    expectTrue(needsClampPreflight.needsClamp(), "clamp preflight needs work when over capacity");
    expectTrue(
        fuse::physics::narrowphase::should_run_contact_buffer_clamp(overflowBuffer),
        "should_run clamp when over capacity");
    expectTrue(overflowBuffer.canApplyMaxCapacityClamp(), "canApplyMaxCapacityClamp true when over capacity");
    expectTrue(overflowBuffer.applyMaxCapacityClamp() == 2u, "applyMaxCapacityClamp truncates to max");
    expectTrue(overflowBuffer.hasDroppedContacts(), "hasDroppedContacts after clamp");
}

void testContactBufferCompactAndClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compact_and_clamp(buffer);
    expectTrue(emptyPreflight.emptyBuffer, "compact-and-clamp preflight marks empty buffer");
    expectTrue(buffer.compactAndClamp() == 0u, "compactAndClamp early-outs on empty buffer");

    buffer.setMaxCapacity(1u);
    buffer.preparePairSlots(2u);
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

    buffer.writeSlot(0u, shallow);
    buffer.writeSlot(1u, deep);
    expectTrue(
        fuse::physics::narrowphase::should_run_contact_buffer_compact_and_clamp(buffer),
        "should_run compact-and-clamp with invalid slot and overflow");
    expectTrue(buffer.compactAndClamp() == 1u, "compactAndClamp gathers and clamps via preflight gate");
    expectNear(buffer.manifoldAt(0u).penetrationDepth, 0.9f, 1e-4f, "compactAndClamp keeps deepest contact");

    buffer.preparePairSlots(1u);
    buffer.writeSlot(0u, shallow);
    buffer.compact();
    const auto noWorkPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compact_and_clamp(buffer);
    expectTrue(noWorkPreflight.noWork, "compact-and-clamp preflight no-work when clean");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_compact_and_clamp(buffer),
        "can_skip compact-and-clamp when no work");
}

void testContactPairBothPlanesDeepenGuard() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 planeA = bodies.addBody({0.f, -1.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 planeB = bodies.addBody({0.f, -2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeA, {0.f, 1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeB, {0.f, 1.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({planeA, planeB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothPlanes,
        "deepen reject reason flags both-plane pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({planeA, planeB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedShapePair,
        "base reject reason unchanged for both-plane pair");

    const auto rejected =
        fuse::physics::narrowphase::detect_contacts_pair_with_preflight({planeA, planeB}, bodies, shapes);
    expectTrue(!rejected.valid, "detect_with_preflight rejects both-plane pair");
}

void testDetectContactsPairWithPreflightGuard() {
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

    const auto rejected =
        fuse::physics::narrowphase::detect_contacts_pair_with_preflight({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!rejected.valid, "detect_with_preflight rejects both-sleeping pair");

    const auto overlap =
        fuse::physics::narrowphase::detect_contacts_pair_with_preflight({bodyA, bodyB}, bodies, shapes);
    expectTrue(overlap.valid, "detect_with_preflight allows valid pair");
    expectTrue(overlap.pointCount > 0u, "detect_with_preflight populates contact points");
}

void testManifoldNormalizeAndPruneIfNeededGuards() {
    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.3f);
    expectTrue(
        fuse::physics::narrowphase::normalize_contact_normal_if_needed(unnormalized),
        "normalize_if_needed scales non-unit normal");
    expectNear(unnormalized.contactNormal.length(), 1.f, 1e-4f, "normalize_if_needed produces unit normal");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        !fuse::physics::narrowphase::normalize_contact_normal_if_needed(clean),
        "normalize_if_needed no-ops on unit normal");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(dirty),
        "prune_if_needed keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "prune_if_needed removes separated slot");

    fuse::physics::narrowphase::ContactManifold alreadyClean = clean;
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(alreadyClean),
        "prune_if_needed preserves clean manifold");
    expectTrue(alreadyClean.pointCount == 1u, "prune_if_needed leaves clean slots untouched");
}

void testFinalizeContactManifoldIfNeededGuard() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::finalize_contact_manifold_if_needed(empty),
        "finalize_if_needed no-ops on empty manifold");

    fuse::physics::narrowphase::ContactManifold manifold =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::finalize_contact_manifold_if_needed(manifold),
        "finalize_if_needed finalizes valid manifold");
    expectTrue(manifold.valid, "finalize_if_needed sets validity");
    expectTrue(manifold.hasFrictionBasis(), "finalize_if_needed builds friction basis");
}

void testFrictionTangentsWithPreflightGuard() {
    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::compute_friction_tangents_with_preflight(unnormalized),
        "compute_with_preflight builds basis for scaled normal");
    expectTrue(unnormalized.hasFrictionBasis(), "compute_with_preflight stores orthonormal basis");
    expectNear(unnormalized.contactNormal.length(), 1.f, 1e-4f, "compute_with_preflight normalizes contact normal");

    const auto cachedTangent1 = unnormalized.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::compute_friction_tangents_with_preflight(unnormalized),
        "compute_with_preflight reuses valid basis");
    expectNear(
        unnormalized.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "compute_with_preflight preserves cached tangent1");

    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::compute_friction_tangents_with_preflight(empty),
        "compute_with_preflight skips empty manifold");
}

void testContactBufferWritePreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    const auto validPreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, valid);
    expectTrue(validPreflight.canWrite(), "write preflight allows valid manifold");
    expectTrue(
        fuse::physics::narrowphase::writeContactSlotWithPreflight(buffer, 0u, valid),
        "write with preflight stores valid slot");
    expectTrue(buffer.slotIsValid(0u), "written slot is valid");

    fuse::physics::narrowphase::ContactManifold invalid{};
    invalid.valid = false;
    invalid.bodyA = 0u;
    invalid.bodyB = 1u;
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 1u, invalid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "write reject reason flags invalid manifold");
    expectTrue(
        !fuse::physics::narrowphase::writeContactSlotWithPreflight(buffer, 1u, invalid),
        "write with preflight skips invalid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 1u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write reject reason flags self pair");
    buffer.writeSlot(1u, selfPair);
    expectTrue(!buffer.slotIsValid(1u), "writeSlot rejects self pair");

    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 9u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidSlot),
        "write reject reason flags out-of-range slot");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
            "InvalidManifold") == 0,
        "write reject reason name resolves InvalidManifold");
}

void testContactBufferCompactionPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "empty buffer rejects for EmptyBuffer compaction reason");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipContactBufferCompaction on empty buffer");
    expectTrue(
        !fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRunContactBufferCompaction false on empty buffer");

    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(1u, manifold);
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "all-valid slots reject for AllValid compaction reason");
    expectTrue(buffer.canSkipCompaction(), "canSkipCompaction true when all slots valid");

    buffer.invalidateSlot(1u);
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRunContactBufferCompaction true when invalid slot exists");
    expectTrue(buffer.compact() == 1u, "compaction gathers valid slot");
    expectTrue(buffer.hasValidContacts(), "compacted buffer has valid contacts");
}

void testContactBufferClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(
        fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::EmptyBuffer),
        "empty buffer rejects for EmptyBuffer clamp reason");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferClamp(buffer),
        "canSkipContactBufferClamp on empty buffer");

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

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, shallow);
    buffer.writeSlot(1u, deep);
    buffer.compact();
    expectTrue(
        fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
        "within-capacity buffer rejects for WithinCapacity clamp reason");
    expectTrue(!buffer.canApplyMaxCapacityClamp(), "canApplyMaxCapacityClamp false within capacity");

    buffer.setMaxCapacity(1u);
    expectTrue(buffer.canApplyMaxCapacityClamp(), "canApplyMaxCapacityClamp true when overflow");
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRunContactBufferClamp true when overflow exists");
    expectTrue(buffer.applyMaxCapacityClamp() == 1u, "clamp keeps deepest penetration");
    expectTrue(buffer.hasDroppedContacts(), "clamp tracks dropped contacts");
    expectNear(buffer.manifoldAt(0u).penetrationDepth, 0.9f, 1e-4f, "clamp retains deepest contact");
}

void testContactBufferCompactAndClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactAndClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer),
        "empty buffer rejects for EmptyBuffer compact-and-clamp reason");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompactAndClamp(buffer),
        "canSkipContactBufferCompactAndClamp on empty buffer");
    expectTrue(buffer.compactAndClamp() == 0u, "compactAndClamp early-outs on empty buffer");

    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);

    buffer.preparePairSlots(1u);
    buffer.writeSlot(0u, manifold);
    buffer.compact();
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactAndClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork),
        "synced within-capacity buffer rejects for NoWork compact-and-clamp reason");
    expectTrue(buffer.compactAndClamp() == 1u, "compactAndClamp no-op returns active count");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, manifold);
    buffer.invalidateSlot(1u);
    buffer.setMaxCapacity(1u);
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferCompactAndClamp(buffer),
        "shouldRunContactBufferCompactAndClamp true when compaction or clamp needed");
    expectTrue(buffer.compactAndClamp() == 1u, "compactAndClamp gathers and clamps");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferCompactAndClampRejectReasonName(
                fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork),
            "NoWork") == 0,
        "compact-and-clamp reject reason name resolves NoWork");
}

void testContactPairDeepenRejectedGuards() {
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

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {dynamicA, dynamicB},
    };
    expectTrue(
        fuse::physics::narrowphase::count_rejected_contact_pairs(mixedPairs, bodies, shapes) == 1u,
        "count_rejected_contact_pairs reports deepen-rejected pairs");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_rejected({sleepingA, sleepingB}, bodies, shapes),
        "contact_pair_deepen_rejected true for both-sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::contact_pair_deepen_rejected({dynamicA, dynamicB}, bodies, shapes),
        "contact_pair_deepen_rejected false for dispatchable pair");
}

void testGenerateContactManifoldWithPreflightGuard() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::generate_contact_manifold_with_preflight(empty),
        "generate_with_preflight no-ops on empty manifold");

    fuse::physics::narrowphase::ContactManifold manifold =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_with_preflight(manifold),
        "generate_with_preflight finalizes valid manifold");
    expectTrue(manifold.valid, "generate_with_preflight sets validity");
    expectTrue(manifold.hasFrictionBasis(), "generate_with_preflight builds friction basis");
}

void testComputeFrictionTangentsWithPreflightGuard() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::compute_friction_tangents_with_preflight(empty),
        "compute_with_preflight skips empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::compute_friction_tangents_with_preflight(needsBuild),
        "compute_with_preflight builds missing basis");
    expectTrue(needsBuild.hasFrictionBasis(), "compute_with_preflight stores orthonormal basis");

    const auto cachedTangent1 = needsBuild.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::compute_friction_tangents_with_preflight(needsBuild),
        "compute_with_preflight reuses valid basis");
    expectNear(
        needsBuild.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "compute_with_preflight preserves cached tangent1");
}

void testContactBufferWriteRejectReasonGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_reject_reason(buffer, 0u, valid) ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::None,
        "valid manifold reports None write reject reason");
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_rejects_for_reason(
            buffer, 0u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::None),
        "valid manifold rejects for None write reason");

    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_reject_reason(buffer, 5u, valid) ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidSlot,
        "out-of-range slot reports InvalidSlot write reject reason");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_buffer_write_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidSlot),
            "InvalidSlot") == 0,
        "InvalidSlot write reject reason has stable label");

    fuse::physics::narrowphase::ContactManifold invalid{};
    invalid.bodyA = 0u;
    invalid.bodyB = 1u;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_reject_reason(buffer, 0u, invalid) ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold,
        "invalid manifold reports InvalidManifold write reject reason");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_reject_reason(buffer, 0u, selfPair) ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair,
        "self pair reports SelfPair write reject reason");

    const auto preflight =
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 0u, valid);
    expectTrue(preflight.can_write(), "write preflight allows valid manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_contact_buffer_write(buffer, 0u, valid),
        "should_skip false for valid manifold write");

    buffer.writeSlot(0u, valid);
    expectTrue(buffer.slotIsValid(0u), "writeSlot stores valid contact");
    expectTrue(
        fuse::physics::narrowphase::should_skip_contact_buffer_write(buffer, 0u, selfPair),
        "should_skip true for self-pair write");
}

void testContactBufferCompactionRejectReasonGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_compaction_reject_reason(buffer) ==
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer,
        "empty buffer reports EmptyBuffer compaction reject reason");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_compaction(buffer),
        "can_skip compaction on empty buffer");
    expectTrue(
        !fuse::physics::narrowphase::should_run_contact_buffer_compaction(buffer),
        "should_run compaction false on empty buffer");
    expectTrue(buffer.canSkipSoAIteration(), "empty buffer skips SoA iteration");

    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(1u, manifold);

    expectTrue(
        fuse::physics::narrowphase::contact_buffer_compaction_reject_reason(buffer) ==
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid,
        "all-valid slots report AllValid compaction reject reason");
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_compaction_rejects_for_reason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "all-valid slots reject for AllValid compaction reason");
    expectTrue(buffer.canSkipCompaction(), "all-valid slots skip compaction work");
    expectTrue(buffer.countValidSlots() == 2u, "countValidSlots reports two valid slots");

    buffer.invalidateSlot(1u);
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_compaction_reject_reason(buffer) ==
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::None,
        "invalid slot reports None compaction reject reason");
    expectTrue(
        fuse::physics::narrowphase::should_run_contact_buffer_compaction(buffer),
        "should_run compaction true when invalid slots exist");
    expectTrue(buffer.compact() == 1u, "compaction gathers valid slot");
}

void testContactBufferClampRejectReasonGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_clamp_reject_reason(buffer) ==
            fuse::physics::narrowphase::ContactBufferClampRejectReason::EmptyBuffer,
        "empty buffer reports EmptyBuffer clamp reject reason");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_clamp(buffer),
        "can_skip clamp on empty buffer");

    buffer.preparePairSlots(1u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.penetrationDepth = 0.5f;
    manifold.addPoint({0.f, 0.f, 0.f}, 0.5f);
    buffer.writeSlot(0u, manifold);
    buffer.compact();

    expectTrue(
        fuse::physics::narrowphase::contact_buffer_clamp_reject_reason(buffer) ==
            fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity,
        "within-capacity buffer reports WithinCapacity clamp reject reason");
    expectTrue(
        !fuse::physics::narrowphase::should_run_contact_buffer_clamp(buffer),
        "should_run clamp false when within capacity");
    expectTrue(buffer.canSkipMaxCapacityClamp(), "canSkipMaxCapacityClamp when within capacity");

    buffer.setMaxCapacity(1u);
    expectTrue(buffer.remainingCapacity() == 0u, "full buffer has zero remaining capacity");
    expectTrue(buffer.isFull(), "buffer reports full at max capacity");

    buffer.preparePairSlots(3u);
    fuse::physics::narrowphase::ContactManifold shallow = manifold;
    shallow.penetrationDepth = 0.1f;
    shallow.points[0].penetration = 0.1f;
    fuse::physics::narrowphase::ContactManifold deep = manifold;
    deep.bodyB = 2u;
    deep.penetrationDepth = 0.9f;
    deep.points[0].penetration = 0.9f;
    fuse::physics::narrowphase::ContactManifold medium = manifold;
    medium.bodyB = 3u;
    medium.penetrationDepth = 0.5f;
    medium.points[0].penetration = 0.5f;
    buffer.writeSlot(0u, shallow);
    buffer.writeSlot(1u, deep);
    buffer.writeSlot(2u, medium);
    buffer.compact();

    expectTrue(
        fuse::physics::narrowphase::contact_buffer_clamp_reject_reason(buffer) ==
            fuse::physics::narrowphase::ContactBufferClampRejectReason::None,
        "overflow buffer reports None clamp reject reason");
    expectTrue(
        fuse::physics::narrowphase::should_run_contact_buffer_clamp(buffer),
        "should_run clamp true when overflow exists");
    expectTrue(buffer.canApplyMaxCapacityClamp(), "canApplyMaxCapacityClamp when overflow exists");
}

void testContactBufferCompactAndClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_compact_and_clamp_reject_reason(buffer) ==
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer,
        "empty buffer reports EmptyBuffer compact-and-clamp reject reason");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_compact_and_clamp(buffer),
        "can_skip compact-and-clamp on empty buffer");
    expectTrue(buffer.compactAndClamp() == 0u, "compactAndClamp early-outs on empty buffer");

    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.penetrationDepth = 0.4f;
    manifold.addPoint({0.f, 0.f, 0.f}, 0.4f);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(1u, manifold);

    const auto noWorkPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_compact_and_clamp(buffer);
    expectTrue(
        noWorkPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::None,
        "prepared valid slots need compact-and-clamp work");
    expectTrue(buffer.compactAndClamp() == 2u, "compactAndClamp gathers two valid contacts");

    const auto afterPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_compact_and_clamp(buffer);
    expectTrue(
        afterPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork,
        "already-compacted buffer reports NoWork compact-and-clamp reject reason");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_buffer_compact_and_clamp_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork),
            "NoWork") == 0,
        "NoWork compact-and-clamp reject reason has stable label");
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_compact_and_clamp_rejects_for_reason(
            buffer,
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork),
        "already-compacted buffer rejects for NoWork");
    expectTrue(buffer.compactAndClamp() == 2u, "compactAndClamp no-op preserves active count");
    expectTrue(buffer.hasValidContacts(), "compacted buffer reports valid contacts");
}

void testContactBufferPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.penetrationDepth = 0.25f;
    valid.addPoint({0.f, 0.f, 0.f}, 0.25f);

    const auto validWrite =
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 0u, valid);
    expectTrue(validWrite.can_write(), "write preflight accepts valid manifold slot");

    fuse::physics::narrowphase::ContactManifold invalid = valid;
    invalid.valid = false;
    const auto invalidWrite =
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 0u, invalid);
    expectTrue(invalidWrite.invalidManifold, "write preflight marks invalid manifold");
    expectTrue(!invalidWrite.can_write(), "write preflight rejects invalid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    const auto selfWrite =
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 0u, selfPair);
    expectTrue(selfWrite.selfPair, "write preflight marks self pair");
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_rejects_for_reason(
            buffer, 0u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write rejects_for_reason matches self pair");

    const auto outOfRangeWrite =
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 4u, valid);
    expectTrue(outOfRangeWrite.outOfRange, "write preflight marks out-of-range slot");

    buffer.writeSlot(0u, valid);
    const auto needsCompaction =
        fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(needsCompaction.needs_compaction(), "compaction preflight requests work with sparse valid slots");

    fuse::physics::narrowphase::ContactManifold second = valid;
    second.bodyB = 2u;
    buffer.writeSlot(1u, second);
    const auto skipCompaction =
        fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(!skipCompaction.needs_compaction(), "compaction preflight skips when all slots are valid");

    fuse::physics::narrowphase::ContactBufferSoA clampBuffer;
    clampBuffer.setMaxCapacity(1u);
    clampBuffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold shallow = valid;
    shallow.penetrationDepth = 0.1f;
    shallow.points[0].penetration = 0.1f;
    fuse::physics::narrowphase::ContactManifold deep = valid;
    deep.bodyB = 2u;
    deep.penetrationDepth = 0.9f;
    deep.points[0].penetration = 0.9f;
    clampBuffer.writeSlot(0u, shallow);
    clampBuffer.writeSlot(1u, deep);
    const auto clampPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(clampBuffer);
    expectTrue(clampPreflight.needs_clamp(), "clamp preflight requests overflow truncation");

    fuse::physics::narrowphase::ContactBufferSoA withinBuffer;
    withinBuffer.setMaxCapacity(2u);
    withinBuffer.preparePairSlots(1u);
    withinBuffer.writeSlot(0u, valid);
    const auto withinPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(withinBuffer);
    expectTrue(!withinPreflight.needs_clamp(), "clamp preflight skips when within capacity");

    const auto compactAndClampPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_compact_and_clamp(clampBuffer);
    expectTrue(compactAndClampPreflight.needs_compact_and_clamp(),
        "compact-and-clamp preflight requests work when clamp needed");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_buffer_compact_and_clamp_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer),
            "EmptyBuffer") == 0,
        "compact-and-clamp reject reason name resolves EmptyBuffer");
}

void testContactBufferGuardedWriteAndCompact() {
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

    fuse::physics::narrowphase::write_contact_buffer_slot_with_preflight(buffer, 0u, shallow);
    fuse::physics::narrowphase::write_contact_buffer_slot_with_preflight(buffer, 1u, deep);
    fuse::physics::narrowphase::write_contact_buffer_slot_with_preflight(buffer, 2u, medium);
    fuse::physics::narrowphase::write_contact_buffer_slot_with_preflight(buffer, 3u, selfPair);

    expectTrue(
        fuse::physics::narrowphase::compact_and_clamp_contact_buffer_with_preflight(buffer) == 2u,
        "guarded compact-and-clamp ignores self pair and truncates to max capacity");
    expectTrue(buffer.droppedCount == 1u, "guarded compact-and-clamp tracks dropped contacts");
    expectNear(buffer.manifoldAt(0u).penetrationDepth, 0.9f, 1e-4f, "guarded clamp keeps deepest penetration");
}

void testContactPairDeepenDispatchGuards() {
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

    const auto sleepingManifold = fuse::physics::narrowphase::detect_contacts_pair_with_deepen_preflight(
        {sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingManifold.valid, "deepen dispatch rejects both-sleeping pair");

    const auto validManifold = fuse::physics::narrowphase::detect_contacts_pair_with_deepen_preflight(
        {dynamicA, dynamicB}, bodies, shapes);
    expectTrue(validManifold.valid, "deepen dispatch allows valid overlapping pair");

    fuse::physics::narrowphase::ContactManifold ready = validManifold;
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_with_deepen_preflight(ready),
        "deepen finalize guard finalizes valid manifold");
    expectTrue(ready.hasFrictionBasis(), "deepen finalize guard builds friction basis");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {dynamicA, dynamicB},
    };
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBufferWithDeepenPreflight(mixedPairs, bodies, shapes, buffer);
    expectTrue(buffer.activeCount == 1u, "deepen narrowphase buffer keeps one dispatchable contact");
}

void testManifoldPruneFinalizeDeepenGuards() {
    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);

    expectTrue(
        fuse::physics::narrowphase::prune_and_finalize_contact_manifold_with_preflight(dirty),
        "prune-and-finalize keeps penetrating slots");
    expectTrue(dirty.valid, "prune-and-finalize sets validity");
    expectTrue(dirty.pointCount == 1u, "prune-and-finalize removes separated slot");
    expectTrue(dirty.hasFrictionBasis(), "prune-and-finalize builds friction basis");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        !fuse::physics::narrowphase::prune_and_finalize_contact_manifold_with_preflight(separated),
        "prune-and-finalize rejects all-separated manifold");
    expectTrue(separated.empty(), "prune-and-finalize clears separated manifold");
}

void testFrictionBasisDeepenGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_manifold(
            empty, fuse::physics::narrowphase::FrictionBasisRejectReason::EmptyManifold),
        "friction rejects_for_manifold flags empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::compute_friction_tangents_with_preflight(needsBuild),
        "compute_friction_tangents_with_preflight builds missing basis");
    expectTrue(needsBuild.hasFrictionBasis(), "compute_friction_tangents_with_preflight stores basis");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        !fuse::physics::narrowphase::compute_friction_tangents_with_preflight(noNormal),
        "compute_friction_tangents_with_preflight skips invalid normal");
}

void testContactBufferWritePreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    const auto validPreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, valid);
    expectTrue(validPreflight.canWrite(), "write preflight allows valid manifold");
    expectTrue(
        fuse::physics::narrowphase::writeContactBufferSlotWithPreflight(buffer, 0u, valid),
        "write with preflight stores valid slot");

    fuse::physics::narrowphase::ContactManifold invalid = valid;
    invalid.valid = false;
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 1u, invalid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "write reject reason flags invalid manifold");
    expectTrue(
        !fuse::physics::narrowphase::writeContactBufferSlotWithPreflight(buffer, 1u, invalid),
        "write with preflight skips invalid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 1u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write reject reason flags self pair");

    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 9u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
        "write reject reason flags out-of-range slot");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
            "SelfPair") == 0,
        "write reject reason name resolves SelfPair");
}

void testContactBufferCompactionPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(empty);
    expectTrue(emptyPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer,
        "compaction preflight flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompaction(empty),
        "can skip compaction on empty buffer");

    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(1u, manifold);
    const auto allValidPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(allValidPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid,
        "compaction preflight flags all-valid slots");
    expectTrue(
        fuse::physics::narrowphase::compactContactBufferWithPreflight(buffer) == 2u,
        "compaction with preflight keeps all-valid count");

    fuse::physics::narrowphase::ContactBufferSoA sparse;
    sparse.preparePairSlots(3u);
    sparse.writeSlot(0u, manifold);
    sparse.writeSlot(2u, manifold);
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferCompaction(sparse),
        "should run compaction when sparse valid flags exist");
    expectTrue(
        fuse::physics::narrowphase::compactContactBufferWithPreflight(sparse) == 2u,
        "compaction with preflight packs sparse slots");
    expectTrue(sparse.countValidSlots() == 2u, "countValidSlots matches compacted contacts");
}

void testContactBufferClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflightContactBufferClamp(empty);
    expectTrue(emptyPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferClampRejectReason::EmptyBuffer,
        "clamp preflight flags empty buffer");

    fuse::physics::narrowphase::ContactBufferSoA within{};
    within.preparePairSlots(1u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    within.writeSlot(0u, manifold);
    within.compact();
    within.setMaxCapacity(4u);
    const auto withinPreflight = fuse::physics::narrowphase::preflightContactBufferClamp(within);
    expectTrue(withinPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity,
        "clamp preflight flags within-capacity buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferClamp(within),
        "can skip clamp within capacity");

    fuse::physics::narrowphase::ContactBufferSoA over{};
    over.setMaxCapacity(1u);
    over.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold shallow = manifold;
    shallow.penetrationDepth = 0.1f;
    shallow.points[0].penetration = 0.1f;
    fuse::physics::narrowphase::ContactManifold deep = manifold;
    deep.bodyB = 2u;
    deep.penetrationDepth = 0.9f;
    deep.points[0].penetration = 0.9f;
    over.writeSlot(0u, shallow);
    over.writeSlot(1u, deep);
    over.compact();
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferClamp(over),
        "should run clamp when active count exceeds max capacity");
    expectTrue(
        fuse::physics::narrowphase::clampContactBufferWithPreflight(over) == 1u,
        "clamp with preflight truncates to max capacity");
    expectNear(over.manifoldAt(0u).penetrationDepth, 0.9f, 1e-4f, "clamp with preflight keeps deepest contact");
}

void testContactBufferCompactAndClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA empty{};
    const auto emptyPreflight = fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(empty);
    expectTrue(emptyPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer,
        "compact-and-clamp preflight flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::compactAndClampContactBufferWithPreflight(empty) == 0u,
        "compact-and-clamp with preflight clears empty buffer");

    fuse::physics::narrowphase::ContactBufferSoA clean{};
    clean.preparePairSlots(1u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.25f);
    clean.writeSlot(0u, manifold);
    clean.compact();
    const auto noWorkPreflight = fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(clean);
    expectTrue(noWorkPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork,
        "compact-and-clamp preflight flags no-work buffer");
    expectTrue(
        fuse::physics::narrowphase::should_skip_narrowphase_buffer_pass(clean),
        "narrowphase buffer pass can skip clean compacted buffer");
    expectTrue(
        fuse::physics::narrowphase::compactAndClampContactBufferWithPreflight(clean) == 1u,
        "compact-and-clamp with preflight preserves clean buffer");
}

void testContactPairUnionPreflightGuards() {
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

    const auto validUnion =
        fuse::physics::narrowphase::preflight_contact_pair_union({dynamicA, dynamicB}, bodies, shapes);
    expectTrue(validUnion.can_dispatch(), "union preflight allows valid pair");
    expectTrue(
        validUnion.reason == fuse::physics::narrowphase::ContactPairRejectReason::None,
        "union preflight reports None for valid pair");

    const auto sleepingUnion =
        fuse::physics::narrowphase::preflight_contact_pair_union({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingUnion.can_dispatch(), "union preflight rejects both-sleeping pair");
    expectTrue(
        sleepingUnion.reason == fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping,
        "union preflight reports BothSleeping");
    expectTrue(
        sleepingUnion.baseReason == fuse::physics::narrowphase::ContactPairRejectReason::None,
        "union preflight base reason unchanged for sleeping pair");

    const fuse::u32 capsuleA = bodies.addBody({4.f, 0.f, 0.f}, 1.f);
    const fuse::u32 capsuleB = bodies.addBody({5.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Capsule, capsuleA, {0.5f, 1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Capsule, capsuleB, {0.5f, 1.f, 0.f});
    expectTrue(
        fuse::physics::narrowphase::is_capsule_capsule_contact_pair({capsuleA, capsuleB}, shapes),
        "capsule-capsule guard flags both-capsule pair");
    const auto capsulePair =
        fuse::physics::narrowphase::detect_contacts_pair({capsuleA, capsuleB}, bodies, shapes);
    expectTrue(!capsulePair.valid, "capsule-capsule pair has no dispatch path");

    const fuse::u32 boxBody = bodies.addBody({6.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Box, boxBody, {1.f, 1.f, 1.f});
    expectTrue(
        fuse::physics::narrowphase::is_box_capsule_contact_pair({boxBody, capsuleA}, shapes),
        "box-capsule guard flags mixed box/capsule pair");
}

void testFinalizeAndFrictionPreflightIfNeededGuards() {
    fuse::physics::narrowphase::ContactManifold finalized =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::finalize_contact_manifold_with_preflight(finalized),
        "finalize with preflight succeeds on valid manifold");
    const auto cachedTangent1 = finalized.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::finalize_contact_manifold_if_needed(finalized),
        "finalize if needed reuses finalized manifold");
    expectNear(
        finalized.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "finalize if needed preserves cached friction basis");

    fuse::physics::narrowphase::ContactManifold needsBasis{};
    needsBasis.contactNormal = {0.f, 1.f, 0.f};
    needsBasis.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::compute_friction_tangents_with_preflight(needsBasis),
        "compute friction with preflight builds basis");
    expectTrue(needsBasis.hasFrictionBasis(), "compute friction with preflight stores orthonormal basis");
}

void testFrictionBasisFollowUpRejectGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            empty, fuse::physics::narrowphase::FrictionBasisRejectReason::EmptyManifold),
        "frictionBasisRejectsForReason matches empty manifold");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            noNormal, fuse::physics::narrowphase::FrictionBasisRejectReason::InvalidNormal),
        "frictionBasisRejectsForReason matches invalid normal");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            needsBuild, fuse::physics::narrowphase::FrictionBasisRejectReason::MissingBasis),
        "frictionBasisRejectsForReason matches missing basis");
    const auto needsPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(
        needsPreflight.reason == fuse::physics::narrowphase::FrictionBasisRejectReason::MissingBasis,
        "friction preflight reports MissingBasis");

    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_preflight_dispatch(needsBuild),
        "rebuild preflight dispatch builds missing basis");
    expectTrue(needsBuild.hasFrictionBasis(), "rebuild preflight dispatch stores orthonormal basis");

    fuse::physics::narrowphase::ContactManifold stale = needsBuild;
    stale.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            stale, fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis),
        "frictionBasisRejectsForReason matches stale basis");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::friction_basis_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis),
            "StaleBasis") == 0,
        "friction basis reject reason name resolves StaleBasis");
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_preflight_dispatch(stale),
        "rebuild preflight dispatch refreshes stale basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(stale),
        "rebuild preflight dispatch matches current normal");
}

void testNarrowphasePairBatchPreflightGuards() {
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

    const std::vector<fuse::physics::broadphase::CandidatePair> mixed = {
        {sleepingA, sleepingB},
        {bodyA, bodyB},
    };
    const auto stats = fuse::physics::narrowphase::compute_narrowphase_pair_stats(mixed, bodies, shapes);
    expectTrue(stats.totalPairs == 2u, "batch stats counts total pairs");
    expectTrue(stats.dispatchablePairs == 1u, "batch stats counts dispatchable pairs");
    expectTrue(stats.rejectedPairs == 1u, "batch stats counts rejected pairs");
    expectTrue(
        fuse::physics::narrowphase::count_dispatchable_contact_pairs(mixed, bodies, shapes) == 1u,
        "dispatchable count matches stats");

    const auto batchPreflight = fuse::physics::narrowphase::preflight_narrowphase_pairs(mixed, bodies, shapes);
    expectTrue(batchPreflight.hasDispatchable, "batch preflight has dispatchable pair");
    expectTrue(!batchPreflight.can_skip_batch(), "batch preflight cannot skip mixed list");
    expectTrue(batchPreflight.can_dispatch_any(), "batch preflight can dispatch any");

    const std::vector<fuse::physics::broadphase::CandidatePair> allRejected = {{sleepingA, sleepingB}};
    const auto rejectedPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_pairs(allRejected, bodies, shapes);
    expectTrue(rejectedPreflight.can_skip_batch(), "batch preflight skips all-rejected list");
    expectTrue(!rejectedPreflight.can_dispatch_any(), "batch preflight cannot dispatch all-rejected list");

    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {sleepingA, sleepingB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejects_for_reason matches sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {bodyA, bodyB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejects_for_reason does not false-positive valid pair");

    const auto sleepingDetect =
        fuse::physics::narrowphase::detect_contacts_pair_if_needed({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingDetect.valid, "detect_if_needed rejects sleeping pair");
    const auto validDetect =
        fuse::physics::narrowphase::detect_contacts_pair_if_needed({bodyA, bodyB}, bodies, shapes);
    expectTrue(validDetect.valid, "detect_if_needed dispatches valid pair");
}

void testManifoldPruneFinalizeDeepenGuards() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_prune(clean),
        "can_skip_manifold_prune true for clean manifold");
    expectTrue(
        fuse::physics::narrowphase::prune_manifold_if_needed(clean),
        "prune_manifold_if_needed preserves clean manifold");
    expectTrue(clean.pointCount == 1u, "prune_manifold_if_needed leaves clean slots untouched");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        !fuse::physics::narrowphase::can_skip_manifold_prune(dirty),
        "can_skip_manifold_prune false when separated slots exist");
    expectTrue(
        fuse::physics::narrowphase::prune_manifold_if_needed(dirty),
        "prune_manifold_if_needed keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "prune_manifold_if_needed removes separated slot");

    const auto dirtyPreflight = fuse::physics::narrowphase::preflight_manifold_prune(dirty, 1e-6f, 1e-4f, 0.05f);
    expectTrue(!dirtyPreflight.needs_any_pruning(0.05f), "prune preflight clean after conditional prune");

    fuse::physics::narrowphase::ContactManifold shallow{};
    shallow.contactNormal = {0.f, 1.f, 0.f};
    shallow.addPoint({0.f, 0.f, 0.f}, 0.5f);
    shallow.addPoint({1.f, 0.f, 0.f}, 0.01f);
    expectTrue(
        fuse::physics::narrowphase::prune_manifold_if_needed(shallow, 1e-6f, 1e-4f, 0.05f),
        "prune_manifold_if_needed handles shallow slots");
    expectTrue(shallow.pointCount == 1u, "prune_manifold_if_needed removes shallow slot");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.valid = true;
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        !fuse::physics::narrowphase::finalize_contact_manifold_if_needed(separated),
        "finalize_if_needed no-ops on separated manifold");
    expectTrue(separated.valid, "finalize_if_needed leaves separated validity unchanged");

    fuse::physics::narrowphase::ContactManifold manifold =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    const auto finalizePreflight = fuse::physics::narrowphase::preflight_manifold_finalize(manifold);
    expectTrue(finalizePreflight.needs_any_work(), "finalize preflight needs work before finalize");
    expectTrue(
        fuse::physics::narrowphase::finalize_contact_manifold_if_needed(manifold),
        "finalize_if_needed finalizes valid manifold");
    expectTrue(manifold.valid, "finalize_if_needed sets validity on success");
    expectTrue(manifold.hasFrictionBasis(), "finalize_if_needed builds friction basis");
}

void testFrictionBasisDeepenPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::can_run_friction_basis_rebuild(empty),
        "can_run_friction_basis_rebuild false for empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::ensure_friction_basis_if_needed(empty),
        "ensure_friction_basis_if_needed false for empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto needsPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(needsPreflight.can_rebuild(), "friction preflight can_rebuild without cached basis");
    expectTrue(
        fuse::physics::narrowphase::can_run_friction_basis_rebuild(needsBuild),
        "can_run_friction_basis_rebuild true without cached basis");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_if_needed(needsBuild),
        "ensure_friction_basis_if_needed builds missing basis");
    expectTrue(needsBuild.hasFrictionBasis(), "ensure_friction_basis_if_needed stores orthonormal basis");

    const auto cachedTangent1 = needsBuild.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_if_needed(needsBuild),
        "ensure_friction_basis_if_needed reuses valid basis");
    expectNear(
        needsBuild.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "ensure_friction_basis_if_needed preserves cached tangent1");

    needsBuild.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::can_run_friction_basis_rebuild(needsBuild),
        "can_run_friction_basis_rebuild true for stale basis");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_if_needed(needsBuild),
        "ensure_friction_basis_if_needed rebuilds stale basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(needsBuild),
        "ensure_friction_basis_if_needed matches current normal after rebuild");
}

void testNarrowphaseRejectReasonGuards() {
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
        fuse::physics::narrowphase::narrowphase_rejects_for_reason(
            {}, bodies, shapes, fuse::physics::narrowphase::NarrowphaseRejectReason::EmptyPairList),
        "narrowphaseRejectsForReason matches empty pair list");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::narrowphase_reject_reason_name(
                fuse::physics::narrowphase::NarrowphaseRejectReason::AllPairsRejected),
            "AllPairsRejected") == 0,
        "AllPairsRejected narrowphase reject reason has stable label");

    const std::vector<fuse::physics::broadphase::CandidatePair> allRejected = {{sleepingA, sleepingB}};
    expectTrue(
        fuse::physics::narrowphase::narrowphase_rejects_for_reason(
            allRejected, bodies, shapes, fuse::physics::narrowphase::NarrowphaseRejectReason::AllPairsRejected),
        "narrowphaseRejectsForReason matches all-rejected pair list");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixed = {{sleepingA, sleepingB}, {bodyA, bodyB}};
    expectTrue(
        fuse::physics::narrowphase::narrowphase_rejects_for_reason(
            mixed, bodies, shapes, fuse::physics::narrowphase::NarrowphaseRejectReason::None),
        "mixed pair list reports None narrowphase reject reason");

    const fuse::physics::narrowphase::NarrowphasePreflight preflight =
        fuse::physics::narrowphase::preflight_narrowphase(mixed, bodies, shapes);
    expectTrue(preflight.can_dispatch(), "narrowphase preflight can dispatch mixed pair list");
    expectTrue(preflight.totalPairs == 2u, "narrowphase preflight counts total pairs");
    expectTrue(preflight.dispatchablePairs == 1u, "narrowphase preflight counts dispatchable pairs");
    expectTrue(preflight.rejectedPairs == 1u, "narrowphase preflight counts rejected pairs");
    expectTrue(
        fuse::physics::narrowphase::can_skip_narrowphase_preflight(allRejected, bodies, shapes),
        "can_skip_narrowphase_preflight on all-rejected list");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_narrowphase_preflight(mixed, bodies, shapes),
        "can_skip_narrowphase_preflight false when dispatchable pair exists");
}

void testContactPairDeepenRejectsForReasonGuards() {
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
        "deepenRejectsForReason matches both-sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {dynamicA, dynamicB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepenRejectsForReason does not false-positive valid pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {
        {sleepingA, sleepingB},
        {dynamicA, dynamicB},
    };
    expectTrue(
        fuse::physics::narrowphase::count_dispatchable_contact_pairs(pairs, bodies, shapes) == 1u,
        "count_dispatchable_contact_pairs counts valid pair");
    expectTrue(
        fuse::physics::narrowphase::count_rejected_contact_pairs_deepen(pairs, bodies, shapes) == 1u,
        "count_rejected_contact_pairs_deepen counts rejected pair");
}

void testManifoldPruneRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold),
        "manifoldPruneRejectsForReason matches empty manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_prune_reject_reason_name(
                fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmptyAfterPrune),
            "WouldBeEmptyAfterPrune") == 0,
        "WouldBeEmptyAfterPrune prune reject reason has stable label");
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_prune(empty),
        "can_skip_manifold_prune on empty manifold");

    fuse::physics::narrowphase::ContactManifold allSeparated{};
    allSeparated.contactNormal = {0.f, 1.f, 0.f};
    allSeparated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            allSeparated, fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmptyAfterPrune),
        "manifoldPruneRejectsForReason matches all-separated manifold");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::None),
        "clean manifold reports None prune reject reason");
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_prune(clean),
        "can_skip_manifold_prune on clean manifold");
}

void testManifoldFinalizeRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold),
        "manifoldFinalizeRejectsForReason matches empty manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_finalize_reject_reason_name(
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal),
            "InvalidNormal") == 0,
        "InvalidNormal finalize reject reason has stable label");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            noNormal, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal),
        "manifoldFinalizeRejectsForReason matches invalid normal");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            separated, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune),
        "manifoldFinalizeRejectsForReason matches would-be-empty-after-prune");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            ready, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None),
        "ready manifold reports None finalize reject reason");
}

void testFrictionBasisRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            empty, fuse::physics::narrowphase::FrictionBasisRejectReason::EmptyManifold),
        "frictionBasisRejectsForReason matches empty manifold");
    expectTrue(
        fuse::physics::narrowphase::can_skip_friction_basis_preflight(empty),
        "can_skip_friction_basis_preflight on empty manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::friction_basis_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis),
            "StaleBasis") == 0,
        "StaleBasis friction reject reason has stable label");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            noNormal, fuse::physics::narrowphase::FrictionBasisRejectReason::InvalidNormal),
        "frictionBasisRejectsForReason matches invalid normal");

    fuse::physics::narrowphase::ContactManifold stale{};
    stale.contactNormal = {0.f, 1.f, 0.f};
    stale.addPoint({0.f, 0.f, 0.f}, 0.2f);
    stale.buildFrictionBasis();
    stale.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            stale, fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis),
        "frictionBasisRejectsForReason matches stale basis");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_friction_basis_preflight(stale),
        "can_skip_friction_basis_preflight false for stale basis");

    fuse::physics::narrowphase::ContactManifold fresh{};
    fresh.contactNormal = {0.f, 1.f, 0.f};
    fresh.addPoint({0.f, 0.f, 0.f}, 0.2f);
    fresh.buildFrictionBasis();
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            fresh, fuse::physics::narrowphase::FrictionBasisRejectReason::None),
        "fresh basis reports None friction reject reason");
    expectTrue(
        fuse::physics::narrowphase::can_skip_friction_basis_preflight(fresh),
        "can_skip_friction_basis_preflight on valid cached basis");
}

void testContactPairDeepenFollowUpGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 negativeMass = bodies.addBody({2.f, 0.f, 0.f}, -1.f);
    const fuse::u32 zeroMassA = bodies.addBody({3.f, 0.f, 0.f}, 0.f);
    const fuse::u32 zeroMassB = bodies.addBody({4.f, 0.f, 0.f}, 0.f);
    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 kinematicB = bodies.addBody({0.f, 3.f, 0.f}, 0.f, fuse::physics::RB_KINEMATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, negativeMass, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, zeroMassA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, zeroMassB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, kinematicB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::is_negative_inverse_mass_pair({dynamicA, negativeMass}, bodies),
        "negative-mass guard flags invalid inverse mass");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({dynamicA, negativeMass}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::NegativeInverseMass,
        "deepen reject reason flags negative inverse mass");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {dynamicA, negativeMass},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::NegativeInverseMass),
        "deepen rejects_for_reason matches negative inverse mass");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({dynamicA, negativeMass}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::None,
        "base reject reason unchanged for negative inverse mass");

    expectTrue(
        fuse::physics::narrowphase::is_zero_mass_contact_pair({zeroMassA, zeroMassB}, bodies),
        "zero-mass guard flags both non-positive inverse mass");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({zeroMassA, zeroMassB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothZeroMass,
        "deepen reject reason flags both-zero-mass pair");
    expectTrue(
        fuse::physics::narrowphase::is_invalid_contact_pair_deepen({zeroMassA, zeroMassB}, bodies, shapes),
        "is_invalid_contact_pair_deepen rejects both-zero-mass pair");
    expectTrue(
        fuse::physics::narrowphase::is_valid_contact_pair_deepen({dynamicA, dynamicB}, bodies, shapes),
        "is_valid_contact_pair_deepen allows dynamic pair");

    expectTrue(
        fuse::physics::narrowphase::is_sleeping_kinematic_mix_pair({sleepingA, kinematicB}, bodies),
        "sleeping-kinematic mix guard detects mixed pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({sleepingA, kinematicB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::SleepingKinematicMix,
        "deepen reject reason flags sleeping-kinematic mix");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::SleepingKinematicMix),
            "SleepingKinematicMix") == 0,
        "reject reason name resolves SleepingKinematicMix");
}

void testManifoldFinalizeRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_reject_reason(empty) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::Empty,
        "finalize reject reason flags empty manifold");
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::Empty),
        "finalize rejects_for_reason matches empty manifold");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_reject_reason(noNormal) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal,
        "finalize reject reason flags invalid normal");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_reject_reason(separated) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyAfterPrune,
        "finalize reject reason flags empty-after-prune");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_reject_reason(ready) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
        "finalize reject reason None for ready manifold");

    const auto readyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(
        readyPreflight.rejectReason == fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
        "finalize preflight reports None reject reason for ready manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_finalize_reject_reason_name(
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyAfterPrune),
            "EmptyAfterPrune") == 0,
        "finalize reject reason name resolves EmptyAfterPrune");
}

void testManifoldPruneDispatchGuards() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        fuse::physics::narrowphase::should_skip_manifold_prune(clean),
        "should_skip_manifold_prune true for clean manifold");
    expectTrue(
        !fuse::physics::narrowphase::can_prune_manifold_in_place(clean),
        "can_prune_manifold_in_place false when pruning not needed");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        !fuse::physics::narrowphase::should_skip_manifold_prune(dirty),
        "should_skip_manifold_prune false when separated slots exist");
    expectTrue(
        fuse::physics::narrowphase::can_prune_manifold_in_place(dirty),
        "can_prune_manifold_in_place true when penetrating slots remain");

    fuse::physics::narrowphase::ContactManifold allSeparated{};
    allSeparated.contactNormal = {0.f, 1.f, 0.f};
    allSeparated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        !fuse::physics::narrowphase::can_prune_manifold_in_place(allSeparated),
        "can_prune_manifold_in_place false when prune would empty manifold");
}

void testFrictionBasisRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_reject_reason(empty) ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::Skipped,
        "friction reject reason Skipped for empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::can_dispatch_friction_basis_rebuild(empty),
        "can_dispatch_friction_basis_rebuild false for empty manifold");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::friction_basis_reject_reason(noNormal) ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::MissingNormal,
        "friction reject reason MissingNormal without valid normal");
    expectTrue(
        !fuse::physics::narrowphase::can_dispatch_friction_basis_rebuild(noNormal),
        "can_dispatch_friction_basis_rebuild false without valid normal");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::friction_basis_reject_reason(needsBuild) ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::None,
        "friction reject reason None when basis may be built");
    expectTrue(
        fuse::physics::narrowphase::can_dispatch_friction_basis_rebuild(needsBuild),
        "can_dispatch_friction_basis_rebuild true for valid manifold");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            needsBuild, fuse::physics::narrowphase::FrictionBasisRejectReason::None),
        "friction rejects_for_reason matches dispatchable manifold");

    needsBuild.buildFrictionBasis();
    needsBuild.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_reject_reason(needsBuild) ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis,
        "friction reject reason StaleBasis for rotated normal");
    expectTrue(
        fuse::physics::narrowphase::can_dispatch_friction_basis_rebuild(needsBuild),
        "can_dispatch_friction_basis_rebuild true for stale basis refresh");

    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(
        stalePreflight.rejectReason == fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis,
        "friction preflight reports StaleBasis reject reason");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::friction_basis_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis),
            "StaleBasis") == 0,
        "friction reject reason name resolves StaleBasis");
}

void testContactPairDeepenRejectsForReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {sleepingA, sleepingB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejects_for_reason flags both-sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {dynamicA, sleepingA},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejects_for_reason does not false-positive mixed pair");
}

void testNarrowphaseBatchPreflightGuards() {
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
        fuse::physics::narrowphase::narrowphase_rejects_for_reason(
            {},
            bodies,
            shapes,
            fuse::physics::narrowphase::NarrowphaseRejectReason::EmptyPairList),
        "narrowphase reject reason flags empty pair list");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::narrowphase_reject_reason_name(
                fuse::physics::narrowphase::NarrowphaseRejectReason::AllPairsRejected),
            "AllPairsRejected") == 0,
        "narrowphase reject reason name resolves AllPairsRejected");

    const auto allRejectedPreflight = fuse::physics::narrowphase::preflight_narrowphase_batch(
        {{sleepingA, sleepingB}}, bodies, shapes);
    expectTrue(!allRejectedPreflight.can_dispatch(), "batch preflight rejects all-sleeping list");
    expectTrue(
        allRejectedPreflight.reason ==
            fuse::physics::narrowphase::NarrowphaseRejectReason::AllPairsRejected,
        "batch preflight reports AllPairsRejected");
    expectTrue(
        fuse::physics::narrowphase::can_skip_narrowphase_dispatch({{sleepingA, sleepingB}}, bodies, shapes),
        "can_skip_narrowphase_dispatch on all-rejected list");

    const auto mixedPreflight = fuse::physics::narrowphase::preflight_narrowphase_batch(
        {{sleepingA, sleepingB}, {bodyA, bodyB}}, bodies, shapes);
    expectTrue(mixedPreflight.can_dispatch(), "batch preflight allows mixed list");
    expectTrue(mixedPreflight.dispatchableCount == 1u, "batch preflight counts one dispatchable pair");
    expectTrue(mixedPreflight.rejectedCount == 1u, "batch preflight counts one rejected pair");
}

void testDetectContactsPairIfNeededGuard() {
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

    const auto rejected =
        fuse::physics::narrowphase::detect_contacts_pair_if_needed({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!rejected.valid, "detect_if_needed returns invalid for deepen-rejected pair");
    expectTrue(rejected.empty(), "detect_if_needed has no points for deepen-rejected pair");

    const auto overlap =
        fuse::physics::narrowphase::detect_contacts_pair_if_needed({bodyA, bodyB}, bodies, shapes);
    expectTrue(overlap.valid, "detect_if_needed detects valid overlapping pair");
    expectTrue(overlap.pointCount > 0u, "detect_if_needed populates contact points");
}

void testManifoldFinalizeRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold),
        "finalize reject reason flags empty manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_finalize_reject_reason_name(
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::NoPenetration),
            "NoPenetration") == 0,
        "finalize reject reason name resolves NoPenetration");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_reject_reason(noNormal) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal,
        "finalize reject reason flags zero-length normal");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    const auto separatedPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(separated);
    expectTrue(
        separatedPreflight.reason ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyAfterPrune,
        "finalize preflight reason flags empty-after-prune");
    expectTrue(!separatedPreflight.can_finalize(), "finalize preflight cannot finalize separated manifold");
}

void testManifoldPruneDispatchGuards() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.5f);
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_prune_dispatch(clean),
        "prune dispatch skips clean manifold");

    fuse::physics::narrowphase::ContactManifold shallow{};
    shallow.contactNormal = {0.f, 1.f, 0.f};
    shallow.addPoint({0.f, 0.f, 0.f}, 0.5f);
    shallow.addPoint({1.f, 0.f, 0.f}, 0.01f);
    const auto shallowPreflight =
        fuse::physics::narrowphase::preflight_manifold_prune_dispatch(shallow, 1e-6f, 1e-4f, 0.05f);
    expectTrue(shallowPreflight.needsShallowPrune, "prune dispatch preflight flags shallow slot");
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(shallow, 1e-6f, 1e-4f, 0.05f),
        "prune if needed keeps deep point");
    expectTrue(shallow.pointCount == 1u, "prune if needed removes shallow slot");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    dirty.addPoint({0.00001f, 0.f, 0.f}, 0.35f);
    expectTrue(
        !fuse::physics::narrowphase::can_skip_manifold_prune_dispatch(dirty),
        "prune dispatch does not skip dirty manifold");
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(dirty),
        "prune if needed keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "prune if needed merges separated and duplicate slots");
}

void testFrictionBasisRejectAndPreflightDispatchGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            empty, fuse::physics::narrowphase::FrictionBasisRejectReason::EmptyManifold),
        "friction reject reason flags empty manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::friction_basis_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRejectReason::InvalidNormal),
            "InvalidNormal") == 0,
        "friction reject reason name resolves InvalidNormal");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto noNormalPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(noNormal);
    expectTrue(
        noNormalPreflight.reason ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::InvalidNormal,
        "friction preflight reason flags invalid normal");
    expectTrue(
        !fuse::physics::narrowphase::rebuild_friction_basis_with_preflight(noNormal),
        "rebuild with preflight returns false for invalid normal");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_with_preflight(needsBuild),
        "rebuild with preflight builds missing basis");
    fuse::physics::narrowphase::compute_friction_tangents_with_preflight(needsBuild);
    expectTrue(needsBuild.hasFrictionBasis(), "compute with preflight stores orthonormal basis");

    const auto cachedTangent1 = needsBuild.frictionBasis.tangent1;
    fuse::physics::narrowphase::compute_friction_tangents_with_preflight(needsBuild);
    expectNear(
        needsBuild.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "compute with preflight preserves valid cached basis");
}

void testNarrowphaseRejectReasonGuards() {
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
        fuse::physics::narrowphase::narrowphase_rejects_for_reason(
            {}, bodies, shapes, fuse::physics::narrowphase::NarrowphaseRejectReason::EmptyPairList),
        "empty pair list rejects for EmptyPairList");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::narrowphase_reject_reason_name(
                fuse::physics::narrowphase::NarrowphaseRejectReason::AllPairsRejected),
            "AllPairsRejected") == 0,
        "AllPairsRejected narrowphase reject reason has stable label");
    expectTrue(
        fuse::physics::narrowphase::narrowphase_rejects_for_reason(
            {{sleepingA, sleepingB}}, bodies, shapes,
            fuse::physics::narrowphase::NarrowphaseRejectReason::AllPairsRejected),
        "all deepen-rejected pairs reject for AllPairsRejected");
    expectTrue(
        fuse::physics::narrowphase::narrowphase_rejects_for_reason(
            {{bodyA, bodyB}}, bodies, shapes, fuse::physics::narrowphase::NarrowphaseRejectReason::None),
        "dispatchable pair reports None narrowphase reject reason");
    expectTrue(
        fuse::physics::narrowphase::can_skip_narrowphase({{sleepingA, sleepingB}}, bodies, shapes),
        "can_skip_narrowphase when all pairs deepen-rejected");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_narrowphase({{bodyA, bodyB}}, bodies, shapes),
        "can_skip_narrowphase false when dispatchable pair exists");
}

void testContactPairDeepenRejectsForReasonGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
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
            {dynamicA, sleepingA},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejects_for_reason does not false-positive mixed pair");
}

void testNarrowphasePairListPreflightGuards() {
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

    const auto emptyPreflight = fuse::physics::narrowphase::preflight_narrowphase_pair_list({}, bodies, shapes);
    expectTrue(!emptyPreflight.can_dispatch(), "empty pair-list preflight cannot dispatch");
    expectTrue(emptyPreflight.emptyPairList, "empty pair-list preflight marks empty list");

    const auto rejectedPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_pair_list({{sleepingA, sleepingB}}, bodies, shapes);
    expectTrue(!rejectedPreflight.can_dispatch(), "all-rejected pair-list preflight cannot dispatch");
    expectTrue(rejectedPreflight.allPairsRejected, "all-rejected pair-list preflight marks all rejected");

    const auto mixedPreflight = fuse::physics::narrowphase::preflight_narrowphase_pair_list(
        {{sleepingA, sleepingB}, {bodyA, bodyB}}, bodies, shapes);
    expectTrue(mixedPreflight.can_dispatch(), "mixed pair-list preflight can dispatch");
    expectTrue(mixedPreflight.dispatchablePairCount == 1u, "mixed pair-list preflight counts dispatchable pairs");
}

void testManifoldPruneRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold),
        "empty manifold rejects for EmptyManifold prune reason");
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_prune(empty),
        "can_skip_manifold_prune on empty manifold");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::NoPruningNeeded),
        "clean manifold rejects for NoPruningNeeded prune reason");
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_prune(clean),
        "can_skip_manifold_prune on clean manifold");
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(clean),
        "prune_contact_manifold_if_needed no-ops on clean manifold");
    expectTrue(clean.pointCount == 1u, "prune_contact_manifold_if_needed preserves clean slots");

    fuse::physics::narrowphase::ContactManifold allSeparated{};
    allSeparated.contactNormal = {0.f, 1.f, 0.f};
    allSeparated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            allSeparated, fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmptyAfterPrune),
        "all-separated manifold rejects for WouldBeEmptyAfterPrune");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_prune_reject_reason_name(
                fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmptyAfterPrune),
            "WouldBeEmptyAfterPrune") == 0,
        "WouldBeEmptyAfterPrune prune reject reason has stable label");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            dirty, fuse::physics::narrowphase::ManifoldPruneRejectReason::None),
        "dirty manifold reports None prune reject reason");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_manifold_prune(dirty),
        "can_skip_manifold_prune false when in-place prune may proceed");
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(dirty),
        "prune_contact_manifold_if_needed keeps penetrating slot");
    expectTrue(dirty.pointCount == 1u, "prune_contact_manifold_if_needed removes separated slot");

    const auto dirtyPreflight = fuse::physics::narrowphase::preflight_manifold_prune(dirty);
    expectTrue(
        dirtyPreflight.reason == fuse::physics::narrowphase::ManifoldPruneRejectReason::NoPruningNeeded,
        "pruned manifold preflight carries NoPruningNeeded reason");
}

void testManifoldFinalizeRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold),
        "empty manifold rejects for EmptyManifold finalize reason");
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_finalize(empty),
        "can_skip_manifold_finalize on empty manifold");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            noNormal, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal),
        "zero-length normal rejects for InvalidNormal finalize reason");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            ready, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None),
        "ready manifold reports None finalize reject reason");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_manifold_finalize(ready),
        "can_skip_manifold_finalize false for ready manifold");

    const auto readyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(readyPreflight.can_finalize(), "finalize preflight accepts ready manifold with reason None");
    expectTrue(
        readyPreflight.reason == fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
        "finalize preflight carries reject reason");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            separated, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune),
        "separated manifold rejects for WouldBeEmptyAfterPrune finalize reason");
}

void testFrictionBasisRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            empty, fuse::physics::narrowphase::FrictionBasisRejectReason::SkippedManifold),
        "empty manifold rejects for SkippedManifold friction reason");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_preflight(empty),
        "should_skip_friction_basis_preflight on empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            needsBuild, fuse::physics::narrowphase::FrictionBasisRejectReason::None),
        "missing basis reports None friction reject reason");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_friction_basis_preflight(needsBuild),
        "should_skip_friction_basis_preflight false when rebuild may proceed");

    needsBuild.buildFrictionBasis();
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            needsBuild, fuse::physics::narrowphase::FrictionBasisRejectReason::ValidCachedBasis),
        "valid cached basis rejects for ValidCachedBasis friction reason");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::friction_basis_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRejectReason::ValidCachedBasis),
            "ValidCachedBasis") == 0,
        "ValidCachedBasis friction reject reason has stable label");

    const auto reusePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(reusePreflight.can_skip_rebuild(), "friction preflight can skip valid cached basis");
    expectTrue(
        reusePreflight.reason == fuse::physics::narrowphase::FrictionBasisRejectReason::ValidCachedBasis,
        "friction preflight carries reject reason");

    needsBuild.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            needsBuild, fuse::physics::narrowphase::FrictionBasisRejectReason::None),
        "stale basis reports None friction reject reason");
    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(stalePreflight.needsRebuild, "stale friction preflight needs rebuild");
    expectTrue(!stalePreflight.can_skip_rebuild(), "stale friction preflight cannot skip rebuild");
}

void testContactPairDeepenPassShouldRunGuards() {
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
        fuse::physics::narrowphase::should_run_contact_pair_dispatch({bodyA, bodyB}, bodies, shapes),
        "should_run_contact_pair_dispatch true for valid pair");
    expectTrue(
        !fuse::physics::narrowphase::should_run_contact_pair_dispatch({bodyA, bodyA}, bodies, shapes),
        "should_run_contact_pair_dispatch false for self pair");
    expectTrue(
        fuse::physics::narrowphase::should_run_contact_pair_dispatch({bodyA, bodyB}, bodies, shapes) ==
            !fuse::physics::narrowphase::should_skip_contact_pair_dispatch({bodyA, bodyB}, bodies, shapes),
        "should_run mirrors should_skip for base dispatch");

    expectTrue(
        fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({bodyA, bodyB}, bodies, shapes),
        "should_run deepen dispatch true for valid pair");
    expectTrue(
        !fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({sleepingA, sleepingB}, bodies, shapes),
        "should_run deepen dispatch false for both-sleeping pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {sleepingA, sleepingB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen_rejects_for_reason matches BothSleeping");
    expectTrue(
        !fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {bodyA, bodyB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen_rejects_for_reason does not false-positive valid pair");
}

void testManifoldPruneDeepenPassRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_reject_reason(empty) ==
            fuse::physics::narrowphase::ManifoldPruneRejectReason::Empty,
        "prune reject reason flags empty manifold");
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldPruneRejectReason::Empty),
        "prune_rejects_for_reason matches empty manifold");
    expectTrue(
        fuse::physics::narrowphase::should_skip_manifold_prune(empty),
        "should_skip_manifold_prune true for empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_prune(empty),
        "should_run_manifold_prune false for empty manifold");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_reject_reason(clean) ==
            fuse::physics::narrowphase::ManifoldPruneRejectReason::AlreadyClean,
        "prune reject reason flags already-clean manifold");
    expectTrue(
        fuse::physics::narrowphase::should_skip_manifold_prune(clean),
        "should_skip_manifold_prune true for clean manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_prune(clean),
        "should_run_manifold_prune false for clean manifold");
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(clean),
        "prune_contact_manifold_if_needed no-ops on clean manifold");
    expectTrue(clean.pointCount == 1u, "conditional prune preserves clean slots");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_reject_reason(dirty) ==
            fuse::physics::narrowphase::ManifoldPruneRejectReason::None,
        "prune reject reason None when work remains");
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_prune(dirty),
        "should_run_manifold_prune true for dirty manifold");
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(dirty),
        "prune_contact_manifold_if_needed keeps penetrating slot");
    expectTrue(dirty.pointCount == 1u, "conditional prune removes separated slot");

    const auto dirtyPreflight = fuse::physics::narrowphase::preflight_manifold_prune(dirty);
    expectTrue(
        dirtyPreflight.reason == fuse::physics::narrowphase::ManifoldPruneRejectReason::AlreadyClean,
        "preflight marks pruned manifold as already clean");
    expectTrue(dirtyPreflight.can_skip_prune(), "preflight can_skip_prune after conditional prune");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_prune_reject_reason_name(
                fuse::physics::narrowphase::ManifoldPruneRejectReason::AlreadyClean),
            "AlreadyClean") == 0,
        "prune reject reason name resolves AlreadyClean");
}

void testManifoldFinalizeDeepenPassRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_reject_reason(empty) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::Empty,
        "finalize reject reason flags empty manifold");
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::Empty),
        "finalize_rejects_for_reason matches empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_finalize(empty),
        "should_run_manifold_finalize false for empty manifold");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_reject_reason(ready) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
        "finalize reject reason None for ready manifold");
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_finalize(ready),
        "should_run_manifold_finalize true for ready manifold");
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_finalize(ready) ==
            !fuse::physics::narrowphase::can_skip_manifold_finalize(ready),
        "should_run mirrors can_skip for finalize");

    const auto readyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(
        readyPreflight.reason == fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
        "finalize preflight reason None for ready manifold");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_reject_reason(separated) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune,
        "finalize reject reason flags empty-after-prune");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_finalize(separated),
        "should_run_manifold_finalize false for separated manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_finalize_reject_reason_name(
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::NoPenetratingPoints),
            "NoPenetratingPoints") == 0,
        "finalize reject reason name resolves NoPenetratingPoints");
}

void testFrictionBasisDeepenPassShouldRunGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_reject_reason(empty) ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::EmptyOrInvalid,
        "friction reject reason flags empty manifold");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            empty, fuse::physics::narrowphase::FrictionBasisRejectReason::EmptyOrInvalid),
        "friction_rejects_for_reason matches empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(empty),
        "should_run_friction_basis_rebuild false for empty manifold");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_preflight(empty),
        "should_skip mirrors should_run for empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::friction_basis_reject_reason(needsBuild) ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::None,
        "friction reject reason None when rebuild needed");
    expectTrue(
        fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run_friction_basis_rebuild true without cached basis");

    needsBuild.buildFrictionBasis();
    expectTrue(
        fuse::physics::narrowphase::friction_basis_reject_reason(needsBuild) ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::BasisReusable,
        "friction reject reason flags reusable basis");
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run_friction_basis_rebuild false for valid cached basis");

    const auto reusePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(reusePreflight.can_skip_rebuild(), "preflight can_skip_rebuild for reusable basis");
    expectTrue(!reusePreflight.can_rebuild(), "preflight cannot rebuild reusable basis");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::friction_basis_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRejectReason::BasisReusable),
            "BasisReusable") == 0,
        "friction reject reason name resolves BasisReusable");
}

void testNarrowphasePairBatchPreflightGuards() {
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

    const std::vector<fuse::physics::broadphase::CandidatePair> mixed = {
        {sleepingA, sleepingB},
        {bodyA, bodyB},
    };
    const auto stats = fuse::physics::narrowphase::compute_narrowphase_pair_stats(mixed, bodies, shapes);
    expectTrue(stats.totalPairs == 2u, "batch stats counts total pairs");
    expectTrue(stats.dispatchablePairs == 1u, "batch stats counts dispatchable pairs");
    expectTrue(stats.rejectedPairs == 1u, "batch stats counts rejected pairs");
    expectTrue(
        fuse::physics::narrowphase::count_dispatchable_contact_pairs(mixed, bodies, shapes) == 1u,
        "dispatchable count matches stats");

    const auto batchPreflight = fuse::physics::narrowphase::preflight_narrowphase_pairs(mixed, bodies, shapes);
    expectTrue(batchPreflight.hasDispatchable, "batch preflight has dispatchable pair");
    expectTrue(!batchPreflight.can_skip_batch(), "batch preflight cannot skip mixed list");
    expectTrue(batchPreflight.can_dispatch_any(), "batch preflight can dispatch any");

    const std::vector<fuse::physics::broadphase::CandidatePair> allRejected = {{sleepingA, sleepingB}};
    const auto rejectedPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_pairs(allRejected, bodies, shapes);
    expectTrue(rejectedPreflight.can_skip_batch(), "batch preflight skips all-rejected list");
    expectTrue(!rejectedPreflight.can_dispatch_any(), "batch preflight cannot dispatch all-rejected list");

    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {sleepingA, sleepingB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejects_for_reason matches sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {bodyA, bodyB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejects_for_reason does not false-positive valid pair");

    const auto sleepingDetect =
        fuse::physics::narrowphase::detect_contacts_pair_if_needed({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingDetect.valid, "detect_if_needed rejects sleeping pair");
    const auto validDetect =
        fuse::physics::narrowphase::detect_contacts_pair_if_needed({bodyA, bodyB}, bodies, shapes);
    expectTrue(validDetect.valid, "detect_if_needed dispatches valid pair");
}

void testManifoldPruneFinalizeDeepenGuards() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_prune(clean),
        "can_skip_manifold_prune true for clean manifold");
    expectTrue(
        fuse::physics::narrowphase::prune_manifold_if_needed(clean),
        "prune_manifold_if_needed preserves clean manifold");
    expectTrue(clean.pointCount == 1u, "prune_manifold_if_needed leaves clean slots untouched");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        !fuse::physics::narrowphase::can_skip_manifold_prune(dirty),
        "can_skip_manifold_prune false when separated slots exist");
    expectTrue(
        fuse::physics::narrowphase::prune_manifold_if_needed(dirty),
        "prune_manifold_if_needed keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "prune_manifold_if_needed removes separated slot");

    const auto dirtyPreflight = fuse::physics::narrowphase::preflight_manifold_prune(dirty, 1e-6f, 1e-4f, 0.05f);
    expectTrue(!dirtyPreflight.needs_any_pruning(0.05f), "prune preflight clean after conditional prune");

    fuse::physics::narrowphase::ContactManifold shallow{};
    shallow.contactNormal = {0.f, 1.f, 0.f};
    shallow.addPoint({0.f, 0.f, 0.f}, 0.5f);
    shallow.addPoint({1.f, 0.f, 0.f}, 0.01f);
    expectTrue(
        fuse::physics::narrowphase::prune_manifold_if_needed(shallow, 1e-6f, 1e-4f, 0.05f),
        "prune_manifold_if_needed handles shallow slots");
    expectTrue(shallow.pointCount == 1u, "prune_manifold_if_needed removes shallow slot");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.valid = true;
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        !fuse::physics::narrowphase::finalize_contact_manifold_if_needed(separated),
        "finalize_if_needed no-ops on separated manifold");
    expectTrue(separated.valid, "finalize_if_needed leaves separated validity unchanged");

    fuse::physics::narrowphase::ContactManifold manifold =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    const auto finalizePreflight = fuse::physics::narrowphase::preflight_manifold_finalize(manifold);
    expectTrue(finalizePreflight.needs_any_work(), "finalize preflight needs work before finalize");
    expectTrue(
        fuse::physics::narrowphase::finalize_contact_manifold_if_needed(manifold),
        "finalize_if_needed finalizes valid manifold");
    expectTrue(manifold.valid, "finalize_if_needed sets validity on success");
    expectTrue(manifold.hasFrictionBasis(), "finalize_if_needed builds friction basis");
}

void testFrictionBasisDeepenPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::can_run_friction_basis_rebuild(empty),
        "can_run_friction_basis_rebuild false for empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::ensure_friction_basis_if_needed(empty),
        "ensure_friction_basis_if_needed false for empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto needsPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(needsPreflight.can_rebuild(), "friction preflight can_rebuild without cached basis");
    expectTrue(
        fuse::physics::narrowphase::can_run_friction_basis_rebuild(needsBuild),
        "can_run_friction_basis_rebuild true without cached basis");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_if_needed(needsBuild),
        "ensure_friction_basis_if_needed builds missing basis");
    expectTrue(needsBuild.hasFrictionBasis(), "ensure_friction_basis_if_needed stores orthonormal basis");

    const auto cachedTangent1 = needsBuild.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_if_needed(needsBuild),
        "ensure_friction_basis_if_needed reuses valid basis");
    expectNear(
        needsBuild.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "ensure_friction_basis_if_needed preserves cached tangent1");

    needsBuild.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::can_run_friction_basis_rebuild(needsBuild),
        "can_run_friction_basis_rebuild true for stale basis");
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_if_needed(needsBuild),
        "ensure_friction_basis_if_needed rebuilds stale basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(needsBuild),
        "ensure_friction_basis_if_needed matches current normal after rebuild");
}

void testRunNarrowphaseDeepenDispatchGuards() {
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

    const std::vector<fuse::physics::broadphase::CandidatePair> deepenRejectedPairs = {
        {sleepingA, sleepingB},
        {kinematicA, kinematicB},
    };

    fuse::physics::narrowphase::ContactBufferSoA rejectedBuffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(deepenRejectedPairs, bodies, shapes, rejectedBuffer);
    expectTrue(rejectedBuffer.isEmpty(), "deepen dispatch skips sleeping and kinematic pairs");
    expectTrue(
        fuse::physics::narrowphase::can_skip_narrowphase(deepenRejectedPairs, bodies, shapes),
        "can_skip_narrowphase true for all deepen-rejected pairs");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {dynamicA, dynamicB},
    };

    fuse::physics::narrowphase::ContactBufferSoA mixedBuffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(mixedPairs, bodies, shapes, mixedBuffer);
    expectTrue(mixedBuffer.activeCount == 1u, "mixed pair list keeps only dispatchable contact");
    const auto contact = mixedBuffer.manifoldAt(0u);
    expectTrue(contact.valid, "dispatchable pair still finalizes in hot path");
    expectTrue(contact.hasFrictionBasis(), "dispatchable pair still builds friction basis");
}

void testBuildFrictionTangentBasesReuseGuard() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(1u);

    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    manifold.buildFrictionBasis();
    buffer.writeSlot(0u, manifold);
    expectTrue(buffer.compact() == 1u, "reuse guard test compacts one manifold");

    const auto cachedTangent1 = buffer.tangentBasisAt(0u).tangent1;
    buffer.buildFrictionTangentBases();
    expectNear(
        buffer.tangentBasisAt(0u).tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "buildFrictionTangentBases preserves valid cached basis");

    fuse::physics::narrowphase::ContactManifold stale = buffer.manifoldAt(0u);
    stale.contactNormal = {1.f, 0.f, 0.f};
    buffer.writeSlot(0u, stale);
    buffer.compact();
    buffer.buildFrictionTangentBases();
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis(
            {1.f, 0.f, 0.f}, buffer.tangentBasisAt(0u)),
        "buildFrictionTangentBases refreshes stale basis");
}

void testContactPairDeepenPassDispatchGuards() {
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
        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {sleepingA, sleepingB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejects_for_reason matches both-sleeping pair");
        !fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {bodyA, bodyB},
        "deepen rejects_for_reason does not false-positive valid pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
    };
    const auto batchPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_pairs(mixedPairs, bodies, shapes);
    expectTrue(batchPreflight.stats.totalPairs == 2u, "batch preflight reports total pairs");
    expectTrue(batchPreflight.stats.dispatchablePairs == 1u, "batch preflight reports dispatchable count");
    expectTrue(batchPreflight.stats.rejectedPairs == 1u, "batch preflight reports rejected count");
    expectTrue(batchPreflight.can_dispatch_any(), "batch preflight can dispatch mixed list");
    expectTrue(!batchPreflight.can_skip_batch(), "batch preflight cannot skip mixed list");

        fuse::physics::narrowphase::should_run_narrowphase_dispatch(mixedPairs, bodies, shapes),
        "should_run_narrowphase_dispatch true when dispatchable pair exists");
        !fuse::physics::narrowphase::should_run_narrowphase_dispatch(
            {{sleepingA, sleepingB}}, bodies, shapes),
        "should_run_narrowphase_dispatch false when all pairs rejected");

    const auto validManifold =
        fuse::physics::narrowphase::detect_contacts_pair_if_needed({bodyA, bodyB}, bodies, shapes);
    expectTrue(validManifold.valid, "detect_if_needed returns valid manifold for dispatchable pair");
    const auto rejectedManifold =
        fuse::physics::narrowphase::detect_contacts_pair_if_needed({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!rejectedManifold.valid, "detect_if_needed returns invalid manifold for deepen-rejected pair");
}

void testManifoldPruneFinalizeDeepenPassGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
        fuse::physics::narrowphase::manifold_prune_reject_reason(empty) ==
            fuse::physics::narrowphase::ManifoldPruneRejectReason::Empty,
        "prune reject reason flags empty manifold");
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldPruneRejectReason::Empty),
        "prune rejects_for_reason matches empty manifold");
        std::strcmp(
            fuse::physics::narrowphase::manifold_prune_reject_reason_name(
                fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmpty),
            "WouldBeEmpty") == 0,
        "prune reject reason name resolves WouldBeEmpty");
        fuse::physics::narrowphase::can_skip_manifold_prune(empty),
        "can_skip_manifold_prune on empty manifold");
        !fuse::physics::narrowphase::should_run_manifold_prune(empty),
        "should_run_manifold_prune false on empty manifold");

    fuse::physics::narrowphase::ContactManifold allSeparated{};
    allSeparated.contactNormal = {0.f, 1.f, 0.f};
    allSeparated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    allSeparated.addPoint({1.f, 0.f, 0.f}, -0.2f);
        fuse::physics::narrowphase::manifold_prune_reject_reason(allSeparated) ==
            fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmpty,
        "prune reject reason flags would-be-empty manifold");
        fuse::physics::narrowphase::should_skip_manifold_prune(allSeparated),
        "should_skip_manifold_prune when prune would leave no points");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
        fuse::physics::narrowphase::should_run_manifold_prune(dirty),
        "should_run_manifold_prune when separated slots exist");
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(dirty),
        "prune_contact_manifold_if_needed keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "prune_contact_manifold_if_needed removes separated slots");

    fuse::physics::narrowphase::ContactManifold shallow{};
    shallow.contactNormal = {0.f, 1.f, 0.f};
    shallow.addPoint({0.f, 0.f, 0.f}, 0.5f);
    shallow.addPoint({1.f, 0.f, 0.f}, 0.01f);
        !fuse::physics::narrowphase::can_skip_manifold_prune(shallow, 1e-6f, 1e-4f, 0.05f),
        "can_skip_manifold_prune false when shallow prune needed");
        fuse::physics::narrowphase::prune_manifold_if_needed(shallow, 1e-6f, 1e-4f, 0.05f),
        "prune_manifold_if_needed keeps deep point");
    expectTrue(shallow.pointCount == 1u, "prune_manifold_if_needed removes shallow slot");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
        fuse::physics::narrowphase::should_run_manifold_finalize(ready),
        "should_run_manifold_finalize true for ready manifold");
        !fuse::physics::narrowphase::should_run_manifold_finalize(empty),
        "should_run_manifold_finalize false for empty manifold");
        fuse::physics::narrowphase::finalize_contact_manifold_if_needed(ready),
        "finalize_if_needed finalizes ready manifold");
    expectTrue(ready.valid, "finalize_if_needed sets validity");
    expectTrue(ready.hasFrictionBasis(), "finalize_if_needed builds friction basis");

    const auto dirtyPreflight = fuse::physics::narrowphase::preflight_manifold_prune(dirty);
    expectTrue(dirtyPreflight.can_skip_prune(), "can_skip_prune true after prune_if_needed");

void testFrictionBasisRebuildDeepenPassGuards() {
        !fuse::physics::narrowphase::should_rebuild_friction_basis(empty),
        "should_rebuild_friction_basis false for empty manifold");
        !fuse::physics::narrowphase::rebuild_friction_basis_from_preflight(empty),
        "rebuild_from_preflight false for empty manifold");
        !fuse::physics::narrowphase::can_run_friction_basis_rebuild(empty),
        "can_run_friction_basis_rebuild false for empty manifold");
        !fuse::physics::narrowphase::ensure_friction_basis_if_needed(empty),
        "ensure_if_needed false for empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
        fuse::physics::narrowphase::should_rebuild_friction_basis(needsBuild),
        "should_rebuild_friction_basis true without cached basis");
        fuse::physics::narrowphase::can_run_friction_basis_rebuild(needsBuild),
        "can_run_friction_basis_rebuild true without cached basis");
        fuse::physics::narrowphase::rebuild_friction_basis_from_preflight(needsBuild),
        "rebuild_from_preflight builds missing basis");
    expectTrue(needsBuild.hasFrictionBasis(), "rebuild_from_preflight stores orthonormal basis");

    const auto cachedTangent1 = needsBuild.frictionBasis.tangent1;
        fuse::physics::narrowphase::ensure_friction_basis_if_needed(needsBuild),
        "ensure_if_needed reuses valid cached basis");
    expectNear(
        needsBuild.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "ensure_if_needed preserves cached tangent1");
        !fuse::physics::narrowphase::should_rebuild_friction_basis(needsBuild),
        "should_rebuild_friction_basis false after valid rebuild");

    needsBuild.contactNormal = {1.f, 0.f, 0.f};
        "should_rebuild_friction_basis true for stale basis");
        "rebuild_from_preflight refreshes stale basis");
        fuse::physics::narrowphase::friction_basis_matches_normal(needsBuild),
        "rebuild_from_preflight produces basis matching current normal");

void testContactPairDeepenPassLayerGuards() {
    const fuse::u32 planeA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 planeB = bodies.addBody({0.f, 2.f, 0.f}, 1.f);
    const fuse::u32 dynamicA = bodies.addBody({1.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeA, {0.f, 1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeB, {0.f, -1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});

        fuse::physics::narrowphase::is_plane_plane_contact_pair({planeA, planeB}, shapes),
        "plane-plane guard detects two planes");
        fuse::physics::narrowphase::contact_pair_deepen_pass_reject_reason({planeA, planeB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::PlanePlane,
        "deepen-pass reject reason flags plane-plane pair");
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({planeA, planeB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedShapePair,
        "base deepen reject reason unchanged for plane-plane pair");

    const fuse::u32 badPlaneBody = bodies.addBody({4.f, 0.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, badPlaneBody, {0.f, 0.f, 0.f});
        fuse::physics::narrowphase::is_invalid_plane_normal_pair({dynamicA, badPlaneBody}, shapes),
        "invalid plane normal guard flags zero-length plane normal");
        fuse::physics::narrowphase::contact_pair_deepen_pass_reject_reason({dynamicA, badPlaneBody}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::InvalidPlaneNormal,
        "deepen-pass reject reason flags invalid plane normal");

    const auto deepenPassPreflight =
        fuse::physics::narrowphase::preflight_contact_pair_deepen_pass({dynamicA, dynamicB}, bodies, shapes);
    expectTrue(deepenPassPreflight.can_dispatch(), "deepen-pass preflight allows valid pair");
        !fuse::physics::narrowphase::should_skip_contact_pair_deepen_pass_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "deepen-pass skip guard allows valid pair");
        fuse::physics::narrowphase::should_skip_contact_pair_deepen_pass_dispatch({planeA, planeB}, bodies, shapes),
        "deepen-pass skip guard rejects plane-plane pair");
            {planeA, planeB},
            fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedShapePair),
        "deepen rejects_for_reason matches base deepen reason for plane-plane");

        {dynamicA, dynamicB},
        fuse::physics::narrowphase::preflight_contact_pair_batch(mixedPairs, bodies, shapes);
    expectTrue(batchPreflight.totalPairs == 2u, "batch preflight counts total pairs");
    expectTrue(batchPreflight.dispatchableCount == 1u, "batch preflight counts dispatchable pairs");
    expectTrue(batchPreflight.rejectedCount == 1u, "batch preflight counts rejected pairs");
    expectTrue(!batchPreflight.allRejected, "batch preflight not all rejected for mixed list");
        !fuse::physics::narrowphase::should_skip_contact_pair_batch(mixedPairs, bodies, shapes),
        "batch skip guard false when one pair dispatchable");
        fuse::physics::narrowphase::should_skip_contact_pair_batch({{planeA, planeB}}, bodies, shapes),
        "batch skip guard true when all pairs rejected");

            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::PlanePlane),
            "PlanePlane") == 0,
        "reject reason name resolves PlanePlane");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
        fuse::physics::narrowphase::manifold_prune_reject_reason(clean) ==
            fuse::physics::narrowphase::ManifoldPruneRejectReason::NoPruneNeeded,
        "prune reject reason flags clean manifold as no-op");
            clean,
            fuse::physics::narrowphase::ManifoldPruneRejectReason::NoPruneNeeded),
        "prune rejects_for_reason matches no-op");
        !fuse::physics::narrowphase::should_run_manifold_prune(clean),
        "should_run_manifold_prune false for clean manifold");

    fuse::physics::narrowphase::ContactManifold dirty = clean;
        "should_run_manifold_prune true for separated slot");
        "prune_if_needed keeps penetrating slot");
    expectTrue(dirty.pointCount == 1u, "prune_if_needed removes separated slot");

        !fuse::physics::narrowphase::prune_contact_manifold_if_needed(allSeparated),
        "prune_if_needed clears all-separated manifold");
    expectTrue(allSeparated.empty(), "prune_if_needed leaves separated manifold empty");

        fuse::physics::narrowphase::manifold_finalize_reject_reason(ready) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
        "finalize reject reason allows ready manifold");

        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            empty,
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold),
        "finalize rejects_for_reason flags empty manifold");
        !fuse::physics::narrowphase::finalize_contact_manifold_if_needed(empty),
        "finalize_if_needed no-ops on empty manifold");

                fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmptyAfterPrune),
            "WouldBeEmptyAfterPrune") == 0,
        "prune reject reason name resolves WouldBeEmptyAfterPrune");

void testFrictionBasisDeepenPassRejectGuards() {
        fuse::physics::narrowphase::friction_basis_rebuild_reject_reason(empty) ==
            fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::SkippedEmpty,
        "friction rebuild reject reason skips empty manifold");
        fuse::physics::narrowphase::friction_basis_rebuild_rejects_for_reason(
            fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::SkippedEmpty),
        "friction rebuild rejects_for_reason matches empty skip");
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(empty),
        "should_run_friction_basis_rebuild false for empty manifold");

    needsBuild.contactNormal = {0.f, 2.f, 0.f};
        fuse::physics::narrowphase::friction_basis_rebuild_reject_reason(needsBuild) ==
            fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::NeedsRebuild,
        "friction rebuild reject reason flags missing basis");
        fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run_friction_basis_rebuild true without cached basis");
        fuse::physics::narrowphase::normalize_contact_normal_if_needed(needsBuild),
        "normalize_if_needed scales non-unit normal");
    expectNear(needsBuild.contactNormal.length(), 1.f, 1e-4f, "normalize_if_needed produces unit normal");
        fuse::physics::narrowphase::ensure_friction_basis_after_preflight(needsBuild),
        "ensure_after_preflight builds orthonormal basis");
    expectTrue(needsBuild.hasFrictionBasis(), "ensure_after_preflight stores friction basis");

    fuse::physics::narrowphase::ContactManifold cached = needsBuild;
    const auto cachedTangent1 = cached.frictionBasis.tangent1;
        fuse::physics::narrowphase::friction_basis_rebuild_reject_reason(cached) ==
            fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuse,
        "friction rebuild reject reason reuses valid basis");
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(cached),
        "should_run_friction_basis_rebuild false for cached basis");
        fuse::physics::narrowphase::ensure_friction_basis_after_preflight(cached),
        "ensure_after_preflight reuses cached basis");
        cached.frictionBasis.tangent1.x,
        "ensure_after_preflight preserves cached tangent1");

            fuse::physics::narrowphase::friction_basis_rebuild_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::NeedsRebuild),
            "NeedsRebuild") == 0,
        "friction rebuild reject reason name resolves NeedsRebuild");


        fuse::physics::narrowphase::should_run_contact_pair_dispatch({bodyA, bodyB}, bodies, shapes),
        "should_run allows valid pair dispatch");
        !fuse::physics::narrowphase::should_run_contact_pair_dispatch({bodyA, bodyA}, bodies, shapes),
        "should_run rejects self pair");
        fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({bodyA, bodyB}, bodies, shapes),
        "should_run deepen allows valid pair");
        !fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({sleepingA, sleepingB}, bodies, shapes),
        "should_run deepen rejects both-sleeping pair");
        "deepen rejects_for_reason matches BothSleeping");

void testManifoldPruneRejectReasonGuards() {
            fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold,
            empty, fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold),
        "should_run_manifold_prune false for empty manifold");

            fuse::physics::narrowphase::ManifoldPruneRejectReason::AlreadyClean,
        "prune reject reason flags already-clean manifold");

        fuse::physics::narrowphase::manifold_prune_reject_reason(dirty) ==
            fuse::physics::narrowphase::ManifoldPruneRejectReason::None,
        "prune reject reason None when separated slots exist");
        "should_run_manifold_prune true for dirty manifold");

                fuse::physics::narrowphase::ManifoldPruneRejectReason::AlreadyClean),
            "AlreadyClean") == 0,
        "prune reject reason name resolves AlreadyClean");

void testManifoldFinalizeRejectReasonGuards() {
        fuse::physics::narrowphase::manifold_finalize_reject_reason(empty) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold,
        "finalize reject reason flags empty manifold");
            empty, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold),
        "finalize rejects_for_reason matches empty manifold");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
        fuse::physics::narrowphase::manifold_finalize_reject_reason(noNormal) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal,
        "finalize reject reason flags invalid normal");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
        fuse::physics::narrowphase::manifold_finalize_reject_reason(separated) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune,
        "finalize reject reason flags empty-after-prune");

        "finalize reject reason None for ready manifold");

    const auto readyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
        readyPreflight.reason == fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
        "finalize preflight reason None for ready manifold");

            fuse::physics::narrowphase::manifold_finalize_reject_reason_name(
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::NoPenetratingPoints),
            "NoPenetratingPoints") == 0,
        "finalize reject reason name resolves NoPenetratingPoints");

void testFrictionBasisRejectReasonGuards() {
        fuse::physics::narrowphase::friction_basis_reject_reason(empty) ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::EmptyManifold,
        "friction reject reason flags empty manifold");
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            empty, fuse::physics::narrowphase::FrictionBasisRejectReason::EmptyManifold),
        "friction rejects_for_reason matches empty manifold");

        fuse::physics::narrowphase::friction_basis_reject_reason(noNormal) ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::InvalidNormal,
        "friction reject reason flags invalid normal");

    fuse::physics::narrowphase::ContactManifold withBasis{};
    withBasis.contactNormal = {0.f, 1.f, 0.f};
    withBasis.addPoint({0.f, 0.f, 0.f}, 0.2f);
    withBasis.buildFrictionBasis();
        fuse::physics::narrowphase::friction_basis_reject_reason(withBasis) ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::CanReuseBasis,
        "friction reject reason flags reusable basis");
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(withBasis),
        "should_run_friction_basis_rebuild false for valid cached basis");

    const auto reusePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(withBasis);
        reusePreflight.reason ==
        "friction preflight reason CanReuseBasis for valid basis");

        fuse::physics::narrowphase::friction_basis_reject_reason(needsBuild) ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::None,
        "friction reject reason None when rebuild needed");
        "should_run_friction_basis_rebuild true when basis missing");

            fuse::physics::narrowphase::friction_basis_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRejectReason::CanReuseBasis),
            "CanReuseBasis") == 0,
        "friction reject reason name resolves CanReuseBasis");

void testContactPairDeepenRejectsForReasonGuards() {
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    bodies.flags[sleepingA] |= fuse::physics::RB_SLEEPING;
    bodies.flags[sleepingB] |= fuse::physics::RB_SLEEPING;

        fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "should_run_contact_pair_deepen_dispatch true for valid pair");
        "should_run_contact_pair_deepen_dispatch false for deepen-rejected pair");
        fuse::physics::narrowphase::should_run_narrowphase({{dynamicA, dynamicB}}, bodies, shapes),
        "should_run_narrowphase true when dispatchable pair exists");
        !fuse::physics::narrowphase::should_run_narrowphase({{sleepingA, sleepingB}}, bodies, shapes),
        "should_run_narrowphase false when all pairs deepen-rejected");

    expectEq(
        static_cast<fuse::u32>(fuse::physics::narrowphase::manifold_prune_reject_reason(empty)),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold),
        "empty manifold reports EmptyManifold prune reject reason");
        "manifold_prune_rejects_for_reason matches empty manifold");

        static_cast<fuse::u32>(fuse::physics::narrowphase::manifold_prune_reject_reason(clean)),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ManifoldPruneRejectReason::NothingToPrune),
        "clean manifold reports NothingToPrune prune reject reason");
        "should_run_manifold_prune false when nothing to prune");

        static_cast<fuse::u32>(fuse::physics::narrowphase::manifold_prune_reject_reason(dirty)),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ManifoldPruneRejectReason::None),
        "dirty manifold reports None prune reject reason");
        "should_run_manifold_prune true when separated slots exist");

        static_cast<fuse::u32>(fuse::physics::narrowphase::manifold_prune_reject_reason(allSeparated)),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmpty),
        "all-separated manifold reports WouldBeEmpty prune reject reason");
        "WouldBeEmpty prune reject reason has stable label");

    const auto preflight = fuse::physics::narrowphase::preflight_manifold_prune(dirty);
        static_cast<fuse::u32>(preflight.reason),
        "prune preflight carries reject reason");

        static_cast<fuse::u32>(fuse::physics::narrowphase::manifold_finalize_reject_reason(empty)),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold),
        "empty manifold reports EmptyManifold finalize reject reason");
        "manifold_finalize_rejects_for_reason matches empty manifold");
        "should_run_manifold_finalize false on empty manifold");

        static_cast<fuse::u32>(fuse::physics::narrowphase::manifold_finalize_reject_reason(noNormal)),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal),
        "zero-length normal reports InvalidNormal finalize reject reason");

        static_cast<fuse::u32>(fuse::physics::narrowphase::manifold_finalize_reject_reason(separated)),
        static_cast<fuse::u32>(
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune),
        "all-separated manifold reports WouldBeEmptyAfterPrune finalize reject reason");

        static_cast<fuse::u32>(fuse::physics::narrowphase::manifold_finalize_reject_reason(ready)),
        static_cast<fuse::u32>(fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None),
        "penetrating manifold reports None finalize reject reason");

    const auto preflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
        "finalize preflight carries reject reason");

        static_cast<fuse::u32>(fuse::physics::narrowphase::friction_basis_reject_reason(empty)),
        static_cast<fuse::u32>(fuse::physics::narrowphase::FrictionBasisRejectReason::EmptyManifold),
        "empty manifold reports EmptyManifold friction reject reason");
        "friction_basis_rejects_for_reason matches empty manifold");
        "should_run_friction_basis_rebuild false on empty manifold");

        static_cast<fuse::u32>(fuse::physics::narrowphase::friction_basis_reject_reason(needsBuild)),
        static_cast<fuse::u32>(fuse::physics::narrowphase::FrictionBasisRejectReason::None),
        "manifold without basis reports None friction reject reason");

        static_cast<fuse::u32>(fuse::physics::narrowphase::friction_basis_reject_reason(withBasis)),
        static_cast<fuse::u32>(fuse::physics::narrowphase::FrictionBasisRejectReason::CanReuse),
        "valid cached basis reports CanReuse friction reject reason");
        "should_run_friction_basis_rebuild false when basis can be reused");
                fuse::physics::narrowphase::FrictionBasisRejectReason::CanReuse),
            "CanReuse") == 0,
        "CanReuse friction reject reason has stable label");

    const auto preflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(withBasis);
        "friction preflight carries reject reason");

void testContactPairDeepenPassRejectReasonGuards() {

        fuse::physics::narrowphase::can_dispatch_contact_pair_deepen({dynamicA, dynamicB}, bodies, shapes),
        "can_dispatch_contact_pair_deepen allows valid pair");
        !fuse::physics::narrowphase::can_dispatch_contact_pair_deepen({sleepingA, sleepingB}, bodies, shapes),
        "can_dispatch_contact_pair_deepen rejects sleeping pair");
        "should_run_contact_pair_deepen_dispatch false for sleeping pair");

        fuse::physics::narrowphase::count_rejected_contact_pairs(mixedPairs, bodies, shapes) == 1u,
        "count_rejected_contact_pairs excludes deepen-rejected pairs");

        fuse::physics::narrowphase::preflight_narrowphase_batch(mixedPairs, bodies, shapes);
    expectTrue(batchPreflight.pairCount == 2u, "batch preflight reports pair count");
    expectTrue(batchPreflight.dispatchableCount == 1u, "batch preflight reports dispatchable count");
    expectTrue(batchPreflight.rejectedCount == 1u, "batch preflight reports rejected count");
    expectTrue(!batchPreflight.canSkip, "batch preflight canSkip false with dispatchable pair");
    expectTrue(batchPreflight.has_dispatchable(), "batch preflight has_dispatchable true");

    const auto sleepingPreflight =
        fuse::physics::narrowphase::preflight_contact_pair_deepen({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(sleepingPreflight.isSleeping, "deepen preflight flags sleeping reject");

        "prune rejects_for_reason flags empty manifold");
        fuse::physics::narrowphase::can_skip_manifold_prune_dispatch(empty),
        "can_skip_manifold_prune_dispatch on empty manifold");

            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::NoPruningNeeded),
        "prune rejects_for_reason flags clean manifold");
        fuse::physics::narrowphase::should_skip_manifold_prune(clean),
        "should_skip_manifold_prune true for clean manifold");

        !fuse::physics::narrowphase::can_skip_manifold_prune_dispatch(dirty),
        "can_skip_manifold_prune_dispatch false when prune needed");

        dirtyPreflight.reason == fuse::physics::narrowphase::ManifoldPruneRejectReason::None,
        "dirty manifold prune preflight carries None reject reason");

            allSeparated, fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmpty),
        "prune rejects_for_reason flags would-be-empty manifold");


            noNormal, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal),
        "finalize rejects_for_reason flags invalid normal");

            separated, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune),
        "finalize rejects_for_reason flags would-be-empty-after-prune");

        !fuse::physics::narrowphase::can_skip_manifold_finalize(ready),
        "can_skip_manifold_finalize false for ready manifold");

        "ready manifold finalize preflight carries None reject reason");
        "NoPenetratingPoints finalize reject reason has stable label");

void testFrictionBasisRebuildRejectReasonGuards() {
            empty, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::Skipped),
        "friction rebuild rejects_for_reason flags skipped manifold");
        !fuse::physics::narrowphase::can_dispatch_friction_basis_rebuild(empty),
        "can_dispatch_friction_basis_rebuild false on empty manifold");

            needsBuild, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::None),
        "friction rebuild rejects_for_reason None when rebuild needed");
        fuse::physics::narrowphase::can_dispatch_friction_basis_rebuild(needsBuild),
        "can_dispatch_friction_basis_rebuild true without cached basis");

    needsBuild.buildFrictionBasis();
            needsBuild, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuse),
        "friction rebuild rejects_for_reason CanReuse with valid basis");
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run_friction_basis_rebuild false with valid cached basis");

    const auto reusePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
        reusePreflight.reason == fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuse,
        "friction preflight carries CanReuse reject reason");
                fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::Skipped),
            "Skipped") == 0,
        "Skipped friction rebuild reject reason has stable label");

void testContactBufferWriteRejectGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

        fuse::physics::narrowphase::contact_buffer_write_rejects_for_reason(
            buffer, 0u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::None),
        "write reject reason allows valid manifold");
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 0u, valid).canWrite(),
        "write preflight allows valid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
            buffer, 0u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write reject reason flags self pair");
    buffer.writeSlot(0u, selfPair);
    expectTrue(buffer.compact() == 0u, "write preflight rejects self pair");

    fuse::physics::narrowphase::ContactManifold invalid{};
            buffer, 1u, invalid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "write reject reason flags invalid manifold");

            buffer, 99u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
        "write reject reason flags out-of-range slot");
            fuse::physics::narrowphase::contact_buffer_write_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
            "SelfPair") == 0,
        "write reject reason name resolves SelfPair");

void testContactBufferCompactionClampRejectGuards() {
        fuse::physics::narrowphase::contact_buffer_compaction_rejects_for_reason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "compaction reject reason flags empty buffer");
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipCompaction true for empty buffer");
        !fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRunCompaction false for empty buffer");

    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(1u, manifold);

            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "compaction reject reason flags all-valid slots");
    expectTrue(buffer.canSkipCompaction(), "canSkipCompaction true when all slots valid");
    expectTrue(buffer.compact() == 2u, "all-valid compaction preserves active count");

    buffer.setMaxCapacity(1u);
    fuse::physics::narrowphase::ContactManifold deeper = manifold;
    deeper.penetrationDepth = 0.9f;
    deeper.points[0].penetration = 0.9f;
    buffer.writeSlot(1u, deeper);
    buffer.compact();
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRunClamp true when active exceeds max capacity");
        fuse::physics::narrowphase::contact_buffer_clamp_rejects_for_reason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::None),
        "clamp reject reason allows clamp when over capacity");
    expectTrue(buffer.applyMaxCapacityClamp() == 1u, "clamp preflight gate truncates to max capacity");
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
        "clamp reject reason flags within-capacity buffer");
            fuse::physics::narrowphase::contact_buffer_clamp_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
            "WithinCapacity") == 0,
        "clamp reject reason name resolves WithinCapacity");


            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::CleanManifold),
        "prune reject reason flags clean manifold");

            allSeparated, fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmptyAfterPrune),

        "should_run_manifold_prune true when separated slots remain");



            separated, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::NoPenetratingPoints),
        "finalize reject reason flags non-penetrating manifold");

        "should_run_manifold_finalize true for penetrating manifold");
            ready, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None),
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal),
            "InvalidNormal") == 0,
        "finalize reject reason name resolves InvalidNormal");

void testContactPairShouldRunDispatchGuards() {

        "should_run base dispatch true for valid pair");
        "should_run base dispatch false for self pair");
        "should_run deepen dispatch true for valid pair");
        "should_run deepen dispatch false for both-sleeping pair");

void testFrictionBasisRebuildRejectGuards() {
            empty, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::EmptyManifold),
        "friction rebuild reject reason flags empty manifold");
        "should_run friction rebuild false for empty manifold");

            noNormal, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::NoValidNormal),
        "friction rebuild reject reason flags missing normal");

            withBasis, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuseBasis),
        "friction rebuild reject reason flags reusable basis");
        "should_run friction rebuild false for valid cached basis");

        "should_run friction rebuild true without cached basis");
        "friction rebuild reject reason allows missing basis");
                fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuseBasis),
        "friction rebuild reject reason name resolves CanReuseBasis");

void testContactBufferDeepenPassPreflights() {
    expectTrue(buffer.canSkipSoAIteration(), "empty buffer skips SoA iteration");
    expectTrue(buffer.canSkipCompaction(), "empty buffer skips compaction");
    expectTrue(buffer.canSkipMaxCapacityClamp(), "empty buffer skips clamp");

    const auto emptyCompaction =
        fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
        emptyCompaction.reason ==
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer,
        "compaction preflight flags empty buffer");
        "canSkipContactBufferCompaction on empty buffer");

    buffer.writeSlot(0u, valid);
    expectTrue(buffer.countValidSlots() == 1u, "countValidSlots counts written slot");
    expectTrue(buffer.slotIsValid(0u), "slotIsValid true for written slot");
    expectTrue(!buffer.slotIsValid(1u), "slotIsValid false for empty slot");

    const auto writePreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, valid);
    expectTrue(writePreflight.canWrite(), "write preflight allows valid manifold");
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 99u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidSlot),
        "write rejects for invalid slot");

        fuse::physics::narrowphase::contactBufferWriteRejectReason(buffer, 0u, selfPair) ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair,

    expectTrue(buffer.activeCount == 1u, "compact gathers valid slot");

    fuse::physics::narrowphase::ContactBufferSoA denseBuffer;
    denseBuffer.preparePairSlots(1u);
    denseBuffer.writeSlot(0u, valid);
    expectTrue(denseBuffer.canSkipCompaction(), "dense all-valid buffer skips compaction");
        fuse::physics::narrowphase::shouldRunContactBufferCompaction(denseBuffer) == false,
        "shouldRunContactBufferCompaction false when all slots valid");

    buffer.setMaxCapacity(0u);
        fuse::physics::narrowphase::preflightContactBufferClamp(buffer).reason ==
            fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity,
        "clamp preflight within capacity when maxCapacity unset");

void testNarrowphaseBatchDeepenPassGuards() {
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0u);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f, 0u);
    const fuse::u32 triggerA = bodies.addBody({0.f, 0.f, 0.f}, 0.f, fuse::physics::RB_TRIGGER);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, triggerA, {1.f, 0.f, 0.f});

        {dynamicA, triggerA},
    expectTrue(batchPreflight.can_dispatch(), "batch preflight can dispatch mixed list");
        !fuse::physics::narrowphase::narrowphase_batch_all_rejected(mixedPairs, bodies, shapes),
        "batch not all rejected for mixed list");
            fuse::physics::narrowphase::ContactPairRejectReason::AnyTrigger),
        "deepen_rejects_for_reason matches any-trigger pair");

void testManifoldPruneFinalizeCombinedPreflights() {
    ready.addPoint({0.f, 0.f, 0.f}, 0.3f);
    const auto readyCombined =
        fuse::physics::narrowphase::preflight_manifold_prune_finalize(ready);
    expectTrue(readyCombined.can_skip_prune(), "combined preflight skips prune for clean manifold");
    expectTrue(readyCombined.can_finalize(), "combined preflight can finalize clean manifold");
        !fuse::physics::narrowphase::should_skip_manifold_prune_finalize(ready),
        "should not skip all when finalize is possible");

    separated.addPoint({0.f, 0.f, 0.f}, -0.2f);
    const auto separatedCombined =
        fuse::physics::narrowphase::preflight_manifold_prune_finalize(separated);
    expectTrue(!separatedCombined.can_finalize(), "combined preflight rejects separated manifold");
        separatedCombined.finalize.wouldBeEmptyAfterPrune,
        "combined preflight flags empty-after-prune");

void testFrictionBasisNormalizeCombinedPreflights() {
    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.2f);
    const auto combined =
        fuse::physics::narrowphase::preflight_friction_basis_normalize_rebuild(unnormalized);
    expectTrue(combined.needsNormalize, "combined preflight flags normalize need");
    expectTrue(combined.needs_work(), "combined preflight needs work without basis");
        !fuse::physics::narrowphase::should_skip_friction_basis_normalize_rebuild(unnormalized),
        "combined skip false when normalize or rebuild needed");

    fuse::physics::narrowphase::ContactManifold unit{};
    unit.contactNormal = {0.f, 1.f, 0.f};
    unit.addPoint({0.f, 0.f, 0.f}, 0.2f);
    unit.buildFrictionBasis();
    const auto reuseCombined =
        fuse::physics::narrowphase::preflight_friction_basis_normalize_rebuild(unit);
    expectTrue(reuseCombined.can_skip_all(), "combined preflight can skip valid basis");
        fuse::physics::narrowphase::should_skip_friction_basis_normalize_rebuild(unit),
        "combined skip true for valid basis");

void testNarrowphaseDispatchPreflightGuards() {

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{dynamicA, dynamicB}};
    const auto dispatchPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_dispatch(pairs, bodies, shapes, buffer);
    expectTrue(dispatchPreflight.can_dispatch(), "dispatch preflight allows valid pair list");
        !fuse::physics::narrowphase::can_skip_narrowphase_dispatch(pairs, bodies, shapes),
        "can_skip_narrowphase_dispatch false for valid pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> emptyPairs{};
    const auto skippedPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_dispatch(emptyPairs, bodies, shapes, buffer);
    expectTrue(skippedPreflight.skipped, "dispatch preflight skips empty pair list");
        fuse::physics::narrowphase::can_skip_narrowphase_dispatch(emptyPairs, bodies, shapes),
        "can_skip_narrowphase_dispatch true for empty pair list");

void testContactPairDeepenGuardPassHelpers() {
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
        fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "should_run deepen dispatch true for valid pair");
    expectTrue(
        !fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({sleepingA, sleepingB}, bodies, shapes),
        "should_run deepen dispatch false for both-sleeping pair");
}

void testManifoldPruneFinalizeGuardPassHelpers() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold),
        "prune rejects_for_reason flags empty manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_prune_reject_reason_name(
                fuse::physics::narrowphase::ManifoldPruneRejectReason::NoPruningNeeded),
            "NoPruningNeeded") == 0,
        "prune reject reason name resolves NoPruningNeeded");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::NoPruningNeeded),
        "prune rejects_for_reason flags clean manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_prune(clean),
        "should_run prune false for clean manifold");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            separated, fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmpty),
        "prune rejects_for_reason flags would-be-empty manifold");
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_prune(separated),
        "should_run prune true when separated slots exist");

    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold),
        "finalize rejects_for_reason flags empty manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_finalize_reject_reason_name(
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal),
            "InvalidNormal") == 0,
        "finalize reject reason name resolves InvalidNormal");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            noNormal, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal),
        "finalize rejects_for_reason flags invalid normal");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.3f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            ready, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None),
        "finalize rejects_for_reason None for ready manifold");
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_finalize(ready),
        "should_run finalize true for ready manifold");
}

void testFrictionBasisRebuildGuardPassHelpers() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rebuild_rejects_for_reason(
            empty, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::EmptyManifold),
        "friction rebuild rejects_for_reason flags empty manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::friction_basis_rebuild_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuseCached),
            "CanReuseCached") == 0,
        "friction rebuild reject reason name resolves CanReuseCached");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rebuild_rejects_for_reason(
            needsBuild, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::None),
        "friction rebuild rejects_for_reason None when basis missing");
    expectTrue(
        fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run friction rebuild true without cached basis");

    fuse::physics::narrowphase::ContactManifold withBasis{};
    withBasis.contactNormal = {0.f, 1.f, 0.f};
    withBasis.addPoint({0.f, 0.f, 0.f}, 0.2f);
    withBasis.buildFrictionBasis();
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rebuild_rejects_for_reason(
            withBasis, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuseCached),
        "friction rebuild rejects_for_reason flags reusable basis");
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(withBasis),
        "should_run friction rebuild false for valid cached basis");
}

void testContactBufferGuardPassHelpers() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.valid = true;
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 0u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::None),
        "write rejects_for_reason None for valid manifold");
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 99u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
        "write rejects_for_reason flags out-of-range slot");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
            "SelfPair") == 0,
        "write reject reason name resolves SelfPair");
    expectTrue(buffer.canSkipWrite(99u, valid), "canSkipWrite true for out-of-range slot");

    buffer.writeSlot(0u, valid);
    expectTrue(
        !fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "compaction rejects_for_reason not AllValid with spare invalid slot");
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRun compaction true with invalid spare slot");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, valid);
    expectTrue(
        !fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "compaction rejects_for_reason not AllValid with invalid slot");
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRun compaction true with invalid slot");

    buffer.compact();
    expectTrue(buffer.activeCount == 1u, "compaction gathers valid slot");

    buffer.setMaxCapacity(1u);
    expectTrue(
        fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
        "clamp rejects_for_reason WithinCapacity at limit");
    expectTrue(buffer.canSkipClamp(), "canSkipClamp true when within capacity");
}

void testNarrowphasePairSlotGuardPassHelpers() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    const auto validPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_pair_slot({bodyA, bodyB}, bodies, shapes);
    expectTrue(validPreflight.can_process(), "pair-slot preflight allows valid pair");
    expectTrue(validPreflight.canDispatch, "pair-slot preflight can dispatch valid pair");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_narrowphase_pair_slot({bodyA, bodyB}, bodies, shapes),
        "should_skip pair-slot false for valid pair");

    const auto selfPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_pair_slot({bodyA, bodyA}, bodies, shapes);
    expectTrue(!selfPreflight.can_process(), "pair-slot preflight rejects self pair");
    expectTrue(selfPreflight.pairRejected, "pair-slot preflight marks self pair rejected");
    expectTrue(
        fuse::physics::narrowphase::should_skip_narrowphase_pair_slot({bodyA, bodyA}, bodies, shapes),
        "should_skip pair-slot true for self pair");
}

void testContactPairDeepenPassDispatchGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 triggerA = bodies.addBody({2.f, 0.f, 0.f}, 0.f, fuse::physics::RB_TRIGGER);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, triggerA, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {dynamicA, triggerA},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::AnyTrigger),
        "deepen rejects_for_reason matches any-trigger pair");
    expectTrue(
        !fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {dynamicA, dynamicB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::AnyTrigger),
        "deepen rejects_for_reason does not false-positive valid pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {dynamicA, triggerA},
        {dynamicA, dynamicB},
    };
    expectTrue(
        fuse::physics::narrowphase::should_run_narrowphase(mixedPairs, bodies, shapes),
        "should_run_narrowphase true when one pair is dispatchable");
    expectTrue(
        !fuse::physics::narrowphase::should_run_narrowphase({{dynamicA, triggerA}}, bodies, shapes),
        "should_run_narrowphase false when all pairs deepen-rejected");
    expectTrue(
        fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "should_run_contact_pair_deepen_dispatch true for valid pair");
    expectTrue(
        !fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({dynamicA, triggerA}, bodies, shapes),
        "should_run_contact_pair_deepen_dispatch false for any-trigger pair");

    const auto dispatchPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_dispatch(mixedPairs, bodies, shapes);
    expectTrue(dispatchPreflight.can_dispatch(), "dispatch preflight allows mixed pair batch");
    expectTrue(dispatchPreflight.dispatchableCount == 1u, "dispatch preflight counts dispatchable pairs");

    const auto emptyPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_dispatch({}, bodies, shapes);
    expectTrue(emptyPreflight.skipped, "dispatch preflight skips empty pair list");
    expectTrue(!emptyPreflight.can_dispatch(), "dispatch preflight cannot dispatch empty list");
}

void testManifoldPruneFinalizeRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold),
        "prune reject reason flags empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_prune(empty),
        "should_run_manifold_prune false for empty manifold");
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold),
        "finalize reject reason flags empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_finalize(empty),
        "should_run_manifold_finalize false for empty manifold");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::AllValid),
        "prune reject reason flags clean manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_prune(clean),
        "should_run_manifold_prune false for clean manifold");
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_finalize(clean),
        "should_run_manifold_finalize true for penetrating manifold");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            separated, fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmpty),
        "prune reject reason flags would-be-empty manifold");
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            separated, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune),
        "finalize reject reason flags would-be-empty-after-prune");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            noNormal, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal),
        "finalize reject reason flags invalid normal");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_prune_reject_reason_name(
                fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmpty),
            "WouldBeEmpty") == 0,
        "prune reject reason name resolves WouldBeEmpty");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_finalize_reject_reason_name(
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::NoPenetration),
            "NoPenetration") == 0,
        "finalize reject reason name resolves NoPenetration");
}

void testFrictionBasisRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            empty, fuse::physics::narrowphase::FrictionBasisRejectReason::EmptyManifold),
        "friction reject reason flags empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(empty),
        "should_run_friction_basis_rebuild false for empty manifold");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            noNormal, fuse::physics::narrowphase::FrictionBasisRejectReason::InvalidNormal),
        "friction reject reason flags invalid normal");

    fuse::physics::narrowphase::ContactManifold withBasis{};
    withBasis.contactNormal = {0.f, 1.f, 0.f};
    withBasis.addPoint({0.f, 0.f, 0.f}, 0.2f);
    withBasis.buildFrictionBasis();
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            withBasis, fuse::physics::narrowphase::FrictionBasisRejectReason::CanReuse),
        "friction reject reason flags reusable basis");
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(withBasis),
        "should_run_friction_basis_rebuild false for valid basis");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::friction_basis_reject_reason(needsBuild) ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::None,
        "friction reject reason None when rebuild needed");
    expectTrue(
        fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run_friction_basis_rebuild true without cached basis");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::friction_basis_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRejectReason::CanReuse),
            "CanReuse") == 0,
        "friction reject reason name resolves CanReuse");
}

void testContactBufferPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(buffer.canSkipSoAIteration(), "canSkipSoAIteration on empty buffer");
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "compaction reject reason flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipContactBufferCompaction on empty buffer");
    expectTrue(
        !fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRunContactBufferCompaction false on empty buffer");
    expectTrue(
        fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::EmptyBuffer),
        "clamp reject reason flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferClamp(buffer),
        "canSkipContactBufferClamp on empty buffer");

    buffer.preparePairSlots(1u);
    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.3f);

    const auto writePreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, valid);
    expectTrue(writePreflight.canWrite(), "write preflight allows valid manifold");

    fuse::physics::narrowphase::ContactManifold invalid{};
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 0u, invalid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "write reject reason flags invalid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 0u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write reject reason flags self pair");

    buffer.writeSlot(0u, valid);
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "compaction reject reason flags all-valid slots");
    expectTrue(buffer.compact() == 1u, "compact keeps single valid slot");

    buffer.setMaxCapacity(1u);
    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold shallow = valid;
    shallow.penetrationDepth = 0.1f;
    shallow.points[0].penetration = 0.1f;
    fuse::physics::narrowphase::ContactManifold deep = valid;
    deep.bodyB = 2u;
    deep.penetrationDepth = 0.9f;
    deep.points[0].penetration = 0.9f;
    buffer.writeSlot(0u, shallow);
    buffer.writeSlot(1u, deep);
    buffer.compact();
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRunContactBufferClamp true when overflow exists");
    expectTrue(
        !fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
        "clamp reject reason not WithinCapacity when overflow exists");
    expectTrue(buffer.applyMaxCapacityClamp() == 1u, "clamp truncates to max capacity");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
            "SelfPair") == 0,
        "write reject reason name resolves SelfPair");
}

void testContactPairGuardPassShouldRunHelpers() {
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
        fuse::physics::narrowphase::should_run_contact_pair_dispatch({bodyA, bodyB}, bodies, shapes),
        "should_run dispatch true for valid pair");
    expectTrue(
        !fuse::physics::narrowphase::should_run_contact_pair_dispatch({bodyA, bodyA}, bodies, shapes),
        "should_run dispatch false for self pair");
    expectTrue(
        fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({bodyA, bodyB}, bodies, shapes),
        "should_run deepen dispatch true for valid pair");
    expectTrue(
        !fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({sleepingA, sleepingB}, bodies, shapes),
        "should_run deepen dispatch false for both-sleeping pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {sleepingA, sleepingB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejects_for_reason matches BothSleeping");
    expectTrue(
        !fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {bodyA, bodyB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejects_for_reason does not false-positive valid pair");
}

void testManifoldPruneFinalizeGuardPassShouldRun() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_prune(clean),
        "should_run prune false for clean manifold");
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_finalize(clean),
        "should_run finalize true for ready manifold");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_manifold_finalize(clean),
        "can_skip finalize false for ready manifold");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_prune(dirty),
        "should_run prune true when separated slots exist");
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_finalize(dirty),
        "should_run finalize true when penetrating slots remain after prune");

    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_finalize(empty),
        "should_run finalize false for empty manifold");
}

void testFrictionBasisRebuildGuardPassShouldRun() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(empty),
        "should_run rebuild false for empty manifold");
    expectTrue(
        fuse::physics::narrowphase::should_skip_friction_basis_preflight(empty),
        "should_skip preflight true for empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run rebuild true without cached basis");

    needsBuild.buildFrictionBasis();
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run rebuild false with valid cached basis");
}

void testContactBufferGuardPassPreflights() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;

    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferWrite(buffer, 0u, valid),
        "shouldRun write true for valid manifold");
    expectTrue(
        !fuse::physics::narrowphase::canSkipContactBufferWrite(buffer, 0u, valid),
        "canSkip write false for valid manifold");
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer,
            99u,
            valid,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
        "write rejects_for_reason flags out-of-range slot");
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer,
            0u,
            selfPair,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write rejects_for_reason flags self pair");

    buffer.writeSlot(0u, valid);
    buffer.writeSlot(1u, valid);
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactRejectsForReason(
            buffer,
            fuse::physics::narrowphase::ContactBufferCompactRejectReason::AllValid),
        "compact rejects_for_reason flags all-valid buffer");
    expectTrue(
        !fuse::physics::narrowphase::shouldRunContactBufferCompact(buffer),
        "shouldRun compact false when all slots valid");
    expectTrue(buffer.compact() == 2u, "compact all-valid buffer keeps both contacts");

    buffer.setMaxCapacity(1u);
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRun clamp true when active exceeds max capacity");
    expectTrue(
        fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer,
            fuse::physics::narrowphase::ContactBufferClampRejectReason::None),
        "clamp rejects_for_reason None when clamp needed");
    expectTrue(buffer.compactAndClamp() == 1u, "compactAndClamp truncates to max capacity");

    fuse::physics::narrowphase::ContactBufferSoA emptyBuffer;
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactRejectsForReason(
            emptyBuffer,
            fuse::physics::narrowphase::ContactBufferCompactRejectReason::EmptyBuffer),
        "compact rejects_for_reason flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            emptyBuffer,
            fuse::physics::narrowphase::ContactBufferClampRejectReason::EmptyBuffer),
        "clamp rejects_for_reason flags empty buffer");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
            "SelfPair") == 0,
        "write reject reason name resolves SelfPair");
}

void testNarrowphasePairSlotPreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    const auto validPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_pair_slot({bodyA, bodyB}, bodies, shapes);
    expectTrue(validPreflight.can_dispatch(), "pair-slot preflight allows valid pair");
    expectTrue(
        fuse::physics::narrowphase::should_run_narrowphase_pair_slot({bodyA, bodyB}, bodies, shapes),
        "should_run pair slot true for valid pair");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_narrowphase_pair_slot({bodyA, bodyB}, bodies, shapes),
        "should_skip pair slot false for valid pair");

    const auto selfPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_pair_slot({bodyA, bodyA}, bodies, shapes);
    expectTrue(!selfPreflight.can_dispatch(), "pair-slot preflight rejects self pair");
    expectTrue(
        fuse::physics::narrowphase::should_skip_narrowphase_pair_slot({bodyA, bodyA}, bodies, shapes),
        "should_skip pair slot true for self pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{bodyA, bodyB}, {bodyA, bodyA}};
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(pairs, bodies, shapes, buffer);
    expectTrue(buffer.activeCount == 1u, "narrowphase skips invalid pair slot via preflight");
}

void testContactPairDeepenPassRejectHelpers() {
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
        "deepen_rejects_for_reason matches both-sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {dynamicA, dynamicB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen_rejects_for_reason does not false-positive valid pair");

    const fuse::u32 tinySphereBody = bodies.addBody({5.f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, tinySphereBody, {1e-5f, 0.f, 0.f});
    expectTrue(
        fuse::physics::narrowphase::is_near_degenerate_shape_pair({dynamicA, tinySphereBody}, shapes),
        "near-degenerate guard flags tiny positive-radius sphere");
    expectTrue(
        !fuse::physics::narrowphase::is_degenerate_shape_pair({dynamicA, tinySphereBody}, shapes),
        "base degenerate guard unchanged for near-degenerate sphere");

    const fuse::u32 planeBody = bodies.addBody({6.f, 0.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeBody, {0.f, 2.f, 0.f});
    expectTrue(
        fuse::physics::narrowphase::is_unnormalized_plane_shape_pair({dynamicA, planeBody}, shapes),
        "unnormalized-plane guard flags scaled plane normal");
}

void testManifoldRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold),
        "prune_rejects_for_reason flags empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_prune(empty),
        "should_run_manifold_prune false for empty manifold");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::AlreadyClean),
        "prune_rejects_for_reason flags already-clean manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_prune(clean),
        "should_run_manifold_prune false for clean manifold");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_prune(dirty),
        "should_run_manifold_prune true when separated slots exist");
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_reject_reason(dirty) ==
            fuse::physics::narrowphase::ManifoldPruneRejectReason::None,
        "prune_reject_reason None when prune may proceed");

    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold),
        "finalize_rejects_for_reason flags empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_finalize(empty),
        "should_run_manifold_finalize false for empty manifold");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            separated,
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune),
        "finalize_rejects_for_reason flags would-be-empty-after-prune");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_finalize(ready),
        "should_run_manifold_finalize true for penetrating manifold");
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_reject_reason(ready) ==
            fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
        "finalize_reject_reason None for ready manifold");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_finalize_reject_reason_name(
                fuse::physics::narrowphase::ManifoldFinalizeRejectReason::InvalidNormal),
            "InvalidNormal") == 0,
        "finalize reject reason name resolves InvalidNormal");
}

void testFrictionBasisRejectReasonGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            empty, fuse::physics::narrowphase::FrictionBasisRejectReason::Skipped),
        "friction_rejects_for_reason flags skipped empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(empty),
        "should_run_friction_basis_rebuild false for empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::friction_basis_reject_reason(needsBuild) ==
            fuse::physics::narrowphase::FrictionBasisRejectReason::None,
        "friction_reject_reason None when rebuild may proceed");
    expectTrue(
        fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run_friction_basis_rebuild true without cached basis");

    needsBuild.buildFrictionBasis();
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            needsBuild, fuse::physics::narrowphase::FrictionBasisRejectReason::CanReuse),
        "friction_rejects_for_reason flags reusable basis");
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run_friction_basis_rebuild false for valid cached basis");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::friction_basis_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRejectReason::CanReuse),
            "CanReuse") == 0,
        "friction reject reason name resolves CanReuse");
}

void testContactBufferPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(3u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    const auto invalidSlotPreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 9u, valid);
    expectTrue(!invalidSlotPreflight.canWrite(), "write preflight rejects invalid slot");
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 9u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidSlot),
        "write_rejects_for_reason flags invalid slot");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 0u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write_rejects_for_reason flags self pair");

    const auto validWritePreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, valid);
    expectTrue(validWritePreflight.canWrite(), "write preflight accepts valid manifold");
    buffer.writeSlot(0u, valid);
    buffer.writeSlot(1u, valid);
    buffer.writeSlot(2u, valid);

    expectTrue(buffer.canSkipCompaction(), "canSkipCompaction true when all prepared slots valid");
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "compaction_rejects_for_reason flags all-valid buffer");
    expectTrue(
        !fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRunCompaction false when all slots valid");

    buffer.clear();
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "compaction_rejects_for_reason flags empty buffer");

    buffer.setMaxCapacity(1u);
    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold shallow = valid;
    shallow.penetrationDepth = 0.1f;
    shallow.points[0].penetration = 0.1f;
    fuse::physics::narrowphase::ContactManifold deep = valid;
    deep.bodyB = 2u;
    deep.penetrationDepth = 0.9f;
    deep.points[0].penetration = 0.9f;
    buffer.writeSlot(0u, shallow);
    buffer.writeSlot(1u, deep);
    buffer.compact();
    expectTrue(buffer.canApplyMaxCapacityClamp(), "canApplyMaxCapacityClamp true when over capacity");
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRunClamp true when over capacity");
    expectTrue(
        fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity) == false,
        "clamp_rejects_for_reason false when clamp needed");
}

void testNarrowphaseDispatchPreflightGuards() {
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

    const auto emptyPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_dispatch({}, bodies, shapes);
    expectTrue(emptyPreflight.emptyPairs, "dispatch preflight marks empty pair list");
    expectTrue(emptyPreflight.can_skip_entire_dispatch(), "dispatch preflight can skip empty list");
    expectTrue(!emptyPreflight.can_prepare_buffer(), "dispatch preflight cannot prepare empty list");

    const std::vector<fuse::physics::broadphase::CandidatePair> sleepingPairs = {{sleepingA, sleepingB}};
    const auto allRejectedPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_dispatch(sleepingPairs, bodies, shapes);
    expectTrue(allRejectedPreflight.allPairsDeepenRejected, "dispatch preflight marks all deepen-rejected");
    expectTrue(
        allRejectedPreflight.dispatchablePairCount == 0u,
        "dispatch preflight reports zero dispatchable pairs");
    expectTrue(
        fuse::physics::narrowphase::should_skip_narrowphase_pair_slot(sleepingPairs[0], bodies, shapes),
        "pair-slot skip guard rejects sleeping pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {dynamicA, dynamicB},
    };
    const auto mixedPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_dispatch(mixedPairs, bodies, shapes);
    expectTrue(!mixedPreflight.can_skip_entire_dispatch(), "dispatch preflight cannot skip mixed list");
    expectTrue(mixedPreflight.dispatchablePairCount == 1u, "dispatch preflight counts dispatchable pair");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_narrowphase_pair_slot(mixedPairs[1], bodies, shapes),
        "pair-slot skip guard allows valid pair");
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
        fuse::physics::narrowphase::is_dispatchable_contact_pair({dynamicA, dynamicB}, bodies, shapes),
        "is_dispatchable true for valid pair");
    expectTrue(
        !fuse::physics::narrowphase::is_dispatchable_contact_pair({sleepingA, sleepingB}, bodies, shapes),
        "is_dispatchable false for both-sleeping pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {sleepingA, sleepingB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen_rejects_for_reason matches both-sleeping pair");
    expectTrue(
        fuse::physics::narrowphase::should_run_narrowphase({{dynamicA, dynamicB}}, bodies, shapes),
        "should_run_narrowphase true when dispatchable pair exists");
    expectTrue(
        !fuse::physics::narrowphase::should_run_narrowphase({{sleepingA, sleepingB}}, bodies, shapes),
        "should_run_narrowphase false when all pairs deepen-rejected");

    const auto batchPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_dispatch(
            {{sleepingA, sleepingB}, {dynamicA, dynamicB}}, bodies, shapes);
    expectTrue(batchPreflight.pairCount == 2u, "batch preflight reports pair count");
    expectTrue(batchPreflight.dispatchableCount == 1u, "batch preflight counts dispatchable pairs");
    expectTrue(batchPreflight.can_dispatch(), "batch preflight can dispatch mixed list");
    expectTrue(!batchPreflight.can_skip_dispatch(), "batch preflight does not skip mixed list");

    const auto deepenPair =
        fuse::physics::narrowphase::detect_contacts_pair_deepen({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!deepenPair.valid, "detect_contacts_pair_deepen rejects both-sleeping pair");
    const auto validDeepenPair =
        fuse::physics::narrowphase::detect_contacts_pair_deepen({dynamicA, dynamicB}, bodies, shapes);
    expectTrue(validDeepenPair.valid, "detect_contacts_pair_deepen detects valid pair");
}

void testManifoldPruneFinalizeFollowUpGuards() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_prune(clean),
        "should_run_manifold_prune false for clean manifold");
    expectTrue(
        fuse::physics::narrowphase::preflight_manifold_prune(clean).should_run_prune() ==
            fuse::physics::narrowphase::should_run_manifold_prune(clean),
        "prune preflight should_run_prune mirrors guard");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_prune(dirty),
        "should_run_manifold_prune true when separated slot exists");
    expectTrue(
        fuse::physics::narrowphase::prune_manifold_if_needed(dirty),
        "prune_manifold_if_needed keeps penetrating point");
    expectTrue(dirty.pointCount == 1u, "prune_manifold_if_needed removes separated slot");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_finalize(ready),
        "should_run_manifold_finalize true for ready manifold");
    const auto finalizePreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(
        finalizePreflight.should_run_finalize(),
        "finalize preflight should_run_finalize true for ready manifold");

    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_finalize(empty),
        "should_run_manifold_finalize false for empty manifold");
}

void testFrictionBasisFollowUpPreflights() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(empty),
        "should_run_friction_basis_rebuild false for empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::preflight_friction_basis_rebuild(empty).should_run_rebuild(),
        "friction preflight should_run_rebuild false for empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 2.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run_friction_basis_rebuild true without cached basis");
    expectTrue(
        fuse::physics::narrowphase::normalize_contact_normal_if_needed(needsBuild),
        "normalize_contact_normal_if_needed normalizes scaled normal");
    expectNear(needsBuild.contactNormal.length(), 1.f, 1e-4f, "normalize leaves unit normal");

    fuse::physics::narrowphase::ContactManifold unit = needsBuild;
    expectTrue(
        !fuse::physics::narrowphase::normalize_contact_normal_if_needed(unit),
        "normalize_contact_normal_if_needed no-ops on unit normal");
    unit.buildFrictionBasis();
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(unit),
        "should_run_friction_basis_rebuild false after basis build");
    expectTrue(
        fuse::physics::narrowphase::preflight_friction_basis_rebuild(unit).should_run_rebuild() ==
            fuse::physics::narrowphase::should_run_friction_basis_rebuild(unit),
        "friction preflight should_run_rebuild mirrors guard");
}

void testContactBufferPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(buffer.canSkipSoAIteration(), "empty buffer skips SoA iteration");
    expectTrue(buffer.canSkipCompaction(), "empty buffer skips compaction");
    expectTrue(buffer.canSkipMaxCapacityClamp(), "empty buffer skips clamp");
    expectTrue(buffer.canSkipCompactAndClamp(), "empty buffer skips compact+clamp");
    expectTrue(
        fuse::physics::narrowphase::should_skip_contact_buffer_compact(buffer),
        "should_skip_contact_buffer_compact on empty buffer");

    buffer.preparePairSlots(3u);
    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);
    valid.buildFrictionBasis();
    buffer.writeSlot(0u, valid);
    buffer.writeSlot(2u, valid);
    expectTrue(buffer.countValidSlots() == 2u, "countValidSlots counts written slots");
    expectTrue(!buffer.canSkipCompaction(), "sparse slots need compaction");

    const auto compactionPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compact(buffer);
    expectTrue(compactionPreflight.needsCompaction, "compaction preflight flags sparse slots");
    expectTrue(compactionPreflight.should_run_compaction(), "compaction preflight should_run_compaction");

    buffer.compact();
    expectTrue(buffer.canSkipCompaction(), "compacted buffer skips compaction");
    expectTrue(
        fuse::physics::narrowphase::should_skip_contact_buffer_compact(buffer),
        "should_skip_contact_buffer_compact after compact");

    buffer.setMaxCapacity(1u);
    expectTrue(!buffer.canSkipMaxCapacityClamp(), "over-capacity buffer needs clamp");
    const auto clampPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(buffer);
    expectTrue(clampPreflight.needsClamp, "clamp preflight flags over-capacity buffer");
    expectTrue(clampPreflight.should_run_clamp(), "clamp preflight should_run_clamp");
    buffer.applyMaxCapacityClamp();
    expectTrue(buffer.canSkipMaxCapacityClamp(), "clamped buffer skips clamp");

    const auto frictionPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_friction_tangents(buffer);
    expectTrue(
        fuse::physics::narrowphase::should_skip_contact_buffer_friction_rebuild(buffer),
        "should_skip_contact_buffer_friction_rebuild with valid tangents");
    buffer.tangent1[0u] = {};
    buffer.tangent2[0u] = {};
    const auto staleFrictionPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_friction_tangents(buffer);
    expectTrue(staleFrictionPreflight.needsRebuildCount == 1u, "friction preflight flags stale tangent slot");
    expectTrue(
        staleFrictionPreflight.should_run_rebuild(),
        "friction preflight should_run_rebuild for stale tangents");
    buffer.buildFrictionTangentBasesIfNeeded();
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis(
            buffer.manifoldAt(0u).contactNormal, buffer.tangentBasisAt(0u)),
        "buildFrictionTangentBasesIfNeeded restores orthonormal frame");
}

void testRunNarrowphaseIntoBufferDeepenGuards() {
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

    fuse::physics::narrowphase::ContactBufferSoA sleepingBuffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBufferDeepen(
        {{sleepingA, sleepingB}}, bodies, shapes, sleepingBuffer);
    expectTrue(sleepingBuffer.isEmpty(), "deepen narrowphase skips all-sleeping pairs");

    fuse::physics::narrowphase::ContactBufferSoA validBuffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBufferDeepen(
        {{dynamicA, dynamicB}}, bodies, shapes, validBuffer);
    expectTrue(validBuffer.activeCount == 1u, "deepen narrowphase produces contact for valid pair");
    expectTrue(
        validBuffer.manifoldAt(0u).hasFrictionBasis(),
        "deepen narrowphase finalizes friction basis");
}

void testContactPairGuardPassRejectGuards() {
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
        "should_run base dispatch for valid pair");
    expectTrue(
        !fuse::physics::narrowphase::can_skip_contact_pair_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "can_skip base dispatch false for valid pair");
    expectTrue(
        !fuse::physics::narrowphase::should_run_contact_pair_dispatch({dynamicA, dynamicA}, bodies, shapes),
        "should_run base dispatch false for self pair");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_pair_dispatch({dynamicA, dynamicA}, bodies, shapes),
        "can_skip base dispatch true for self pair");

    expectTrue(
        !fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({sleepingA, sleepingB}, bodies, shapes),
        "should_run deepen dispatch false for both-sleeping pair");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_pair_deepen_dispatch({sleepingA, sleepingB}, bodies, shapes),
        "can_skip deepen dispatch true for both-sleeping pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {sleepingA, sleepingB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen rejectsForReason matches BothSleeping");

    const auto deepenPreflight =
        fuse::physics::narrowphase::preflight_contact_pair_deepen({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(deepenPreflight.bothSleeping, "deepen preflight flags bothSleeping");
    expectTrue(!deepenPreflight.can_dispatch(), "deepen preflight rejects both-sleeping pair");
}

void testManifoldPruneFinalizeGuardPass() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            empty, fuse::physics::narrowphase::ManifoldPruneRejectReason::EmptyManifold),
        "prune rejectsForReason flags EmptyManifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::manifold_prune_reject_reason_name(
                fuse::physics::narrowphase::ManifoldPruneRejectReason::AlreadyClean),
            "AlreadyClean") == 0,
        "prune reject reason name resolves AlreadyClean");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_prune(empty),
        "should_run prune false for empty manifold");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::AlreadyClean),
        "prune rejectsForReason flags AlreadyClean");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_prune(clean),
        "should_run prune false for clean manifold");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            separated, fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmpty),
        "prune rejectsForReason flags WouldBeEmpty");
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            separated, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune),
        "finalize rejectsForReason flags WouldBeEmptyAfterPrune");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_finalize(separated),
        "should_run finalize false for separated manifold");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_finalize(ready),
        "should_run finalize true for penetrating manifold");
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            ready, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None),
        "finalize rejectsForReason None for ready manifold");

    const auto readyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(
        readyPreflight.reason == fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None,
        "finalize preflight reason None for ready manifold");
}

void testFrictionBasisGuardPass() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rebuild_rejects_for_reason(
            empty, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::EmptyManifold),
        "friction rebuild rejectsForReason flags EmptyManifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(empty),
        "should_run friction rebuild false for empty manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::friction_basis_rebuild_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuseBasis),
            "CanReuseBasis") == 0,
        "friction rebuild reject reason name resolves CanReuseBasis");

    fuse::physics::narrowphase::ContactManifold withBasis{};
    withBasis.contactNormal = {0.f, 1.f, 0.f};
    withBasis.addPoint({0.f, 0.f, 0.f}, 0.2f);
    withBasis.buildFrictionBasis();
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rebuild_rejects_for_reason(
            withBasis, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuseBasis),
        "friction rebuild rejectsForReason flags CanReuseBasis");
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(withBasis),
        "should_run friction rebuild false when basis can be reused");

    const auto reusePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(withBasis);
    expectTrue(
        reusePreflight.reason == fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuseBasis,
        "friction preflight reason CanReuseBasis for valid basis");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run friction rebuild true without cached basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rebuild_rejects_for_reason(
            needsBuild, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::None),
        "friction rebuild rejectsForReason None when rebuild needed");
}

void testContactBufferGuardPass() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    fuse::physics::narrowphase::generate_contact_manifold(valid);

    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer,
            0u,
            valid,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::None),
        "write rejectsForReason None for valid manifold");
    buffer.writeSlot(0u, valid);
    expectTrue(buffer.slotIsValid(0u), "writeSlot stores valid contact after preflight");

    fuse::physics::narrowphase::ContactManifold invalid{};
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer,
            0u,
            invalid,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "write rejectsForReason flags InvalidManifold");
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer,
            99u,
            valid,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
        "write rejectsForReason flags OutOfRangeSlot");

    buffer.validFlags[1u] = 0u;
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRun compaction when invalid slot exists");
    expectTrue(
        !fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkip compaction false when holes exist");
    buffer.compact();
    expectTrue(buffer.activeCount == 1u, "compact gathers valid slot after preflight gate");
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "compaction rejectsForReason flags AllValid after compact");

    buffer.setMaxCapacity(1u);
    buffer.preparePairSlots(3u);
    for (fuse::u32 slot = 0u; slot < 3u; ++slot) {
        fuse::physics::narrowphase::ContactManifold manifold = valid;
        manifold.bodyA = slot;
        manifold.bodyB = slot + 1u;
        manifold.penetrationDepth = static_cast<fuse::physics::f32>(slot) * 0.1f + 0.1f;
        buffer.writeSlot(slot, manifold);
    }
    buffer.compact();
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRun clamp when activeCount exceeds maxCapacity");
    buffer.applyMaxCapacityClamp();
    expectTrue(buffer.activeCount == 1u, "clamp reduces activeCount to maxCapacity");
    expectTrue(buffer.droppedCount == 2u, "clamp records dropped contacts");
    expectTrue(
        fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
        "clamp rejectsForReason flags WithinCapacity after clamp");
}

void testContactPairBatchDeepenPreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 dynamicA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 triggerA = bodies.addBody({2.f, 0.f, 0.f}, 0.f, fuse::physics::RB_TRIGGER);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, triggerA, {1.f, 0.f, 0.f});

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {dynamicA, triggerA},
        {dynamicA, dynamicB},
    };

    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {sleepingA, sleepingB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen_rejects_for_reason matches both-sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {dynamicA, dynamicB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen_rejects_for_reason does not false-positive valid pair");

    expectTrue(
        fuse::physics::narrowphase::count_rejected_contact_pairs_deepen(mixedPairs, bodies, shapes) == 2u,
        "count_rejected tallies deepen-rejected pairs");

    const auto batchPreflight =
        fuse::physics::narrowphase::preflight_contact_pair_batch_deepen(mixedPairs, bodies, shapes);
    expectTrue(batchPreflight.totalPairs == 3u, "batch preflight reports total pair count");
    expectTrue(batchPreflight.dispatchableCount == 1u, "batch preflight counts dispatchable pairs");
    expectTrue(batchPreflight.rejectedCount == 2u, "batch preflight counts rejected pairs");
    expectTrue(!batchPreflight.allRejected, "batch preflight not all-rejected with one dispatchable pair");
    expectTrue(batchPreflight.can_dispatch_any(), "batch preflight can dispatch any when one valid pair");

    const auto allRejectedPreflight = fuse::physics::narrowphase::preflight_contact_pair_batch_deepen(
        {{sleepingA, sleepingB}, {dynamicA, triggerA}}, bodies, shapes);
    expectTrue(allRejectedPreflight.allRejected, "batch preflight all-rejected when none dispatchable");
    expectTrue(!allRejectedPreflight.can_dispatch_any(), "batch preflight cannot dispatch when all rejected");
}

void testManifoldProcessPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);

    const auto readyProcess = fuse::physics::narrowphase::preflight_manifold_process(ready);
    expectTrue(readyProcess.can_process(), "process preflight can process ready manifold");
    expectTrue(!readyProcess.can_skip_all(), "process preflight does not skip ready manifold");
    expectTrue(!readyProcess.prune.needs_pruning(), "process preflight prune clean for ready manifold");
    expectTrue(readyProcess.finalize.can_finalize(), "process preflight finalize accepts ready manifold");

    expectTrue(
        fuse::physics::narrowphase::finalize_manifold_using_preflight(ready),
        "finalize_using_preflight finalizes ready manifold");
    expectTrue(ready.valid, "finalize_using_preflight sets validity");
    expectTrue(ready.hasFrictionBasis(), "finalize_using_preflight builds friction basis");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    dirty.addPoint({0.00001f, 0.f, 0.f}, 0.35f);
    expectTrue(
        fuse::physics::narrowphase::prune_manifold_using_preflight(dirty),
        "prune_using_preflight keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "prune_using_preflight removes separated and duplicate slots");
    expectNear(dirty.maxPenetration(), 0.4f, 1e-4f, "prune_using_preflight keeps deepest merged point");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        !fuse::physics::narrowphase::prune_manifold_using_preflight(separated),
        "prune_using_preflight clears all-separated manifold");
    expectTrue(separated.empty(), "prune_using_preflight leaves separated manifold empty");

    const auto separatedProcess = fuse::physics::narrowphase::preflight_manifold_process(separated);
    expectTrue(separatedProcess.can_skip_all(), "process preflight skips all-separated manifold");
    expectTrue(
        !fuse::physics::narrowphase::finalize_manifold_using_preflight(separated),
        "finalize_using_preflight no-ops on separated manifold");
}

void testFrictionBasisRebuildUsingPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold scaled{};
    scaled.contactNormal = {0.f, 2.f, 0.f};
    scaled.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::normalize_contact_normal_if_needed(scaled),
        "normalize_if_needed modifies scaled normal");
    expectNear(scaled.contactNormal.length(), 1.f, 1e-4f, "normalize_if_needed produces unit normal");

    fuse::physics::narrowphase::ContactManifold unit{};
    unit.contactNormal = {0.f, 1.f, 0.f};
    unit.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        !fuse::physics::narrowphase::normalize_contact_normal_if_needed(unit),
        "normalize_if_needed no-ops on unit normal");

    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_using_preflight(scaled),
        "rebuild_using_preflight builds basis for valid manifold");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(scaled),
        "rebuild_using_preflight produces basis matching normal");

    const auto cachedTangent1 = scaled.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_using_preflight(scaled),
        "rebuild_using_preflight reuses valid basis");
    expectNear(
        scaled.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "rebuild_using_preflight preserves cached tangent1");

    scaled.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_using_preflight(scaled),
        "rebuild_using_preflight refreshes stale basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(scaled),
        "rebuild_using_preflight matches normal after refresh");

    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::rebuild_friction_basis_using_preflight(empty),
        "rebuild_using_preflight skips empty manifold");
}

void testContactBufferWritePreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;

    expectTrue(
        fuse::physics::narrowphase::ContactBufferSoA::canWriteSlot(0u, 2u, valid),
        "canWriteSlot accepts valid manifold");
    expectTrue(
        !fuse::physics::narrowphase::ContactBufferSoA::canWriteSlot(2u, 2u, valid),
        "canWriteSlot rejects out-of-range slot");
    expectTrue(
        !fuse::physics::narrowphase::ContactBufferSoA::canWriteSlot(1u, 2u, selfPair),
        "canWriteSlot rejects self pair");

    expectTrue(buffer.writeSlotIfValid(0u, valid), "writeSlotIfValid writes valid manifold");
    expectTrue(!buffer.writeSlotIfValid(1u, selfPair), "writeSlotIfValid rejects self pair");
    expectTrue(buffer.compact() == 1u, "writeSlotIfValid compacts one valid slot");
}

void testRunNarrowphaseDeepenPreflightGuards() {
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

    fuse::physics::narrowphase::ContactBufferSoA baseBuffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(pairs, bodies, shapes, baseBuffer);
    expectTrue(baseBuffer.activeCount == 2u, "base narrowphase dispatches sleeping and dynamic pairs");

    fuse::physics::narrowphase::ContactBufferSoA deepenBuffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBufferDeepen(pairs, bodies, shapes, deepenBuffer);
    expectTrue(deepenBuffer.activeCount == 1u, "deepen narrowphase skips sleeping pair");
    expectTrue(deepenBuffer.manifoldAt(0u).valid, "deepen narrowphase produces valid dynamic contact");

    const auto deepenManifolds = fuse::physics::narrowphase::runNarrowphaseDeepen(pairs, bodies, shapes);
    expectTrue(deepenManifolds.size() == 1u, "runNarrowphaseDeepen returns one dispatchable contact");
}

void testContactPairDeepenDispatchGuards() {
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
        fuse::physics::narrowphase::is_dispatchable_contact_pair({dynamicA, dynamicB}, bodies, shapes),
        "is_dispatchable true for valid pair");
    expectTrue(
        !fuse::physics::narrowphase::is_dispatchable_contact_pair({sleepingA, sleepingB}, bodies, shapes),
        "is_dispatchable false for both-sleeping pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {sleepingA, sleepingB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "deepen_rejects_for_reason matches both-sleeping pair");

    const auto baseManifold = fuse::physics::narrowphase::detect_contacts_pair(
        {sleepingA, sleepingB}, bodies, shapes);
    const auto deepenManifold = fuse::physics::narrowphase::detect_contacts_pair_deepen(
        {sleepingA, sleepingB}, bodies, shapes);
    expectTrue(baseManifold.valid, "base detect still dispatches sleeping pair");
    expectTrue(!deepenManifold.valid, "deepen detect rejects sleeping pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {dynamicA, dynamicB},
    };
    const auto filtered =
        fuse::physics::narrowphase::filter_dispatchable_contact_pairs(mixedPairs, bodies, shapes);
    expectTrue(filtered.size() == 1u, "filter_dispatchable keeps one valid pair");
    expectTrue(filtered[0].bodyA == dynamicA && filtered[0].bodyB == dynamicB, "filtered pair is dynamic pair");
}

void testRunNarrowphaseDeepenDispatch() {
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
    fuse::physics::narrowphase::runNarrowphaseIntoBufferDeepen(pairs, bodies, shapes, buffer);
    expectTrue(buffer.activeCount == 1u, "deepen narrowphase compacts one dispatchable contact");
    const auto manifold = buffer.manifoldAt(0u);
    expectTrue(manifold.valid, "deepen narrowphase produces valid manifold");
    expectTrue(manifold.bodyA == dynamicA && manifold.bodyB == dynamicB, "deepen narrowphase keeps dynamic pair");

    const auto manifolds = fuse::physics::narrowphase::runNarrowphaseDeepen(pairs, bodies, shapes);
    expectTrue(manifolds.size() == 1u, "runNarrowphaseDeepen returns one manifold");
    expectTrue(manifolds[0].hasFrictionBasis(), "deepen vector path finalizes friction basis");
}

void testContactBufferWritePreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);
    valid.buildFrictionBasis();

    const auto validPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_write(0u, buffer, valid);
    expectTrue(validPreflight.can_write(), "write preflight allows valid manifold");
    expectTrue(buffer.writeSlotIfValid(0u, valid), "writeSlotIfValid writes valid manifold");
    expectTrue(buffer.validFlags[0u] == 1u, "writeSlotIfValid marks slot valid");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    const auto selfPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_write(1u, buffer, selfPair);
    expectTrue(!selfPreflight.can_write(), "write preflight rejects self pair");
    expectTrue(selfPreflight.selfPair, "write preflight flags self pair");
    expectTrue(
        fuse::physics::narrowphase::should_skip_contact_buffer_write(1u, buffer, selfPair),
        "should_skip_contact_buffer_write on self pair");
    expectTrue(!buffer.writeSlotIfValid(1u, selfPair), "writeSlotIfValid rejects self pair");

    fuse::physics::narrowphase::ContactManifold invalid{};
    invalid.bodyA = 2u;
    invalid.bodyB = 3u;
    const auto invalidPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_write(1u, buffer, invalid);
    expectTrue(!invalidPreflight.can_write(), "write preflight rejects invalid manifold");
    expectTrue(invalidPreflight.invalidManifold, "write preflight flags invalid manifold");
}

void testManifoldFinalizeWithPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::finalize_contact_manifold_with_preflight(empty),
        "finalize_with_preflight no-ops on empty manifold");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        !fuse::physics::narrowphase::finalize_contact_manifold_with_preflight(separated),
        "finalize_with_preflight rejects separated manifold");

    fuse::physics::narrowphase::ContactManifold manifold =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::finalize_contact_manifold_with_preflight(manifold),
        "finalize_with_preflight finalizes penetrating manifold");
    expectTrue(manifold.valid, "finalize_with_preflight sets validity");
    expectTrue(manifold.hasFrictionBasis(), "finalize_with_preflight builds friction basis");
    expectTrue(!manifold.hasNonUnitNormal(), "finalize_with_preflight normalizes contact normal");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.5f);
    dirty.addPoint({0.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(dirty),
        "prune_if_needed keeps penetrating point after separated prune");
    expectTrue(dirty.pointCount == 1u, "prune_if_needed removes separated slot");
}

void testFrictionBasisRebuildWithPreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::rebuild_friction_basis_with_preflight(empty),
        "rebuild_with_preflight false on empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 2.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_with_preflight(needsBuild),
        "rebuild_with_preflight builds basis for scaled normal");
    expectTrue(needsBuild.hasFrictionBasis(), "rebuild_with_preflight produces orthonormal basis");
    expectTrue(!needsBuild.hasNonUnitNormal(), "rebuild_with_preflight normalizes contact normal");

    const auto cachedTangent1 = needsBuild.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_with_preflight(needsBuild),
        "rebuild_with_preflight reuses valid basis");
    expectNear(
        needsBuild.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "rebuild_with_preflight preserves cached tangent1");

    fuse::physics::narrowphase::ContactManifold stale = needsBuild;
    stale.contactNormal = {1.f, 0.f, 0.f};
    fuse::physics::narrowphase::compute_friction_tangents_with_preflight(stale);
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(stale),
        "compute_with_preflight refreshes stale basis");
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
void testContactPairZeroInvMassDeepenGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 zeroMassA = bodies.addBody({0.f, 0.f, 0.f}, 0.f);
    const fuse::u32 zeroMassB = bodies.addBody({1.5f, 0.f, 0.f}, 0.f);
    const fuse::u32 dynamicA = bodies.addBody({0.f, 2.f, 0.f}, 1.f);
    const fuse::u32 dynamicB = bodies.addBody({1.5f, 2.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, zeroMassA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, zeroMassB, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, dynamicB, {1.f, 0.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::is_zero_inv_mass_contact_pair({zeroMassA, zeroMassB}, bodies),
        "zero inv-mass guard detects both non-responsive bodies");
        !fuse::physics::narrowphase::is_zero_inv_mass_contact_pair({dynamicA, dynamicB}, bodies),
        "zero inv-mass guard allows dynamic pair");
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({zeroMassA, zeroMassB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::ZeroInvMass,
        "deepen reject reason flags zero inv-mass pair");
        fuse::physics::narrowphase::contact_pair_reject_reason({zeroMassA, zeroMassB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::None,
        "base reject reason unchanged for zero inv-mass pair");
        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
            {zeroMassA, zeroMassB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::ZeroInvMass),
        "deepen rejects_for_reason matches zero inv-mass pair");
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
            "ZeroInvMass") == 0,
        "reject reason name resolves ZeroInvMass");
}

void testNarrowphaseBatchPreflightGuards() {
void testContactPairDeepenShouldRunGuards() {
void testContactPairDeepenDispatchSkipGuards() {
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

void testNarrowphasePairBatchPreflightGuards() {
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    const std::vector<fuse::physics::broadphase::CandidatePair> mixed = {
        {sleepingA, sleepingB},
        {bodyA, bodyB},
    };
    const auto stats = fuse::physics::narrowphase::compute_narrowphase_pair_stats(mixed, bodies, shapes);
    expectTrue(stats.totalPairs == 2u, "batch stats counts total pairs");
    expectTrue(stats.dispatchablePairs == 1u, "batch stats counts dispatchable pairs");
    expectTrue(stats.rejectedPairs == 1u, "batch stats counts rejected pairs");
    expectTrue(
        fuse::physics::narrowphase::count_dispatchable_contact_pairs(mixed, bodies, shapes) == 1u,
        "dispatchable count matches stats");

    const auto batchPreflight = fuse::physics::narrowphase::preflight_narrowphase_pairs(mixed, bodies, shapes);
    expectTrue(batchPreflight.hasDispatchable, "batch preflight has dispatchable pair");
    expectTrue(!batchPreflight.can_skip_batch(), "batch preflight cannot skip mixed list");
    expectTrue(batchPreflight.can_dispatch_any(), "batch preflight can dispatch any");

    const std::vector<fuse::physics::broadphase::CandidatePair> allRejected = {{sleepingA, sleepingB}};
    const auto rejectedPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_pairs(allRejected, bodies, shapes);
    expectTrue(rejectedPreflight.can_skip_batch(), "batch preflight skips all-rejected list");
    expectTrue(!rejectedPreflight.can_dispatch_any(), "batch preflight cannot dispatch all-rejected list");

        fuse::physics::narrowphase::contact_pair_deepen_rejects_for_reason(
        fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "should_run deepen dispatch true for valid pair");
        !fuse::physics::narrowphase::can_skip_contact_pair_deepen_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "can_skip deepen dispatch false for valid pair");
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

        fuse::physics::narrowphase::is_empty_narrowphase_input(bodies, emptyShapes),
        "empty-set guard flags bodies without shapes");
        fuse::physics::narrowphase::can_skip_narrowphase_for_empty_input(bodies, emptyShapes),
        "skip guard early-outs when shapes are missing");

        !fuse::physics::narrowphase::can_skip_narrowphase_for_empty_input(bodies, shapes),
        "skip guard allows dispatch when bodies and shapes exist");

        fuse::physics::narrowphase::preflight_contact_pair({bodyA, bodyA}, bodies, shapes);
        fuse::physics::narrowphase::contact_pair_was_rejected(rejectedPreflight),
        "contact_pair_was_rejected true for rejected preflight");

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
    const std::vector<fuse::physics::broadphase::CandidatePair> emptyPairs{};
    const auto emptyPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_batch(emptyPairs, bodies, shapes);
    expectTrue(emptyPreflight.skipped, "batch preflight skips empty pair list");
    expectTrue(!emptyPreflight.can_dispatch(), "batch preflight cannot dispatch empty list");

    const std::vector<fuse::physics::broadphase::CandidatePair> rejectedPairs = {{sleepingA, sleepingB}};
        fuse::physics::narrowphase::preflight_narrowphase_batch(rejectedPairs, bodies, shapes);
    expectTrue(rejectedPreflight.skipped, "batch preflight skips all-rejected pairs");
    expectTrue(rejectedPreflight.stats.rejectedCount == 1u, "batch preflight counts rejected pair");
        fuse::physics::narrowphase::can_skip_narrowphase(rejectedPairs, bodies, shapes),
        "can_skip_narrowphase true for all-rejected batch");

    const auto mixedPreflight =
    expectTrue(!mixedPreflight.skipped, "batch preflight does not skip mixed batch");
    expectTrue(mixedPreflight.can_dispatch(), "batch preflight can dispatch mixed batch");
    expectTrue(mixedPreflight.stats.dispatchableCount == 1u, "batch preflight counts dispatchable pair");
        fuse::physics::narrowphase::count_dispatchable_contact_pairs(mixedPairs, bodies, shapes) == 1u,
        "count_dispatchable matches batch preflight");
        fuse::physics::narrowphase::has_dispatchable_contact_pairs(mixedPairs, bodies, shapes),
        "has_dispatchable true for mixed batch");
        !fuse::physics::narrowphase::can_skip_narrowphase(mixedPairs, bodies, shapes),
        "can_skip_narrowphase false when dispatchable pair exists");

void testManifoldGeneratePreflightGuards() {
    const auto emptyPreflight = fuse::physics::narrowphase::preflight_generate_contact_manifold(empty);
    expectTrue(emptyPreflight.skipped, "generate preflight skips empty manifold");
    expectTrue(!emptyPreflight.can_generate(), "generate preflight cannot generate empty manifold");
        fuse::physics::narrowphase::can_skip_generate_contact_manifold(empty),
        "can_skip_generate true for empty manifold");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    const auto readyPreflight = fuse::physics::narrowphase::preflight_generate_contact_manifold(ready);
    expectTrue(!readyPreflight.skipped, "generate preflight does not skip ready manifold");
    expectTrue(readyPreflight.can_generate(), "generate preflight can generate penetrating manifold");
    expectTrue(readyPreflight.needsFrictionBasis, "generate preflight needs friction basis");
    expectTrue(!readyPreflight.needsPruning, "generate preflight reports no pruning for clean manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({sleepingA, sleepingB}, bodies, shapes),
        "should_run deepen dispatch false for both-sleeping pair");
        fuse::physics::narrowphase::can_skip_contact_pair_deepen_dispatch({sleepingA, sleepingB}, bodies, shapes),
        "can_skip deepen dispatch true for both-sleeping pair");

void testManifoldPruneRejectReasonGuards() {
        "empty manifold reports EmptyManifold prune reject reason");
        fuse::physics::narrowphase::can_skip_manifold_prune(empty),
        "can_skip_manifold_prune on empty manifold");
        !fuse::physics::narrowphase::should_run_manifold_prune(empty),
        "should_run_manifold_prune false on empty manifold");
        "can_skip_contact_pair_deepen_dispatch on both-sleeping pair");
        "should_run_contact_pair_deepen_dispatch false on both-sleeping pair");
        fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "should_run_contact_pair_deepen_dispatch true on valid pair");

        "empty manifold rejects for EmptyManifold");
                fuse::physics::narrowphase::ManifoldPruneRejectReason::AllClean),
            "AllClean") == 0,
        "AllClean prune reject reason has stable label");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::NoPruningNeeded),
        "clean manifold reports NoPruningNeeded prune reject reason");
        fuse::physics::narrowphase::can_skip_manifold_prune(clean),
        "can_skip_manifold_prune on clean manifold");
        !fuse::physics::narrowphase::should_run_manifold_prune(clean),
        "should_run_manifold_prune false on clean manifold");
    expectTrue(
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            clean, fuse::physics::narrowphase::ManifoldPruneRejectReason::AllClean),
        "clean manifold rejects for AllClean");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
            separated, fuse::physics::narrowphase::ManifoldPruneRejectReason::AllSeparated),
        "prune rejects_for_reason flags all-separated manifold");
        !fuse::physics::narrowphase::prune_contact_manifold_with_preflight(separated),
        "prune_with_preflight clears all-separated manifold");
    expectTrue(separated.empty(), "prune_with_preflight clears separated slots");
        "deepen rejects_for_reason matches sleeping pair");
    expectTrue(
            {bodyA, bodyB},
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),

    const auto sleepingDetect =
        fuse::physics::narrowphase::detect_contacts_pair_if_needed({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingDetect.valid, "detect_if_needed rejects sleeping pair");
    const auto validDetect =
        fuse::physics::narrowphase::detect_contacts_pair_if_needed({bodyA, bodyB}, bodies, shapes);
    expectTrue(validDetect.valid, "detect_if_needed dispatches valid pair");

void testManifoldPruneFinalizeDeepenGuards() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
        fuse::physics::narrowphase::can_skip_manifold_prune(clean),
        "can_skip_manifold_prune true for clean manifold");
        fuse::physics::narrowphase::prune_manifold_if_needed(clean),
        "prune_manifold_if_needed preserves clean manifold");
    expectTrue(clean.pointCount == 1u, "prune_manifold_if_needed leaves clean slots untouched");
        !fuse::physics::narrowphase::preflight_generate_contact_manifold(separated).can_generate(),
        "generate preflight rejects separated manifold");
}

void testManifoldPruneChainPreflightGuards() {
    clean.addPoint({1.f, 0.f, 0.f}, 0.25f);

    const auto cleanChain = fuse::physics::narrowphase::preflight_manifold_prune_chain(clean);
    expectTrue(!cleanChain.needs_pruning(), "prune chain preflight skips clean manifold");
        fuse::physics::narrowphase::prune_contact_points_if_needed(clean),
        "prune if needed preserves clean manifold");
    expectTrue(clean.pointCount == 2u, "prune if needed leaves clean slots untouched");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
        fuse::physics::narrowphase::prune_contact_manifold_with_preflight(dirty),
        "prune_with_preflight keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "prune_with_preflight removes separated slot");

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
void testContactPairGuardedDispatch() {

    const auto skipped =
        fuse::physics::narrowphase::detect_contacts_pair_guarded({bodyA, bodyA}, bodies, shapes);
    expectTrue(skipped.skipped, "guarded dispatch skips self pair");
    expectTrue(!skipped.detected, "guarded dispatch does not detect rejected pair");
        skipped.reason == fuse::physics::narrowphase::ContactPairRejectReason::SelfPair,
        "guarded dispatch reports self-pair reject reason");

    const auto detected =
        fuse::physics::narrowphase::detect_contacts_pair_result({bodyA, bodyB}, bodies, shapes);
    expectTrue(!detected.skipped, "guarded dispatch runs valid pair");
    expectTrue(detected.detected, "guarded dispatch marks valid pair as detected");
    expectTrue(detected.manifold.valid, "guarded dispatch returns valid overlapping manifold");

    const auto emptyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(empty);
        emptyPreflight.reason == fuse::physics::narrowphase::ManifoldFinalizeFailureReason::Empty,
        "finalize preflight reports Empty reason");
        fuse::physics::narrowphase::should_skip_manifold_finalize(empty),
        "should_skip_manifold_finalize true for empty manifold");

    const auto noNormalPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(noNormal);
        noNormalPreflight.reason ==
            fuse::physics::narrowphase::ManifoldFinalizeFailureReason::InvalidNormal,
        "finalize preflight reports InvalidNormal");

    fuse::physics::narrowphase::ContactManifold allSeparated{};
    allSeparated.contactNormal = {0.f, 1.f, 0.f};
    allSeparated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    const auto prunePreflight = fuse::physics::narrowphase::preflight_manifold_finalize(allSeparated);
        prunePreflight.reason ==
            fuse::physics::narrowphase::ManifoldFinalizeFailureReason::PruneWouldEmpty,
        "finalize preflight reports PruneWouldEmpty");

    fuse::physics::narrowphase::ContactManifold ready{};
    ready.contactNormal = {0.f, 1.f, 0.f};
    ready.addPoint({0.f, 0.f, 0.f}, 0.25f);
    const auto readyPreflight = fuse::physics::narrowphase::preflight_finalize_contact_manifold(ready);
    expectTrue(readyPreflight.can_finalize(), "finalize preflight accepts penetrating manifold");
        fuse::physics::narrowphase::generate_contact_manifold_guarded(ready),
        "guarded finalize succeeds for ready manifold");
    expectTrue(ready.valid, "guarded finalize sets validity");
    expectTrue(ready.hasFrictionBasis(), "guarded finalize builds friction basis");

    fuse::physics::narrowphase::ContactManifold guardedFail = separated;
        !fuse::physics::narrowphase::generate_contact_manifold_guarded(guardedFail),
        "guarded finalize fails for separated manifold");
    expectTrue(!guardedFail.valid, "guarded finalize clears validity on failure");

void testManifoldPruneGuardedDispatch() {
        fuse::physics::narrowphase::should_skip_manifold_prune(clean),
        "should_skip_manifold_prune true for clean manifold");
        fuse::physics::narrowphase::prune_contact_points_guarded(clean),
        "guarded prune preserves clean manifold");
    expectTrue(clean.pointCount == 1u, "guarded prune leaves clean slots untouched");

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
    const auto readyPreflight = fuse::physics::narrowphase::preflight_manifold_finalize(ready);
    expectTrue(readyPreflight.can_finalize(), "finalize preflight allows penetrating manifold");
        std::strcmp(
            fuse::physics::narrowphase::manifold_finalize_failure_reason_name(
                fuse::physics::narrowphase::ManifoldFinalizeFailureReason::None),
            "None") == 0,
        "finalize failure reason name for None");

void testManifoldFinalizeGuardedEntryPoints() {
    empty.valid = true;
    const auto emptyResult = fuse::physics::narrowphase::generate_contact_manifold_guarded(empty);
    expectTrue(emptyResult.skipped, "guarded finalize skips empty manifold");
    expectTrue(!emptyResult.finalized, "guarded finalize does not finalize empty manifold");
    expectTrue(!empty.valid, "guarded finalize clears invalid empty manifold");

    fuse::physics::narrowphase::ContactManifold manifold =
    const auto result = fuse::physics::narrowphase::generate_contact_manifold_result(manifold);
    expectTrue(result.finalized, "guarded finalize succeeds for valid detected manifold");
    expectTrue(!result.skipped, "guarded finalize does not skip valid manifold");
    expectTrue(manifold.hasFrictionBasis(), "guarded finalize builds friction basis");

    fuse::physics::narrowphase::ContactManifold skipCandidate =
        fuse::physics::narrowphase::generate_contact_manifold_if_needed(skipCandidate),
        "if_needed finalizes valid manifold");
    expectTrue(skipCandidate.valid, "if_needed leaves finalized manifold valid");

    fuse::physics::narrowphase::ContactManifold reject{};
    reject.contactNormal = {0.f, 1.f, 0.f};
    reject.addPoint({0.f, 0.f, 0.f}, -0.2f);
        !fuse::physics::narrowphase::generate_contact_manifold_if_needed(reject),
        "if_needed skips manifold that fails preflight");

void testManifoldPrunePreflightShallowFlag() {
        !fuse::physics::narrowphase::can_skip_manifold_prune(dirty),
        "can_skip_manifold_prune false when separated slots exist");
        fuse::physics::narrowphase::prune_manifold_if_needed(dirty),
        "prune_manifold_if_needed keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "prune_manifold_if_needed removes separated slot");

    const auto dirtyPreflight = fuse::physics::narrowphase::preflight_manifold_prune(dirty, 1e-6f, 1e-4f, 0.05f);
    expectTrue(!dirtyPreflight.needs_any_pruning(0.05f), "prune preflight clean after conditional prune");

    fuse::physics::narrowphase::ContactManifold shallow{};
    shallow.contactNormal = {0.f, 1.f, 0.f};
    shallow.addPoint({0.f, 0.f, 0.f}, 0.5f);
    shallow.addPoint({1.f, 0.f, 0.f}, 0.01f);

    const auto withoutShallow = fuse::physics::narrowphase::preflight_manifold_prune(shallow);
    expectTrue(!withoutShallow.hasShallow, "prune preflight omits shallow flag when depth unset");

    const auto withShallow = fuse::physics::narrowphase::preflight_manifold_prune(shallow, 1e-6f, 1e-4f, 0.05f);
    expectTrue(withShallow.hasShallow, "prune preflight flags shallow penetrations");

    const auto skipPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(empty);
    expectTrue(skipPreflight.skipped, "friction preflight skips empty manifold");
    expectTrue(skipPreflight.shouldSkip, "friction preflight marks shouldSkip for empty manifold");
        fuse::physics::narrowphase::should_skip_friction_basis_preflight(empty),
        "should_skip_friction_basis_preflight true for empty manifold");
        fuse::physics::narrowphase::prune_manifold_if_needed(shallow, 1e-6f, 1e-4f, 0.05f),
        "prune_manifold_if_needed handles shallow slots");
    expectTrue(shallow.pointCount == 1u, "prune_manifold_if_needed removes shallow slot");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.valid = true;
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
        !fuse::physics::narrowphase::finalize_contact_manifold_if_needed(separated),
        "finalize_if_needed no-ops on separated manifold");
    expectTrue(separated.valid, "finalize_if_needed leaves separated validity unchanged");

    const auto finalizePreflight = fuse::physics::narrowphase::preflight_manifold_finalize(manifold);
    expectTrue(finalizePreflight.needs_any_work(), "finalize preflight needs work before finalize");
        fuse::physics::narrowphase::finalize_contact_manifold_if_needed(manifold),
        "finalize_if_needed finalizes valid manifold");
    expectTrue(manifold.valid, "finalize_if_needed sets validity on success");
    expectTrue(manifold.hasFrictionBasis(), "finalize_if_needed builds friction basis");

void testFrictionBasisDeepenPreflightGuards() {
        !fuse::physics::narrowphase::can_run_friction_basis_rebuild(empty),
        "can_run_friction_basis_rebuild false for empty manifold");
        !fuse::physics::narrowphase::ensure_friction_basis_if_needed(empty),
        "ensure_friction_basis_if_needed false for empty manifold");
    dirty.addPoint({0.00001f, 0.f, 0.f}, 0.35f);
    const auto dirtyChain = fuse::physics::narrowphase::preflight_manifold_prune_chain(dirty);
    expectTrue(dirtyChain.needsSeparationPrune, "prune chain flags separated slot");
    expectTrue(dirtyChain.needsDuplicatePrune, "prune chain flags duplicate slot");
    expectTrue(dirtyChain.can_prune_in_place(), "prune chain can prune dirty manifold in place");
    expectTrue(!fuse::physics::narrowphase::can_skip_manifold_prune(dirty), "can_skip false for dirty manifold");
    expectTrue(dirty.pruneContactPointsIfNeeded(), "prune if needed keeps penetrating slots");
    expectTrue(dirty.pointCount == 1u, "prune if needed removes separated and duplicate slots");

void testManifoldFinalizeChainGuards() {
        !fuse::physics::narrowphase::finalize_contact_manifold_if_needed(empty),
        "finalize if needed no-ops on empty manifold");
    expectTrue(empty.valid, "finalize if needed leaves empty validity unchanged");

    const auto chainPreflight =
        fuse::physics::narrowphase::preflight_manifold_finalize_chain(manifold);
    expectTrue(!chainPreflight.skipped, "finalize chain preflight does not skip detected manifold");
    expectTrue(chainPreflight.can_finalize(), "finalize chain preflight can finalize detected manifold");
        "finalize if needed finalizes valid manifold");
    expectTrue(manifold.valid, "finalize if needed sets validity on success");
    expectTrue(manifold.hasFrictionBasis(), "finalize if needed builds friction basis");

void testFrictionBasisEnsurePreflightGuards() {
    const auto emptyEnsure = fuse::physics::narrowphase::preflight_friction_basis_ensure(empty);
    expectTrue(emptyEnsure.skipped, "ensure preflight skips empty manifold");
    expectTrue(emptyEnsure.can_skip_ensure(), "ensure preflight can skip empty manifold");
        "ensure if needed returns false for empty manifold");
        fuse::physics::narrowphase::manifold_prune_rejects_for_reason(
            separated, fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmptyAfterPrune),
        "separated manifold reports WouldBeEmptyAfterPrune prune reject reason");
            fuse::physics::narrowphase::manifold_prune_reject_reason_name(
                fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmptyAfterPrune),
            "WouldBeEmptyAfterPrune") == 0,
        "WouldBeEmptyAfterPrune prune reject reason has stable label");

void testManifoldFinalizeRejectReasonGuards() {
            empty, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::EmptyManifold),
        "empty manifold reports EmptyManifold finalize reject reason");
        "should_run_manifold_finalize false on empty manifold");

            ready, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::None),
        "ready manifold reports None finalize reject reason");
        fuse::physics::narrowphase::should_run_manifold_finalize(ready),
        "should_run_manifold_finalize true for ready manifold");

            separated, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune),
        "separated manifold reports WouldBeEmptyAfterPrune finalize reject reason");
        "NoPenetratingPoints finalize reject reason has stable label");

void testFrictionBasisRebuildRejectReasonGuards() {
        fuse::physics::narrowphase::friction_basis_rebuild_rejects_for_reason(
            empty, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::EmptyManifold),
        "empty manifold reports EmptyManifold friction rebuild reject reason");
        fuse::physics::narrowphase::can_skip_friction_basis_rebuild_dispatch(empty),
        "can_skip friction rebuild dispatch on empty manifold");
        "should_run friction rebuild false on empty manifold");
            separated, fuse::physics::narrowphase::ManifoldPruneRejectReason::WouldBeEmpty),
        "separated manifold rejects for WouldBeEmpty");
        fuse::physics::narrowphase::should_run_manifold_prune(separated),
        "should_run_manifold_prune true when separated slot exists");

        "empty manifold finalize rejects for EmptyManifold");

        "ready manifold finalize rejects for None");
        "should_run_manifold_finalize true on ready manifold");

        "separated manifold finalize rejects for WouldBeEmptyAfterPrune");

        "empty manifold friction rebuild rejects for EmptyManifold");
        fuse::physics::narrowphase::can_skip_friction_basis_preflight(empty),
        "can_skip_friction_basis_preflight on empty manifold");
        "should_run_friction_basis_rebuild false on empty manifold");

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

    const auto needsPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(needsPreflight.can_rebuild(), "friction preflight can_rebuild without cached basis");
        fuse::physics::narrowphase::can_run_friction_basis_rebuild(needsBuild),
        "can_run_friction_basis_rebuild true without cached basis");
        fuse::physics::narrowphase::ensure_friction_basis_if_needed(needsBuild),
        "ensure_friction_basis_if_needed builds missing basis");
    expectTrue(needsBuild.hasFrictionBasis(), "ensure_friction_basis_if_needed stores orthonormal basis");

        "ensure_friction_basis_if_needed reuses valid basis");
    const auto needsPreflight = fuse::physics::narrowphase::preflight_friction_basis_ensure(needsBuild);
    expectTrue(!needsPreflight.skipped, "ensure preflight does not skip valid manifold");
    expectTrue(needsPreflight.needsEnsure, "ensure preflight needs build without cached basis");
        "ensure if needed builds orthonormal frame");
        fuse::physics::narrowphase::friction_basis_matches_normal(needsBuild),
        "ensure if needed produces basis matching normal");

        "ensure if needed reuses cached basis");
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
    const auto missingPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(missingPreflight.missing, "friction preflight flags missing basis");
    expectTrue(missingPreflight.needs_rebuild(), "friction preflight needs rebuild when basis missing");

    needsBuild.buildFrictionBasis();
    const auto freshPreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(freshPreflight.can_reuse(), "friction preflight can reuse valid basis");

    needsBuild.contactNormal = {1.f, 0.f, 0.f};
    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_rebuild(needsBuild);
    expectTrue(stalePreflight.stale, "friction preflight flags stale basis after normal change");
    expectTrue(stalePreflight.needs_rebuild(), "friction preflight needs rebuild when stale");

void testContactBufferFrictionTangentBasesIfNeeded() {
    buffer.reserve(2u);

    fuse::physics::narrowphase::generate_contact_manifold(manifold);
    buffer.writeSlot(0u, manifold);

    buffer.buildFrictionTangentBasesIfNeeded();
    const auto basisAfterFirst = buffer.tangentBasisAt(0u);
        fuse::physics::narrowphase::isOrthonormalTangentBasis(manifold.contactNormal, basisAfterFirst),
        "if_needed rebuild builds orthonormal basis on first pass");

    const auto cachedTangent1 = basisAfterFirst.tangent1;
    const auto basisAfterSecond = buffer.tangentBasisAt(0u);
        basisAfterSecond.tangent1.x,
        "if_needed preserves cached basis on second pass");
        "ensure_friction_basis_if_needed preserves cached tangent1");

        "can_run_friction_basis_rebuild true for stale basis");
        "ensure_friction_basis_if_needed rebuilds stale basis");
        "ensure_friction_basis_if_needed matches current normal after rebuild");
        "ensure if needed preserves cached tangent1");

    const auto stalePreflight = fuse::physics::narrowphase::preflight_friction_basis_ensure(needsBuild);
    expectTrue(stalePreflight.stale, "ensure preflight flags stale basis");
    expectTrue(!stalePreflight.can_skip_ensure(), "ensure preflight cannot skip stale basis");
        "rebuild with preflight refreshes stale basis");
        fuse::physics::narrowphase::can_skip_friction_basis_ensure(needsBuild),
        "can_skip ensure true after rebuild");
        fuse::physics::narrowphase::friction_basis_rebuild_rejects_for_reason(
            needsBuild, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::None),
        "valid manifold without basis reports None friction rebuild reject reason");
        "should_run friction rebuild true without cached basis");

            needsBuild, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuse),
        "valid manifold with basis reports CanReuse friction rebuild reject reason");
        fuse::physics::narrowphase::can_skip_friction_basis_rebuild_dispatch(needsBuild),
        "can_skip friction rebuild dispatch with valid cached basis");
        std::strcmp(
            fuse::physics::narrowphase::friction_basis_rebuild_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuse),
            "CanReuse") == 0,
        "CanReuse friction rebuild reject reason has stable label");

void testContactBufferRejectReasonGuards() {
        "valid manifold friction rebuild rejects for None");

    fuse::physics::narrowphase::ContactManifold withBasis = needsBuild;
    withBasis.buildFrictionBasis();
            withBasis, fuse::physics::narrowphase::FrictionBasisRebuildRejectReason::CanReuse),
        "cached basis friction rebuild rejects for CanReuse");
        fuse::physics::narrowphase::can_skip_friction_basis_preflight(withBasis),
        "can_skip_friction_basis_preflight on valid basis");

void testContactBufferWriteRejectReasonGuards() {

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.penetrationDepth = 0.25f;

        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer,
            0u,
            valid,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRange),
        "write to unprepared buffer reports OutOfRange reject reason");

            fuse::physics::narrowphase::ContactBufferWriteRejectReason::None),
        "valid manifold reports None write reject reason");
    buffer.writeSlot(0u, valid);
    expectTrue(buffer.validFlags[0u] == 1u, "writeSlot accepts valid manifold through preflight gate");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
            1u,
            selfPair,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "self pair reports SelfPair write reject reason");

        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::None),
        "buffer with valid slot reports None compaction reject reason");
        fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRunContactBufferCompaction true when invalid slots remain");
    expectTrue(buffer.compact() == 1u, "compact gathers valid slot through preflight gate");

    buffer.clear();
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "cleared buffer reports EmptyBuffer compaction reject reason");
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipContactBufferCompaction on empty buffer");

    buffer.preparePairSlots(3u);
    buffer.writeSlot(1u, valid);
    buffer.writeSlot(2u, valid);
    buffer.setMaxCapacity(2u);
        fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::None),
        "overflow buffer reports None clamp reject reason");
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRunContactBufferClamp true when overflow exists");
    expectTrue(buffer.applyMaxCapacityClamp() == 2u, "applyMaxCapacityClamp drops excess through preflight gate");
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
        "clamped buffer reports WithinCapacity clamp reject reason");
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
            "SelfPair") == 0,
        "SelfPair write reject reason has stable label");
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_rejects_for_reason(
            buffer, 0u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::None),
        fuse::physics::narrowphase::should_run_contact_buffer_write(buffer, 0u, valid),
        "should_run_contact_buffer_write true for valid manifold");

    fuse::physics::narrowphase::ContactManifold invalid{};
            buffer, 0u, invalid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "invalid manifold reports InvalidManifold write reject reason");
        fuse::physics::narrowphase::can_skip_contact_buffer_write(buffer, 0u, invalid),
        "can_skip_contact_buffer_write on invalid manifold");

            buffer, 0u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),

            buffer, 9u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidSlot),
        "out-of-range slot reports InvalidSlot write reject reason");
}

void testContactBufferCompactionRejectReasonGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
        fuse::physics::narrowphase::contact_buffer_compaction_rejects_for_reason(
        "empty buffer reports EmptyBuffer compaction reject reason");
        fuse::physics::narrowphase::can_skip_contact_buffer_compaction(buffer),
        "can_skip_contact_buffer_compaction on empty buffer");

    buffer.preparePairSlots(2u);
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllInvalid),
        "all-invalid slots report AllInvalid compaction reject reason");
        !fuse::physics::narrowphase::should_run_contact_buffer_compaction(buffer),
        "should_run_contact_buffer_compaction false when all slots invalid");

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
        fuse::physics::narrowphase::should_run_contact_buffer_compaction(buffer),
        "should_run_contact_buffer_compaction true when valid slot exists");
    expectTrue(buffer.compact() == 1u, "compaction preflight preserves valid slot");

void testRunNarrowphaseDeepenIntoBufferGuards() {
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

    fuse::physics::narrowphase::ContactBufferSoA deepenBuffer;
    deepenBuffer.reserve(static_cast<fuse::u32>(pairs.size()));
    fuse::physics::narrowphase::runNarrowphaseDeepenIntoBuffer(pairs, bodies, shapes, deepenBuffer);
    expectTrue(deepenBuffer.activeCount == 1u, "deepen buffer skips deepen-rejected pair");

    fuse::physics::narrowphase::ContactBufferSoA baseBuffer;
    baseBuffer.reserve(static_cast<fuse::u32>(pairs.size()));
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(pairs, bodies, shapes, baseBuffer);
    expectTrue(baseBuffer.activeCount >= 1u, "base buffer still dispatches valid pair");

    const auto deepenManifold =
        fuse::physics::narrowphase::detect_contacts_pair_deepen({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!deepenManifold.valid, "detect_contacts_pair_deepen rejects sleeping pair");
    const auto validManifold =
        fuse::physics::narrowphase::detect_contacts_pair_deepen({dynamicA, dynamicB}, bodies, shapes);
    expectTrue(validManifold.valid, "detect_contacts_pair_deepen allows valid pair");
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
        fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch(
            {dynamicA, dynamicB}, bodies, shapes),
        "should_run deepen dispatch allows valid pair");
    expectTrue(
        !fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch(
            {sleepingA, sleepingB}, bodies, shapes),
        "should_run deepen dispatch rejects both-sleeping pair");

    const auto rejectedManifold = fuse::physics::narrowphase::detect_contacts_pair_with_preflight(
        {sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!rejectedManifold.valid, "detect_with_preflight rejects deepen-rejected pair");

    const auto validManifold = fuse::physics::narrowphase::detect_contacts_pair_with_preflight(
        {dynamicA, dynamicB}, bodies, shapes);
    expectTrue(validManifold.valid, "detect_with_preflight allows valid pair");

    const fuse::u32 capsuleA = bodies.addBody({0.f, 4.f, 0.f}, 1.f);
    const fuse::u32 capsuleB = bodies.addBody({0.f, 6.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Capsule, capsuleA, {0.5f, 1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Capsule, capsuleB, {0.5f, 1.f, 0.f});
    expectTrue(
        fuse::physics::narrowphase::is_capsule_capsule_contact_pair({capsuleA, capsuleB}, shapes),
        "capsule-capsule guard flags both-capsule pair");

    const fuse::u32 boxBody = bodies.addBody({0.f, -1.f, 0.f}, 1.f);
    const fuse::u32 planeBody = bodies.addBody({0.f, -2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Box, boxBody, {1.f, 1.f, 1.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeBody, {0.f, 1.f, 0.f});
    expectTrue(
        fuse::physics::narrowphase::is_box_plane_contact_pair({boxBody, planeBody}, shapes),
        "box-plane guard flags unsupported pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> allRejected = {
        {sleepingA, sleepingB},
    };
    expectTrue(
        fuse::physics::narrowphase::first_contact_pair_deepen_reject_in_batch(
            allRejected, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping,
        "first deepen reject reports BothSleeping when all rejected");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixed = {
        {sleepingA, sleepingB},
        {dynamicA, dynamicB},
    };
    expectTrue(
        fuse::physics::narrowphase::first_contact_pair_deepen_reject_in_batch(mixed, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::None,
        "first deepen reject returns None when one pair dispatchable");
}

void testContactBufferDeepenPassPreflights() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    fuse::physics::narrowphase::generate_contact_manifold(valid);

    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferWrite(buffer, 0u, valid),
        "shouldRun write true for valid manifold");
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 4u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidSlot),
        "write rejects_for_reason flags invalid slot");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 0u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write rejects_for_reason flags self pair");

    fuse::physics::narrowphase::ContactManifold invalid{};
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 0u, invalid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "write rejects_for_reason flags invalid manifold");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
            "InvalidManifold") == 0,
        "write reject reason name resolves InvalidManifold");

    buffer.writeSlot(0u, valid);
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRun compaction true when invalid slots remain");
    expectTrue(
        !fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllInvalid),
        "compaction rejects_for_reason does not false-positive partial buffer");

    fuse::physics::narrowphase::ContactBufferSoA allInvalid;
    allInvalid.preparePairSlots(2u);
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            allInvalid, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllInvalid),
        "compaction rejects_for_reason flags all-invalid buffer");

    fuse::physics::narrowphase::ContactBufferSoA empty{};
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            empty, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "compaction rejects_for_reason flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferClamp(empty),
        "canSkipClamp true for empty buffer");

    fuse::physics::narrowphase::ContactManifold validB =
        fuse::physics::narrowphase::collideSphereSphere({3.f, 0.f, 0.f}, 1.f, {4.5f, 0.f, 0.f}, 1.f, 2u, 3u);
    fuse::physics::narrowphase::generate_contact_manifold(validB);
    buffer.writeSlot(1u, validB);
    buffer.compact();
    expectTrue(buffer.activeCount == 2u, "compact gathers both valid slots");
    buffer.setMaxCapacity(1u);
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRun clamp true when active exceeds max capacity");
    buffer.applyMaxCapacityClamp();
    expectTrue(buffer.activeCount == 1u, "clamp reduces active count to max capacity");
    expectTrue(
        fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
        "clamp rejects_for_reason flags within-capacity buffer");
}

void testManifoldFrictionDeepenPassPredicates() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_prune(clean),
        "should_run prune false for clean manifold");
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_finalize(clean),
        "should_run finalize true for penetrating manifold");

    fuse::physics::narrowphase::ContactManifold separated{};
    separated.contactNormal = {0.f, 1.f, 0.f};
    separated.addPoint({0.f, 0.f, 0.f}, -0.1f);
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_finalize(separated),
        "should_run finalize false for separated manifold");

    fuse::physics::narrowphase::ContactManifold needsBasis{};
    needsBasis.contactNormal = {0.f, 1.f, 0.f};
    needsBasis.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBasis),
        "should_run friction rebuild true without cached basis");
    fuse::physics::narrowphase::compute_friction_tangents_with_preflight(needsBasis);
    expectTrue(needsBasis.hasFrictionBasis(), "compute_with_preflight builds friction basis");
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBasis),
        "should_run friction rebuild false after basis built");

    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});
    const std::vector<fuse::physics::broadphase::CandidatePair> validPairs = {{bodyA, bodyB}};

    fuse::physics::narrowphase::ContactBufferSoA buffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBufferWithDeepenPreflight(
        validPairs, bodies, shapes, buffer);
    expectTrue(buffer.activeCount == 1u, "deepen preflight narrowphase produces one contact");

    const fuse::u32 sleepingA = bodies.addBody({0.f, 2.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    const fuse::u32 sleepingB = bodies.addBody({0.f, 3.f, 0.f}, 1.f, fuse::physics::RB_SLEEPING);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sleepingB, {1.f, 0.f, 0.f});
    fuse::physics::narrowphase::ContactBufferSoA rejectedBuffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBufferWithDeepenPreflight(
        {{sleepingA, sleepingB}}, bodies, shapes, rejectedBuffer);
    expectTrue(rejectedBuffer.isEmpty(), "deepen preflight narrowphase skips rejected pairs");
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
        fuse::physics::narrowphase::can_skip_contact_pair_deepen_dispatch({sleepingA, sleepingB}, bodies, shapes),
        "can_skip deepen dispatch mirrors should_skip for sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({sleepingA, sleepingB}, bodies, shapes),
        "should_run deepen dispatch false for sleeping pair");
    expectTrue(
        fuse::physics::narrowphase::should_run_contact_pair_deepen_dispatch({dynamicA, dynamicB}, bodies, shapes),
        "should_run deepen dispatch true for valid pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {dynamicA, dynamicB},
    };
    expectTrue(
        fuse::physics::narrowphase::narrowphase_batch_rejects_for_reason(
            mixedPairs,
            bodies,
            shapes,
            fuse::physics::narrowphase::NarrowphaseBatchRejectReason::None),
        "batch reject reason None when one pair dispatchable");
    expectTrue(
        fuse::physics::narrowphase::narrowphase_batch_rejects_for_reason(
            {{sleepingA, sleepingB}},
            bodies,
            shapes,
            fuse::physics::narrowphase::NarrowphaseBatchRejectReason::AllRejected),
        "batch reject reason AllRejected when all deepen-rejected");
    expectTrue(
        fuse::physics::narrowphase::narrowphase_batch_rejects_for_reason(
            {},
            bodies,
            shapes,
            fuse::physics::narrowphase::NarrowphaseBatchRejectReason::EmptyPairList),
        "batch reject reason EmptyPairList for empty input");

    const auto batchPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_batch(mixedPairs, bodies, shapes);
    expectTrue(batchPreflight.can_run(), "batch preflight can_run with dispatchable pair");
    expectTrue(
        fuse::physics::narrowphase::should_run_narrowphase_batch(mixedPairs, bodies, shapes),
        "should_run_narrowphase_batch true when one pair dispatchable");
    expectTrue(
        !fuse::physics::narrowphase::should_run_narrowphase_batch({{sleepingA, sleepingB}}, bodies, shapes),
        "should_run_narrowphase_batch false when all rejected");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::narrowphase_batch_reject_reason_name(
                fuse::physics::narrowphase::NarrowphaseBatchRejectReason::AllRejected),
            "AllRejected") == 0,
        "batch reject reason name resolves AllRejected");
}

void testManifoldPruneFinalizePassGuards() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        fuse::physics::narrowphase::can_skip_manifold_prune(clean),
        "can_skip_manifold_prune true for clean manifold");
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_prune(clean),
        "should_run_manifold_prune false for clean manifold");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_prune(dirty),
        "should_run_manifold_prune true when separated slot present");

    fuse::physics::narrowphase::ContactManifold ready =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::should_run_manifold_finalize(ready),
        "should_run_manifold_finalize true for penetrating manifold");
    const fuse::physics::narrowphase::ContactManifold emptyFinalize{};
    expectTrue(
        !fuse::physics::narrowphase::should_run_manifold_finalize(emptyFinalize),
        "should_run_manifold_finalize false for empty manifold");
}

void testFrictionBasisRebuildPassGuards() {
    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run_friction_basis_rebuild true without cached basis");

    needsBuild.buildFrictionBasis();
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(needsBuild),
        "should_run_friction_basis_rebuild false with valid cached basis");

    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::should_run_friction_basis_rebuild(empty),
        "should_run_friction_basis_rebuild false for empty manifold");
}

void testContactBufferCompactClampPassGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_compact_rejects_for_reason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactRejectReason::EmptyBuffer),
        "compact rejects_for_reason flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_compact(buffer),
        "can_skip_contact_buffer_compact on empty buffer");

    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold manifold =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    fuse::physics::narrowphase::generate_contact_manifold(manifold);
    buffer.writeSlot(1u, manifold);
    expectTrue(
        fuse::physics::narrowphase::should_run_contact_buffer_compact(buffer),
        "should_run_contact_buffer_compact when valid slot has gap");
    expectTrue(buffer.compact() == 1u, "compact gathers valid slot through preflight gate");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_compact(buffer),
        "can_skip_contact_buffer_compact after compaction");

    buffer.setMaxCapacity(0u);
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_clamp_rejects_for_reason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
        "clamp rejects_for_reason flags within-capacity buffer");
    buffer.setMaxCapacity(1u);
    expectTrue(
        !fuse::physics::narrowphase::should_run_contact_buffer_clamp(buffer),
        "should_run_contact_buffer_clamp false within capacity");

    fuse::physics::narrowphase::ContactBufferSoA overflow;
    overflow.preparePairSlots(2u);
    overflow.writeSlot(0u, manifold);
    overflow.writeSlot(1u, manifold);
    overflow.compact();
    overflow.setMaxCapacity(1u);
    expectTrue(
        fuse::physics::narrowphase::should_run_contact_buffer_clamp(overflow),
        "should_run_contact_buffer_clamp when over capacity");
    expectTrue(overflow.applyMaxCapacityClamp() == 1u, "clamp drops excess through preflight gate");
}

void testContactBufferFrictionBuildPassGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_friction_build_rejects_for_reason(
            buffer, fuse::physics::narrowphase::ContactBufferFrictionBuildRejectReason::EmptyBuffer),
        "friction build rejects_for_reason flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_friction_build(buffer),
        "can_skip_contact_buffer_friction_build on empty buffer");

    buffer.preparePairSlots(1u);
    fuse::physics::narrowphase::ContactManifold manifold =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    fuse::physics::narrowphase::generate_contact_manifold(manifold);
    buffer.writeSlot(0u, manifold);
    buffer.compact();
    expectTrue(
        fuse::physics::narrowphase::should_run_contact_buffer_friction_build(buffer),
        "should_run_contact_buffer_friction_build with valid slot");
    buffer.buildFrictionTangentBases();
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis(
            buffer.contactNormals[0],
            {buffer.tangent1[0], buffer.tangent2[0]}),
        "friction build stores orthonormal basis through preflight gate");

    const auto compactAndClampPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_compact_and_clamp(buffer);
    expectTrue(
        compactAndClampPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork,
        "compact-and-clamp preflight NoWork after compact");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_compact_and_clamp(buffer),
        "can_skip_contact_buffer_compact_and_clamp when already compact");
}

void testContactBufferWritePreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    const auto validPreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, valid);
    expectTrue(validPreflight.canWrite(), "write preflight accepts valid manifold");
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectReason(buffer, 0u, valid) ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::None,
        "write reject reason None for valid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer,
            0u,
            selfPair,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write rejects_for_reason flags self pair");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
            "SelfPair") == 0,
        "SelfPair write reject reason has stable label");

    fuse::physics::narrowphase::ContactManifold invalid{};
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer,
            0u,
            invalid,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "write rejects_for_reason flags invalid manifold");

    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer,
            4u,
            valid,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidSlot),
        "write rejects_for_reason flags out-of-range slot");

    buffer.writeSlot(0u, valid);
    expectTrue(buffer.slotIsValid(0u), "slotIsValid true after write");
    buffer.invalidateSlot(0u);
    expectTrue(!buffer.slotIsValid(0u), "slotIsValid false after invalidate");
}

void testContactBufferCompactionPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer,
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "empty buffer reports EmptyBuffer compaction reject reason");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipContactBufferCompaction on empty buffer");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferCompactionRejectReasonName(
                fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
            "AllValid") == 0,
        "AllValid compaction reject reason has stable label");

    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(1u, manifold);

    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer,
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "contiguous valid slots report AllValid compaction reject reason");
    expectEq(buffer.compact(), 2u, "compact early-outs via preflight on all-valid slots");

    buffer.preparePairSlots(3u);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(2u, manifold);
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRunContactBufferCompaction true when invalid gap exists");
    expectEq(buffer.compact(), 2u, "compact gathers valid slots across gap");
}

void testContactBufferClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(
        fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer,
            fuse::physics::narrowphase::ContactBufferClampRejectReason::EmptyBuffer),
        "empty buffer reports EmptyBuffer clamp reject reason");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferClamp(buffer),
        "canSkipContactBufferClamp on empty buffer");

    buffer.setMaxCapacity(2u);
    buffer.preparePairSlots(1u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.penetrationDepth = 0.5f;
    manifold.addPoint({0.f, 0.f, 0.f}, 0.5f);
    buffer.writeSlot(0u, manifold);
    buffer.compact();
    expectTrue(
        fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer,
            fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
        "within-capacity buffer reports WithinCapacity clamp reject reason");
    expectTrue(buffer.canSkipMaxCapacityClamp(), "canSkipMaxCapacityClamp when within capacity");

    buffer.preparePairSlots(3u);
    fuse::physics::narrowphase::ContactManifold shallow = manifold;
    shallow.bodyB = 2u;
    shallow.penetrationDepth = 0.1f;
    shallow.points[0].penetration = 0.1f;
    fuse::physics::narrowphase::ContactManifold deep = manifold;
    deep.bodyB = 3u;
    deep.penetrationDepth = 0.9f;
    deep.points[0].penetration = 0.9f;
    buffer.writeSlot(0u, shallow);
    buffer.writeSlot(1u, deep);
    buffer.writeSlot(2u, manifold);
    buffer.compact();
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRunContactBufferClamp true when active exceeds max capacity");
    expectEq(buffer.applyMaxCapacityClamp(), 2u, "applyMaxCapacityClamp truncates via preflight gate");
}

void testContactBufferCompactAndClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactAndClampRejectsForReason(
            buffer,
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer),
        "empty buffer reports EmptyBuffer compact-and-clamp reject reason");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompactAndClamp(buffer),
        "canSkipContactBufferCompactAndClamp on empty buffer");
    expectEq(buffer.compactAndClamp(), 0u, "compactAndClamp early-outs via preflight on empty buffer");

    buffer.preparePairSlots(1u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.penetrationDepth = 0.4f;
    manifold.addPoint({0.f, 0.f, 0.f}, 0.4f);
    buffer.writeSlot(0u, manifold);
    buffer.compact();
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactAndClampRejectsForReason(
            buffer,
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork),
        "synced within-capacity buffer reports NoWork compact-and-clamp reject reason");
    expectEq(buffer.compactAndClamp(), 1u, "compactAndClamp no-op returns synced active count");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(1u, manifold);
    buffer.invalidateSlot(1u);
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferCompactAndClamp(buffer),
        "shouldRunContactBufferCompactAndClamp true when invalid slots exist");

    fuse::physics::narrowphase::ContactBufferSoA clampBuffer;
    clampBuffer.setMaxCapacity(1u);
    clampBuffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold shallow = manifold;
    shallow.bodyB = 2u;
    shallow.penetrationDepth = 0.1f;
    shallow.points[0].penetration = 0.1f;
    fuse::physics::narrowphase::ContactManifold deep = manifold;
    deep.bodyB = 3u;
    deep.penetrationDepth = 0.9f;
    deep.points[0].penetration = 0.9f;
    clampBuffer.writeSlot(0u, shallow);
    clampBuffer.writeSlot(1u, deep);
    expectEq(clampBuffer.compactAndClamp(), 1u, "compactAndClamp gathers then clamps via preflight gate");
    expectNear(clampBuffer.manifoldAt(0u).penetrationDepth, 0.9f, 1e-4f, "compactAndClamp keeps deepest contact");
}

void testContactBufferPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(buffer.canSkipSoAIteration(), "empty buffer can skip SoA iteration");
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "compaction rejects_for_reason flags empty buffer");

    const auto emptyCompactionPreflight =
        fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(emptyCompactionPreflight.emptyBuffer, "compaction preflight marks empty buffer");
    expectTrue(
        !emptyCompactionPreflight.needsCompaction(),
        "compaction preflight does not need compaction on empty buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkip compaction on empty buffer");

    buffer.preparePairSlots(2u);
    expectTrue(!buffer.canSkipSoAIteration(), "prepared buffer cannot skip SoA iteration");
    expectTrue(buffer.countValidSlots() == 0u, "prepared buffer has zero valid slots");
    expectTrue(buffer.canSkipCompactAndClamp(), "prepared buffer can skip compactAndClamp with no valid slots");

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.penetrationDepth = 0.5f;
    valid.addPoint({0.f, 0.f, 0.f}, 0.5f);

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;

    const auto invalidWritePreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, selfPair);
    expectTrue(!invalidWritePreflight.canWrite(), "write preflight rejects self pair");
    expectTrue(
        invalidWritePreflight.reason ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair,
        "write preflight reports SelfPair");

    const auto outOfRangeWritePreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 4u, valid);
    expectTrue(!outOfRangeWritePreflight.canWrite(), "write preflight rejects out-of-range slot");
    expectTrue(
        outOfRangeWritePreflight.reason ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidSlot,
        "write preflight reports InvalidSlot");

    fuse::physics::narrowphase::writeContactBufferSlotWithPreflight(buffer, 0u, valid);
    fuse::physics::narrowphase::writeContactBufferSlotWithPreflight(buffer, 1u, selfPair);
    expectTrue(buffer.countValidSlots() == 1u, "write_with_preflight keeps only valid manifold");
    expectTrue(buffer.slotIsValid(0u), "slot 0 valid after guarded write");
    expectTrue(!buffer.slotIsValid(1u), "slot 1 invalid after self-pair reject");
    expectTrue(
        !buffer.canSkipCompaction(),
        "buffer with invalid slot cannot skip compaction");

    fuse::physics::narrowphase::ContactManifold deeper = valid;
    deeper.bodyB = 2u;
    deeper.penetrationDepth = 0.9f;
    deeper.points[0].penetration = 0.9f;
    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::writeContactBufferSlotWithPreflight(buffer, 0u, valid);
    fuse::physics::narrowphase::writeContactBufferSlotWithPreflight(buffer, 1u, deeper);
    expectTrue(buffer.countValidSlots() == 2u, "two valid slots prepared for clamp test");
    expectTrue(buffer.canSkipCompaction(), "all-valid buffer can skip compaction");
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "compaction rejects_for_reason flags all-valid buffer");

    buffer.setMaxCapacity(1u);
    expectTrue(buffer.compact() == 2u, "compact gathers valid slots before clamp");
    expectTrue(buffer.canApplyMaxCapacityClamp(), "buffer can apply clamp when over capacity");
    const auto clampPreflight = fuse::physics::narrowphase::preflightContactBufferClamp(buffer);
    expectTrue(clampPreflight.needsClamp(), "clamp preflight needs clamp when over capacity");
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRun clamp when over capacity");

    expectTrue(
        fuse::physics::narrowphase::clampContactBufferWithPreflight(buffer) == 1u,
        "clamp_with_preflight clamps to max capacity");
    expectTrue(buffer.droppedCount == 1u, "clamp_with_preflight tracks dropped contacts");
    expectNear(buffer.manifoldAt(0u).penetrationDepth, 0.9f, 1e-4f, "clamp keeps deepest penetration");

    buffer.clear();
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactAndClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer),
        "compactAndClamp rejects_for_reason flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::compactAndClampContactBufferWithPreflight(buffer) == 0u,
        "compactAndClamp_with_preflight early-outs on empty buffer");
}

void testContactPairDeepenPassGuards() {
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

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {dynamicA, dynamicB},
    };
    expectTrue(
        fuse::physics::narrowphase::first_contact_pair_deepen_rejects_for_reason(
            mixedPairs,
            bodies,
            shapes,
            fuse::physics::narrowphase::ContactPairRejectReason::BothSleeping),
        "first deepen reject reason matches first rejected pair");
    expectTrue(
        fuse::physics::narrowphase::first_contact_pair_deepen_reject_reason(
            {{dynamicA, dynamicB}}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::None,
        "first deepen reject reason is None for valid pair list");

    const auto sleepingManifold = fuse::physics::narrowphase::detect_contacts_pair_with_deepen_preflight(
        {sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingManifold.valid, "deepen preflight dispatch rejects both-sleeping pair");
    const auto validManifold = fuse::physics::narrowphase::detect_contacts_pair_with_deepen_preflight(
        {dynamicA, dynamicB}, bodies, shapes);
    expectTrue(validManifold.valid, "deepen preflight dispatch allows valid pair");

    const auto runPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_run(mixedPairs, bodies, shapes);
    expectTrue(runPreflight.can_run(), "run preflight can dispatch with one valid pair");
    expectTrue(runPreflight.batch.dispatchableCount == 1u, "run preflight reports dispatchable count");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_narrowphase_run(mixedPairs, bodies, shapes),
        "should_skip run false when one pair dispatchable");
    expectTrue(
        fuse::physics::narrowphase::should_skip_narrowphase_run({{sleepingA, sleepingB}}, bodies, shapes),
        "should_skip run true when all pairs deepen-rejected");
    expectTrue(
        fuse::physics::narrowphase::should_skip_narrowphase_run({}, bodies, shapes),
        "should_skip run true for empty pair list");
}

void testManifoldPruneFinalizeDeepenPassGuards() {
    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);

    expectTrue(
        fuse::physics::narrowphase::prune_and_finalize_contact_manifold_with_preflight(dirty),
        "prune_and_finalize_with_preflight keeps penetrating manifold");
    expectTrue(dirty.valid, "prune_and_finalize sets validity");
    expectTrue(dirty.hasFrictionBasis(), "prune_and_finalize builds friction basis");
    expectTrue(dirty.pointCount == 1u, "prune_and_finalize removes separated slot");

    fuse::physics::narrowphase::ContactManifold ready =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_with_preflight(ready),
        "generate_with_preflight finalizes valid manifold");
    expectTrue(ready.valid, "generate_with_preflight sets validity");
}

void testFrictionBasisDeepenPassGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    fuse::physics::narrowphase::compute_friction_tangents_with_preflight(empty);
    expectTrue(!empty.hasFrictionBasis(), "compute_with_preflight clears basis on empty manifold");
    expectTrue(
        !fuse::physics::narrowphase::ensure_friction_basis_with_preflight(empty),
        "ensure_with_preflight skips empty manifold");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_with_preflight(needsBuild),
        "ensure_with_preflight builds missing basis");
    expectTrue(needsBuild.hasFrictionBasis(), "ensure_with_preflight stores orthonormal basis");

    const auto cachedTangent1 = needsBuild.frictionBasis.tangent1;
    fuse::physics::narrowphase::compute_friction_tangents_with_preflight(needsBuild);
    expectNear(
        needsBuild.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "compute_with_preflight preserves cached tangent1");
}

void testContactBufferPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    const auto validWritePreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, valid);
    expectTrue(validWritePreflight.canWrite(), "write preflight allows valid manifold");
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 0u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::None),
        "write rejects_for_reason matches valid manifold");

    const auto outOfRangePreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 4u, valid);
    expectTrue(!outOfRangePreflight.canWrite(), "write preflight rejects out-of-range slot");
    expectTrue(outOfRangePreflight.outOfRangeSlot, "write preflight flags out-of-range slot");

    fuse::physics::narrowphase::ContactManifold invalid = valid;
    invalid.valid = false;
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer,
            0u,
            invalid,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "write rejects_for_reason flags invalid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer,
            0u,
            selfPair,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write rejects_for_reason flags self pair");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
            "OutOfRangeSlot") == 0,
        "write reject reason name resolves OutOfRangeSlot");

    buffer.writeSlot(0u, valid);
    expectTrue(buffer.compact() == 1u, "compact keeps valid slot via preflight gate");

    fuse::physics::narrowphase::ContactBufferSoA allValidBuffer;
    allValidBuffer.preparePairSlots(1u);
    allValidBuffer.writeSlot(0u, valid);
    allValidBuffer.compact();
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            allValidBuffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "compaction rejects_for_reason flags all-valid buffer");

    buffer.clear();
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "compaction rejects_for_reason flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipCompaction true for empty buffer");

    buffer.setMaxCapacity(1u);
    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, valid);
    fuse::physics::narrowphase::ContactManifold second = valid;
    second.bodyB = 2u;
    second.penetrationDepth = 0.5f;
    second.points[0].penetration = 0.5f;
    buffer.writeSlot(1u, second);
    expectTrue(buffer.compact() == 2u, "compact collects both valid slots before clamp");
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRunClamp true when active exceeds max capacity");
    expectTrue(buffer.applyMaxCapacityClamp() == 1u, "applyMaxCapacityClamp applies clamp via preflight gate");
    expectTrue(buffer.droppedCount == 1u, "dropped count tracks clamped contacts");

    buffer.clear();
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactAndClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer),
        "compact-and-clamp rejects_for_reason flags empty buffer");

    buffer.preparePairSlots(1u);
    buffer.writeSlot(0u, valid);
    buffer.activeCount = 1u;
    const auto noWorkPreflight =
        fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(buffer);
    expectTrue(noWorkPreflight.noWork, "compact-and-clamp preflight flags no-work buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompactAndClamp(buffer),
        "canSkipCompactAndClamp true when no work needed");

    const auto frictionPreflight = fuse::physics::narrowphase::preflightContactBufferFrictionBuild(buffer);
    expectTrue(frictionPreflight.canBuild(), "friction-build preflight allows valid contacts");
    fuse::physics::narrowphase::buildContactBufferFrictionTangentBasesWithPreflight(buffer);
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis(
            buffer.contactNormals[0u], buffer.tangentBasisAt(0u)),
        "friction-build preflight produces orthonormal basis");

    buffer.clear();
    expectTrue(
        fuse::physics::narrowphase::contactBufferFrictionBuildRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferFrictionBuildRejectReason::EmptyBuffer),
        "friction-build rejects_for_reason flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferFrictionBuild(buffer),
        "canSkipFrictionBuild true for empty buffer");
    expectTrue(
        !fuse::physics::narrowphase::rebuild_contact_buffer_friction_bases_with_preflight(buffer),
        "rebuild_contact_buffer_friction_bases_with_preflight skips empty buffer");
}

void testContactManifoldBufferWritePreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::contact_manifold_write_rejects_for_reason(
            empty, fuse::physics::narrowphase::ContactManifoldWriteRejectReason::EmptyManifold),
        "manifold write rejects_for_reason flags empty manifold");
    expectTrue(
        fuse::physics::narrowphase::should_skip_contact_manifold_buffer_write(empty),
        "should_skip_manifold_buffer_write on empty manifold");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::contact_manifold_write_rejects_for_reason(
            noNormal, fuse::physics::narrowphase::ContactManifoldWriteRejectReason::InvalidNormal),
        "manifold write rejects_for_reason flags invalid normal");

    fuse::physics::narrowphase::ContactManifold notFinalized{};
    notFinalized.contactNormal = {0.f, 1.f, 0.f};
    notFinalized.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::contact_manifold_write_rejects_for_reason(
            notFinalized, fuse::physics::narrowphase::ContactManifoldWriteRejectReason::NotFinalized),
        "manifold write rejects_for_reason flags not-finalized manifold");

    fuse::physics::narrowphase::ContactManifold ready =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    fuse::physics::narrowphase::generate_contact_manifold(ready);
    expectTrue(
        fuse::physics::narrowphase::can_write_contact_manifold_to_buffer(ready),
        "can_write true for finalized manifold");

    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(1u);
    expectTrue(
        fuse::physics::narrowphase::write_contact_manifold_to_buffer_with_preflight(buffer, 0u, ready),
        "write_with_preflight stores finalized manifold");
    expectTrue(buffer.compact() == 1u, "buffer retains written manifold");
}

void testContactPairDeepenDispatchGuards() {
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

    const auto validManifold = fuse::physics::narrowphase::detect_contacts_pair_deepen(
        {dynamicA, dynamicB}, bodies, shapes);
    expectTrue(validManifold.valid, "deepen dispatch produces contact for valid pair");

    const auto sleepingManifold = fuse::physics::narrowphase::detect_contacts_pair_deepen(
        {sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingManifold.valid, "deepen dispatch rejects both-sleeping pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {dynamicA, dynamicB},
    };
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBufferWithDeepenPreflight(
        mixedPairs, bodies, shapes, buffer);
    expectTrue(buffer.activeCount == 1u, "deepen narrowphase keeps one dispatchable contact");

    const auto deepenResults =
        fuse::physics::narrowphase::runNarrowphaseDeepen(mixedPairs, bodies, shapes);
    expectTrue(deepenResults.size() == 1u, "runNarrowphaseDeepen returns one contact");
    expectTrue(deepenResults[0u].hasFrictionBasis(), "runNarrowphaseDeepen finalizes friction basis");
}

void testContactPairDeepenPass6Guards() {
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
        fuse::physics::narrowphase::should_dispatch_contact_pair_deepen({dynamicA, dynamicB}, bodies, shapes),
        "should_dispatch allows valid deepen pair");
    expectTrue(
        !fuse::physics::narrowphase::should_dispatch_contact_pair_deepen({sleepingA, sleepingB}, bodies, shapes),
        "should_dispatch rejects deepen-rejected pair");

    const auto detected =
        fuse::physics::narrowphase::detect_contacts_pair_with_deepen_preflight({dynamicA, dynamicB}, bodies, shapes);
    expectTrue(detected.valid, "detect_with_deepen_preflight produces contact for valid pair");

    const auto rejected =
        fuse::physics::narrowphase::detect_contacts_pair_with_deepen_preflight({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!rejected.valid, "detect_with_deepen_preflight skips deepen-rejected pair");

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {dynamicA, dynamicB},
    };
    const auto filtered =
        fuse::physics::narrowphase::filter_dispatchable_contact_pairs(mixedPairs, bodies, shapes);
    expectTrue(filtered.size() == 1u, "filter_dispatchable keeps one valid pair");
    expectTrue(filtered[0].bodyA == dynamicA && filtered[0].bodyB == dynamicB, "filter_dispatchable keeps dynamic pair");
}

void testManifoldPruneFinalizePass6Guards() {
    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    dirty.addPoint({2.f, 0.f, 0.f}, 0.3f);
    dirty.addPoint({3.f, 0.f, 0.f}, 0.25f);
    dirty.points[4].point = {4.f, 0.f, 0.f};
    dirty.points[4].penetration = 0.35f;
    dirty.pointCount = 5u;

    const auto preflight = fuse::physics::narrowphase::preflight_manifold_prune(dirty);
    expectTrue(preflight.exceedsMaxPoints, "prune preflight flags exceeds-max-points manifold");
    expectTrue(
        fuse::physics::narrowphase::finalize_manifold_after_prune_with_preflight(dirty),
        "finalize_after_prune finalizes pruned manifold");
    expectTrue(dirty.valid, "finalize_after_prune sets validity");
    expectTrue(dirty.pointCount <= fuse::physics::narrowphase::kMaxContactPointsPerManifold,
               "finalize_after_prune caps point count");
    expectTrue(dirty.hasFrictionBasis(), "finalize_after_prune builds friction basis");
}

void testFrictionBasisPass6Preflights() {
    fuse::physics::narrowphase::ContactManifold stale{};
    stale.contactNormal = {0.f, 1.f, 0.f};
    stale.addPoint({0.f, 0.f, 0.f}, 0.2f);
    stale.frictionBasis = {fuse::physics::vec3{1.f, 1.f, 0.f}, fuse::physics::vec3{0.f, 0.f, 1.f}};

    expectTrue(
        fuse::physics::narrowphase::friction_basis_is_stale_but_rebuildable(stale),
        "stale_but_rebuildable flags orthonormal mismatch");

    fuse::physics::narrowphase::ContactManifold unnormalized = stale;
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::normalize_contact_normal_if_needed(unnormalized),
        "normalize_if_needed normalizes non-unit normal");
    expectNear(unnormalized.contactNormal.length(), 1.f, 1e-4f, "normalize_if_needed stores unit normal");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 2.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_after_normalize_with_preflight(needsBuild),
        "rebuild_after_normalize builds basis");
    expectTrue(needsBuild.hasFrictionBasis(), "rebuild_after_normalize stores orthonormal basis");
}

void testContactBufferPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    const auto invalidSlotPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 4u, valid);
    expectTrue(!invalidSlotPreflight.can_write(), "write preflight rejects invalid slot");
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_rejects_for_reason(
            buffer, 4u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidSlot),
        "write rejects_for_reason flags invalid slot");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    const auto selfPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 0u, selfPair);
    expectTrue(!selfPreflight.can_write(), "write preflight rejects self pair");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_buffer_write_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
            "SelfPair") == 0,
        "write reject reason name resolves SelfPair");

    expectTrue(buffer.writeSlotWithPreflight(0u, valid), "write_with_preflight accepts valid manifold");
    expectTrue(buffer.slotIsValid(0u), "write_with_preflight marks slot valid");

    fuse::physics::narrowphase::ContactManifold second = valid;
    second.bodyB = 2u;
    buffer.writeSlot(1u, second);
    buffer.validFlags[1u] = 0u;

    const auto compactionPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(compactionPreflight.needs_compaction(), "compaction preflight needs work with invalid slot");
    expectTrue(buffer.compactWithPreflight() == 1u, "compact_with_preflight keeps valid slot");

    buffer.setMaxCapacity(1u);
    buffer.preparePairSlots(2u);
    buffer.writeSlotWithPreflight(0u, valid);
    fuse::physics::narrowphase::ContactManifold deeper = valid;
    deeper.bodyB = 3u;
    deeper.penetrationDepth = 0.9f;
    deeper.points[0].penetration = 0.9f;
    buffer.writeSlotWithPreflight(1u, deeper);
    expectTrue(buffer.compactWithPreflight() == 2u, "compact before clamp gathers both slots");

    const auto clampPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(buffer);
    expectTrue(clampPreflight.needs_clamp(), "clamp preflight needs overflow truncation");
    expectTrue(buffer.compactAndClampWithPreflight() == 1u, "compact_and_clamp_with_preflight truncates");
    expectNear(buffer.manifoldAt(0u).penetrationDepth, 0.9f, 1e-4f, "clamp keeps deepest penetration");

    fuse::physics::narrowphase::ContactBufferSoA empty{};
    const auto frictionPreflight = fuse::physics::narrowphase::preflight_contact_buffer_friction_basis(empty);
    expectTrue(!frictionPreflight.can_rebuild(), "friction preflight rejects empty buffer");
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_friction_basis_rejects_for_reason(
            empty, fuse::physics::narrowphase::ContactBufferFrictionBasisRejectReason::EmptyBuffer),
        "friction rejects_for_reason flags empty buffer");
    expectTrue(
        buffer.buildFrictionTangentBasesWithPreflight(),
        "friction_with_preflight rebuilds for valid buffer");
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis(
            buffer.contactNormals[0u], buffer.tangentBasisAt(0u)),
        "friction_with_preflight stores orthonormal tangents");
}

void testContactBufferPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.3f);

    const auto validWritePreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, valid);
    expectTrue(validWritePreflight.canWrite(), "write preflight allows valid manifold");
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectReason(buffer, 0u, valid) ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::None,
        "write reject reason None for valid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer,
            0u,
            selfPair,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write rejects_for_reason flags self pair");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
            "OutOfRangeSlot") == 0,
        "write reject reason name resolves OutOfRangeSlot");

    buffer.writeSlot(0u, valid);
    buffer.writeSlot(1u, valid);
    valid.bodyB = 2u;
    const auto compactionPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(
        compactionPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid,
        "compaction preflight AllValid when all slots valid");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipCompaction true for all-valid buffer");

    fuse::physics::narrowphase::ContactBufferSoA empty{};
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            empty, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "compaction rejects_for_reason flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(empty).reason ==
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer,
        "compactAndClamp preflight EmptyBuffer on empty buffer");
    expectTrue(buffer.compactAndClamp() == 2u, "compactAndClamp keeps all-valid contacts");

    buffer.setMaxCapacity(1u);
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRunClamp true when active exceeds max capacity");
    expectTrue(buffer.compactAndClamp() == 1u, "compactAndClamp clamps to max capacity");
    expectTrue(buffer.droppedCount == 1u, "dropped count tracks clamped contacts");

    const auto frictionPreflight = fuse::physics::narrowphase::preflightContactBufferFrictionBases(buffer);
    expectTrue(frictionPreflight.needsRebuild(), "friction-bases preflight needs rebuild with valid slots");
    expectTrue(
        fuse::physics::narrowphase::buildContactBufferFrictionBasesWithPreflight(buffer),
        "buildFrictionBasesWithPreflight rebuilds tangents");
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis(
            buffer.contactNormals[0u],
            buffer.tangentBasisAt(0u)),
        "friction-bases preflight stores orthonormal tangents");
}

void testNarrowphaseIntoBufferPreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    const std::vector<fuse::physics::broadphase::CandidatePair> emptyPairs{};
    const auto emptyPreflight =
        fuse::physics::narrowphase::preflightNarrowphaseIntoBuffer(emptyPairs, bodies, shapes);
    expectTrue(emptyPreflight.emptyPairs, "into-buffer preflight marks empty pair list");
    expectTrue(
        fuse::physics::narrowphase::should_skip_narrowphase_into_buffer(emptyPairs, bodies, shapes),
        "should_skip_into_buffer on empty pair list");

    fuse::physics::narrowphase::ContactBufferSoA buffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(emptyPairs, bodies, shapes, buffer);
    expectTrue(buffer.isEmpty(), "into-buffer early-out leaves empty contact buffer");

    const std::vector<fuse::physics::broadphase::CandidatePair> validPairs = {{bodyA, bodyB}};
    const auto validPreflight =
        fuse::physics::narrowphase::preflightNarrowphaseIntoBuffer(validPairs, bodies, shapes);
    expectTrue(validPreflight.can_run(), "into-buffer preflight can run with valid pairs");
    expectTrue(validPreflight.dispatchableCount == 1u, "into-buffer preflight counts dispatchable pair");

    const auto slotPreflight = fuse::physics::narrowphase::preflight_narrowphase_pair_slot(
        0u, {bodyA, bodyA}, bodies, shapes);
    expectTrue(!slotPreflight.can_dispatch(), "pair-slot preflight rejects self pair");
    expectTrue(
        slotPreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::SelfPair,
        "pair-slot preflight reports SelfPair");
    expectTrue(
        fuse::physics::narrowphase::should_skip_narrowphase_pair_slot(0u, {bodyA, bodyA}, bodies, shapes),
        "should_skip_pair_slot on self pair");
}

void testManifoldPruneFinalizeIfNeededGuards() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(clean),
        "prune_if_needed keeps clean manifold");
    expectTrue(clean.pointCount == 1u, "prune_if_needed does not remove penetrating point");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(dirty),
        "prune_if_needed prunes separated slot");
    expectTrue(dirty.pointCount == 1u, "prune_if_needed removes separated slot");

    fuse::physics::narrowphase::ContactManifold ready =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::finalize_contact_manifold_if_needed(ready),
        "finalize_if_needed finalizes valid manifold");
    expectTrue(ready.valid, "finalize_if_needed sets validity");
    expectTrue(ready.hasFrictionBasis(), "finalize_if_needed builds friction basis");

    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::finalize_contact_manifold_if_needed(empty),
        "finalize_if_needed no-ops on empty manifold");
}

void testFrictionBasisEnsureWithPreflight() {
    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_with_preflight(unnormalized),
        "ensure_with_preflight builds basis for scaled normal");
    expectTrue(unnormalized.hasFrictionBasis(), "ensure_with_preflight stores orthonormal basis");
    expectNear(unnormalized.contactNormal.length(), 1.f, 1e-4f, "ensure_with_preflight normalizes contact normal");

    const auto cachedTangent1 = unnormalized.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_with_preflight(unnormalized),
        "ensure_with_preflight reuses valid basis");
    expectNear(
        unnormalized.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "ensure_with_preflight preserves cached tangent1");
}

void testContactBufferPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(buffer.canSkipSoAIteration(), "empty contact buffer skips SoA iteration");

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    const auto outOfRangeWrite =
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 0u, valid);
    expectTrue(
        outOfRangeWrite.reason ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot,
        "write preflight rejects out-of-range slot");
    expectTrue(!outOfRangeWrite.canWrite(), "write preflight cannot write out-of-range slot");

    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold invalid = valid;
    invalid.valid = false;
    const auto invalidWrite =
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 0u, invalid);
    expectTrue(
        invalidWrite.reason ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold,
        "write preflight rejects invalid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_rejects_for_reason(
            buffer, 1u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write rejects_for_reason flags self pair");

    const auto validWrite =
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 0u, valid);
    expectTrue(validWrite.canWrite(), "write preflight allows valid manifold");
    fuse::physics::narrowphase::write_contact_buffer_slot_with_preflight(buffer, 0u, valid);
    expectTrue(buffer.slotIsValid(0u), "write_with_preflight stores valid slot");

    const auto emptyCompaction =
        fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(
        emptyCompaction.reason !=
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer,
        "prepared buffer is not empty for compaction preflight");

    fuse::physics::narrowphase::ContactManifold second = valid;
    second.bodyB = 2u;
    buffer.writeSlot(1u, second);
    expectTrue(buffer.canSkipCompaction(), "contiguous valid slots skip compaction");

    fuse::physics::narrowphase::ContactBufferSoA sparseBuffer;
    sparseBuffer.preparePairSlots(3u);
    sparseBuffer.writeSlot(2u, second);
    expectTrue(!sparseBuffer.canSkipCompaction(), "sparse valid slots need compaction");
    const auto compactionPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_compaction(sparseBuffer);
    expectTrue(compactionPreflight.needsCompaction(), "compaction preflight needs compaction work");
    expectTrue(
        fuse::physics::narrowphase::compact_contact_buffer_with_preflight(sparseBuffer) == 1u,
        "compact_with_preflight gathers sparse valid slot");
    expectTrue(sparseBuffer.activeCount == 1u, "compact_with_preflight sets active count");
    expectTrue(sparseBuffer.slotIsValid(0u), "compact_with_preflight packs valid slot to front");

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, valid);
    buffer.writeSlot(1u, second);
    buffer.compact();

    buffer.setMaxCapacity(1u);
    expectTrue(buffer.canApplyMaxCapacityClamp(), "overflow buffer requests clamp");
    const auto clampPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(buffer);
    expectTrue(clampPreflight.needsClamp(), "clamp preflight needs clamp work");
    expectTrue(
        fuse::physics::narrowphase::apply_contact_buffer_max_capacity_clamp_with_preflight(buffer) == 1u,
        "clamp_with_preflight truncates overflow");
    expectTrue(buffer.droppedCount == 1u, "clamp_with_preflight tracks dropped contacts");

    fuse::physics::narrowphase::ContactBufferSoA compactBuffer;
    compactBuffer.preparePairSlots(2u);
    compactBuffer.writeSlot(0u, valid);
    compactBuffer.writeSlot(1u, second);
    compactBuffer.compact();
    const auto noWorkPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_compact_and_clamp(compactBuffer);
    expectTrue(
        noWorkPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork,
        "compact-and-clamp preflight reports NoWork when already compact and within capacity");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_compact_and_clamp(compactBuffer),
        "can_skip compact-and-clamp on clean buffer");

    fuse::physics::narrowphase::ContactBufferSoA tangentBuffer;
    tangentBuffer.preparePairSlots(1u);
    tangentBuffer.writeSlot(0u, valid);
    tangentBuffer.compact();
    tangentBuffer.tangent1[0u] = {};
    tangentBuffer.tangent2[0u] = {};
    expectTrue(
        !tangentBuffer.canSkipFrictionTangentBuild(),
        "buffer with cleared tangents needs friction tangent build");
    const auto tangentPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_friction_tangents(tangentBuffer);
    expectTrue(
        tangentPreflight.needsFrictionTangentBuild(),
        "friction tangent preflight needs build for stale tangents");
    fuse::physics::narrowphase::build_contact_buffer_friction_tangents_with_preflight(tangentBuffer);
    expectTrue(
        tangentBuffer.canSkipFrictionTangentBuild(),
        "friction tangent build_with_preflight leaves orthonormal tangents");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_buffer_friction_tangent_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferFrictionTangentRejectReason::AllValid),
            "AllValid") == 0,
        "friction tangent reject reason name resolves AllValid");
}

void testContactPairDispatchPreflightGuards() {
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

    const auto validPreflight = fuse::physics::narrowphase::preflight_contact_pair_dispatch(
        {dynamicA, dynamicB}, bodies, shapes);
    expectTrue(validPreflight.can_dispatch(), "dispatch preflight allows valid pair");
    expectTrue(!validPreflight.rejected, "dispatch preflight does not reject valid pair");

    const auto sleepingPreflight = fuse::physics::narrowphase::preflight_contact_pair_dispatch(
        {sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingPreflight.can_dispatch(), "dispatch preflight rejects both-sleeping pair");
    expectTrue(
        fuse::physics::narrowphase::should_skip_contact_pair_dispatch_preflight(
            {sleepingA, sleepingB}, bodies, shapes),
        "should_skip dispatch preflight on rejected pair");
}

void testManifoldShallowPrunePreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::manifold_shallow_prune_rejects_for_reason(
            empty,
            fuse::physics::narrowphase::ManifoldShallowPruneRejectReason::EmptyManifold,
            0.05f),
        "shallow prune rejects_for_reason flags empty manifold");

    fuse::physics::narrowphase::ContactManifold deepOnly{};
    deepOnly.contactNormal = {0.f, 1.f, 0.f};
    deepOnly.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::manifold_shallow_prune_rejects_for_reason(
            deepOnly,
            fuse::physics::narrowphase::ManifoldShallowPruneRejectReason::NoShallowPenetrations,
            0.05f),
        "shallow prune rejects_for_reason flags no shallow penetrations");

    fuse::physics::narrowphase::ContactManifold mixed{};
    mixed.contactNormal = {0.f, 1.f, 0.f};
    mixed.addPoint({0.f, 0.f, 0.f}, 0.2f);
    mixed.addPoint({1.f, 0.f, 0.f}, 0.02f);
    const auto shallowPreflight =
        fuse::physics::narrowphase::preflight_manifold_shallow_prune(mixed, 0.05f);
    expectTrue(shallowPreflight.can_prune(), "shallow prune preflight allows mixed manifold");
    expectTrue(shallowPreflight.hasShallow, "shallow prune preflight detects shallow slot");
    expectTrue(
        fuse::physics::narrowphase::prune_shallow_penetrations_with_preflight(mixed, 0.05f),
        "shallow prune_with_preflight keeps deep slot");
    expectTrue(mixed.pointCount == 1u, "shallow prune_with_preflight removes shallow slot");
}

void testContactNormalNormalizePreflightGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        fuse::physics::narrowphase::contact_normal_normalize_rejects_for_reason(
            empty,
            fuse::physics::narrowphase::ContactNormalNormalizeRejectReason::EmptyManifold),
        "normalize rejects_for_reason flags empty manifold");

    fuse::physics::narrowphase::ContactManifold unit{};
    unit.contactNormal = {0.f, 1.f, 0.f};
    unit.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::contact_normal_normalize_rejects_for_reason(
            unit,
            fuse::physics::narrowphase::ContactNormalNormalizeRejectReason::AlreadyUnit),
        "normalize rejects_for_reason flags already-unit normal");

    fuse::physics::narrowphase::ContactManifold unnormalized = unit;
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    const auto normalizePreflight =
        fuse::physics::narrowphase::preflight_contact_normal_normalize(unnormalized);
    expectTrue(normalizePreflight.can_normalize(), "normalize preflight allows non-unit normal");
    expectTrue(
        fuse::physics::narrowphase::normalize_contact_normal_with_preflight(unnormalized),
        "normalize_with_preflight normalizes contact normal");
    expectNear(unnormalized.contactNormal.length(), 1.f, 1e-4f, "normalize_with_preflight yields unit normal");
}

void testNarrowphaseDispatchPreflightGuards() {
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

    const std::vector<fuse::physics::broadphase::CandidatePair> mixedPairs = {
        {sleepingA, sleepingB},
        {dynamicA, dynamicB},
    };
    const auto dispatchPreflight =
        fuse::physics::narrowphase::preflight_run_narrowphase_into_buffer(mixedPairs, bodies, shapes);
    expectTrue(dispatchPreflight.can_dispatch(), "dispatch preflight can dispatch with one valid pair");
    expectTrue(!dispatchPreflight.can_skip(), "dispatch preflight does not skip mixed batch");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_narrowphase_dispatch(mixedPairs, bodies, shapes),
        "should_skip dispatch false for mixed batch");

    expectTrue(
        fuse::physics::narrowphase::should_skip_narrowphase_dispatch(
            {{sleepingA, sleepingB}}, bodies, shapes),
        "should_skip dispatch true when all pairs rejected");
}

void testContactBufferPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(buffer.canSkipSoAIteration(), "empty buffer skips SoA iteration");
    expectEq(buffer.countValidSlots(), 0u, "countValidSlots early-outs when empty");

    const auto emptyCompaction =
        fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(emptyCompaction.emptyBuffer, "compaction preflight marks empty buffer");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_compaction(buffer),
        "can_skip compaction on empty buffer");

    const auto emptyClamp = fuse::physics::narrowphase::preflight_contact_buffer_clamp(buffer);
    expectTrue(emptyClamp.emptyBuffer, "clamp preflight marks empty buffer");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_clamp(buffer),
        "can_skip clamp on empty buffer");

    const auto emptyFriction =
        fuse::physics::narrowphase::preflight_contact_buffer_friction_bases(buffer);
    expectTrue(emptyFriction.emptyBuffer, "friction-bases preflight marks empty buffer");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_friction_bases(buffer),
        "can_skip friction-bases on empty buffer");

    const auto emptyCompactClamp =
        fuse::physics::narrowphase::preflight_contact_buffer_compact_and_clamp(buffer);
    expectTrue(emptyCompactClamp.emptyBuffer, "compact-and-clamp preflight marks empty buffer");
    expectEq(
        fuse::physics::narrowphase::compact_and_clamp_contact_buffer_with_preflight(buffer),
        0u,
        "compact_and_clamp_with_preflight early-outs on empty buffer");

    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.5f);

    const auto validWrite =
        fuse::physics::narrowphase::preflight_contact_buffer_write_slot(buffer, 0u, valid);
    expectTrue(validWrite.canWrite(), "write-slot preflight accepts valid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_slot_rejects_for_reason(
            buffer,
            1u,
            selfPair,
            fuse::physics::narrowphase::ContactBufferWriteSlotRejectReason::SelfPair),
        "write-slot rejects_for_reason flags self pair");
    expectTrue(
        fuse::physics::narrowphase::should_skip_contact_buffer_write_slot(buffer, 1u, selfPair),
        "should_skip write-slot on self pair");
    buffer.writeSlot(0u, valid);
    buffer.writeSlot(1u, selfPair);
    expectTrue(buffer.countValidSlots() == 1u, "writeSlot ignores self pair via preflight");

    const auto needsCompaction =
        fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(needsCompaction.needsCompaction(), "compaction preflight needs work with hole in slots");
    expectEq(buffer.compact(), 1u, "compact gathers valid slot via preflight gate");

    const auto allValidCompaction =
        fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(allValidCompaction.allValid, "compaction preflight marks all-valid buffer");
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_compaction_rejects_for_reason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "compaction rejects_for_reason flags all-valid buffer");

    buffer.setMaxCapacity(1u);
    fuse::physics::narrowphase::ContactManifold shallow = valid;
    shallow.bodyB = 2u;
    shallow.penetrationDepth = 0.1f;
    shallow.points[0].penetration = 0.1f;
    fuse::physics::narrowphase::ContactManifold deep = valid;
    deep.bodyB = 3u;
    deep.penetrationDepth = 0.9f;
    deep.points[0].penetration = 0.9f;

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, shallow);
    buffer.writeSlot(1u, deep);
    buffer.compact();
    expectTrue(buffer.canApplyMaxCapacityClamp(), "overflow buffer requests post clamp");
    const auto needsClamp = fuse::physics::narrowphase::preflight_contact_buffer_clamp(buffer);
    expectTrue(needsClamp.needsClamp(), "clamp preflight needs work when over capacity");
    expectEq(buffer.applyMaxCapacityClamp(), 1u, "applyMaxCapacityClamp truncates via preflight gate");
    expectNear(buffer.manifoldAt(0u).penetrationDepth, 0.9f, 1e-4f, "clamp keeps deepest contact");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_buffer_clamp_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
            "WithinCapacity") == 0,
        "clamp reject reason name resolves WithinCapacity");
}

void testNarrowphaseDeepenPassGuards() {
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

    const auto sleepingManifold = fuse::physics::narrowphase::detect_contacts_pair_deepen(
        {sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingManifold.valid, "detect_contacts_pair_deepen rejects sleeping pair");

    const auto validManifold = fuse::physics::narrowphase::detect_contacts_pair_deepen(
        {dynamicA, dynamicB}, bodies, shapes);
    expectTrue(validManifold.valid, "detect_contacts_pair_deepen accepts valid pair");

    fuse::physics::narrowphase::ContactBufferSoA buffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer({{sleepingA, sleepingB}}, bodies, shapes, buffer);
    expectTrue(buffer.isEmpty(), "runNarrowphaseIntoBuffer early-outs when all pairs deepen-rejected");

    fuse::physics::narrowphase::ContactManifold ready =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_deepen(ready),
        "generate_contact_manifold_deepen finalizes valid manifold");
    expectTrue(ready.hasFrictionBasis(), "generate_contact_manifold_deepen builds friction basis");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::prune_and_finalize_contact_manifold_with_preflight(dirty),
        "prune_and_finalize_with_preflight keeps penetrating slots");
    expectTrue(dirty.valid, "prune_and_finalize_with_preflight sets validity");
    expectTrue(dirty.pointCount == 1u, "prune_and_finalize_with_preflight removes separated slot");

    fuse::physics::narrowphase::ContactManifold needsTangents{};
    needsTangents.contactNormal = {0.f, 1.f, 0.f};
    needsTangents.addPoint({0.f, 0.f, 0.f}, 0.2f);
    fuse::physics::narrowphase::compute_friction_tangents_with_preflight(needsTangents);
    expectTrue(needsTangents.hasFrictionBasis(), "compute_friction_tangents_with_preflight builds basis");

    needsTangents.buildFrictionBasis();
    const auto cachedTangent1 = needsTangents.frictionBasis.tangent1;
    fuse::physics::narrowphase::compute_friction_tangents_with_preflight(needsTangents);
    expectNear(
        needsTangents.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "compute_friction_tangents_with_preflight preserves cached basis");
}

void testContactBufferWritePreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    const auto validPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 0u, valid);
    expectTrue(validPreflight.can_write(), "write preflight accepts valid manifold");
    expectTrue(
        validPreflight.reason == fuse::physics::narrowphase::ContactBufferWriteRejectReason::None,
        "write preflight reports None for valid manifold");

    fuse::physics::narrowphase::ContactManifold invalid = valid;
    invalid.valid = false;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_rejects_for_reason(
            buffer, 0u, invalid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "write rejects_for_reason flags invalid manifold");
    expectTrue(
        fuse::physics::narrowphase::should_skip_contact_buffer_write(buffer, 0u, invalid),
        "write skip guard rejects invalid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_rejects_for_reason(
            buffer, 0u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write rejects_for_reason flags self pair");
    buffer.writeSlot(0u, selfPair);
    expectTrue(buffer.compact() == 0u, "writeSlot rejects self pair via preflight");

    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_rejects_for_reason(
            buffer,
            4u,
            valid,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
        "write rejects_for_reason flags out-of-range slot");
    expectTrue(
        fuse::physics::narrowphase::write_contact_buffer_slot_with_preflight(buffer, 1u, valid),
        "write_with_preflight stores valid manifold");
    expectTrue(buffer.slotIsValid(1u), "write_with_preflight marks slot valid");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_buffer_write_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
            "SelfPair") == 0,
        "write reject reason name resolves SelfPair");
}

void testContactBufferCompactionClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_compaction_rejects_for_reason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "compaction rejects_for_reason flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_clamp_rejects_for_reason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::EmptyBuffer),
        "clamp rejects_for_reason flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_compact_and_clamp_rejects_for_reason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer),
        "compact-and-clamp rejects_for_reason flags empty buffer");
    expectTrue(buffer.compactAndClamp() == 0u, "compactAndClamp early-outs on empty buffer via preflight");

    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(1u, manifold);

    const auto compactionPreflight = fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(!compactionPreflight.needs_compaction(), "compaction preflight skips all-valid slots");
    expectTrue(compactionPreflight.allValid, "compaction preflight marks all-valid buffer");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_compaction(buffer),
        "can_skip_compaction on all-valid buffer");

    buffer.setMaxCapacity(1u);
    expectTrue(buffer.compact() == 2u, "compact sets activeCount before clamp");
    const auto clampPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(buffer);
    expectTrue(clampPreflight.needs_clamp(), "clamp preflight requests overflow truncation");
    expectTrue(
        fuse::physics::narrowphase::should_run_contact_buffer_clamp(buffer),
        "should_run_clamp on overflow buffer");
    expectTrue(buffer.applyMaxCapacityClamp() == 1u, "applyMaxCapacityClamp truncates via preflight gate");
    expectTrue(buffer.droppedCount == 1u, "clamp tracks dropped contacts");

    fuse::physics::narrowphase::ContactBufferSoA withinBuffer;
    withinBuffer.setMaxCapacity(4u);
    withinBuffer.preparePairSlots(2u);
    withinBuffer.writeSlot(0u, manifold);
    withinBuffer.compact();
    const auto withinPreflight = fuse::physics::narrowphase::preflight_contact_buffer_clamp(withinBuffer);
    expectTrue(!withinPreflight.needs_clamp(), "clamp preflight skips when within capacity");
    expectTrue(withinPreflight.withinCapacity, "clamp preflight marks within-capacity buffer");

    fuse::physics::narrowphase::ContactBufferSoA sparseBuffer;
    sparseBuffer.preparePairSlots(3u);
    sparseBuffer.writeSlot(0u, manifold);
    sparseBuffer.writeSlot(2u, manifold);
    const auto sparseCompaction =
        fuse::physics::narrowphase::preflight_contact_buffer_compaction(sparseBuffer);
    expectTrue(sparseCompaction.needs_compaction(), "compaction preflight requests sparse gather");
    expectTrue(sparseBuffer.compact() == 2u, "compact gathers sparse valid slots");

    fuse::physics::narrowphase::ContactBufferSoA packedBuffer;
    packedBuffer.preparePairSlots(2u);
    packedBuffer.writeSlot(0u, manifold);
    packedBuffer.writeSlot(1u, manifold);
    packedBuffer.compact();
    const auto noWorkPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_compact_and_clamp(packedBuffer);
    expectTrue(
        noWorkPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork,
        "compact-and-clamp preflight reports NoWork on packed buffer");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_compact_and_clamp(packedBuffer),
        "can_skip_compact_and_clamp on packed buffer");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_buffer_compact_and_clamp_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork),
            "NoWork") == 0,
        "compact-and-clamp reject reason name resolves NoWork");
}

void testContactBufferWritePreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    const auto validPreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, valid);
    expectTrue(validPreflight.canWrite(), "write preflight allows valid manifold");
    expectTrue(
        buffer.writeSlotWithPreflight(0u, valid),
        "writeSlotWithPreflight succeeds for valid manifold");

    fuse::physics::narrowphase::ContactManifold invalid = valid;
    invalid.valid = false;
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 1u, invalid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "write reject reason flags invalid manifold");
    expectTrue(
        !buffer.writeSlotWithPreflight(1u, invalid),
        "writeSlotWithPreflight rejects invalid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 1u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write reject reason flags self pair");

    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 99u, valid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
        "write reject reason flags out-of-range slot");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferWrite(buffer, 99u, valid),
        "canSkipContactBufferWrite on out-of-range slot");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
            "SelfPair") == 0,
        "write reject reason name resolves SelfPair");
}

void testContactBufferCompactionPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    const auto emptyPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(
        emptyPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer,
        "compaction preflight flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipCompaction on empty buffer");

    buffer.preparePairSlots(3u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(2u, manifold);

    const auto needsPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(needsPreflight.needsCompaction(), "compaction preflight needs work with holes");
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRunCompaction when invalid slots exist");
    expectTrue(buffer.compact() == 2u, "compact removes invalid slot");

    const auto cleanPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(
        cleanPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid,
        "compaction preflight all-valid after compact");
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "compaction rejects_for_reason matches all-valid buffer");
}

void testContactBufferClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.setMaxCapacity(2u);
    buffer.preparePairSlots(3u);

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

    buffer.writeSlot(0u, shallow);
    buffer.writeSlot(1u, deep);
    buffer.writeSlot(2u, shallow);

    const auto withinPreflight = fuse::physics::narrowphase::preflightContactBufferClamp(buffer);
    expectTrue(
        withinPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity,
        "clamp preflight within capacity before compact");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferClamp(buffer),
        "canSkipClamp before compact");

    buffer.compact();
    const auto needsPreflight = fuse::physics::narrowphase::preflightContactBufferClamp(buffer);
    expectTrue(needsPreflight.needsClamp(), "clamp preflight needs work after compact overflow");
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRunClamp when active exceeds max capacity");
    expectTrue(buffer.applyMaxCapacityClamp() == 2u, "clamp truncates to max capacity");
    expectTrue(buffer.hasDroppedContacts(), "clamp sets dropped count");
}

void testContactBufferCompactAndClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    const auto emptyPreflight = fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(buffer);
    expectTrue(
        emptyPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer,
        "compact-and-clamp preflight flags empty buffer");

    buffer.setMaxCapacity(1u);
    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.penetrationDepth = 0.5f;
    manifold.addPoint({0.f, 0.f, 0.f}, 0.5f);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(1u, manifold);

    const auto workPreflight = fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(buffer);
    expectTrue(workPreflight.needsCompactAndClamp(), "compact-and-clamp preflight needs work");
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferCompactAndClamp(buffer),
        "shouldRunCompactAndClamp with holes and overflow");
    expectTrue(buffer.compactAndClamp() == 1u, "compactAndClamp keeps deepest contact");
}

void testContactBufferFrictionRebuildPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    const auto emptyPreflight = fuse::physics::narrowphase::preflightContactBufferFrictionRebuild(buffer);
    expectTrue(
        emptyPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferFrictionRebuildRejectReason::EmptyBuffer,
        "friction rebuild preflight flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferFrictionRebuild(buffer),
        "canSkipFrictionRebuild on empty buffer");

    buffer.preparePairSlots(1u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    manifold.buildFrictionBasis();
    buffer.writeSlot(0u, manifold);
    buffer.compact();

    const auto orthonormalPreflight =
        fuse::physics::narrowphase::preflightContactBufferFrictionRebuild(buffer);
    expectTrue(
        orthonormalPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferFrictionRebuildRejectReason::AllOrthonormal,
        "friction rebuild preflight skips orthonormal slots");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferFrictionRebuild(buffer),
        "canSkipFrictionRebuild when all orthonormal");

    buffer.tangent1[0] = {99.f, 0.f, 0.f};
    const auto stalePreflight = fuse::physics::narrowphase::preflightContactBufferFrictionRebuild(buffer);
    expectTrue(stalePreflight.needsRebuild(), "friction rebuild preflight needs stale slot");
    buffer.buildFrictionTangentBasesIfNeeded();
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis(
            {0.f, 1.f, 0.f}, buffer.tangentBasisAt(0u)),
        "buildFrictionTangentBasesIfNeeded restores orthonormal frame");
}

void testContactPairBothPlaneDeepenRejectGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 planeA = bodies.addBody({0.f, -1.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 planeB = bodies.addBody({0.f, -2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeA, {0.f, 1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeB, {0.f, 1.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({planeA, planeB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothPlane,
        "deepen reject reason flags both-plane pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({planeA, planeB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedShapePair,
        "base reject reason unchanged for both-plane pair");
    expectTrue(
        fuse::physics::narrowphase::should_skip_contact_pair_deepen_dispatch({planeA, planeB}, bodies, shapes),
        "deepen skip guard rejects both-plane pair");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::BothPlane),
            "BothPlane") == 0,
        "reject reason name resolves BothPlane");
}

void testManifoldNormalizeContactNormalGuards() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !empty.normalizeContactNormalIfNeeded(),
        "normalize returns false for empty manifold");

    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.3f);
    expectTrue(unnormalized.normalizeContactNormalIfNeeded(), "normalize succeeds for scaled normal");
    expectNear(unnormalized.contactNormal.y, 1.f, 1e-4f, "normalize produces unit normal");
    expectTrue(
        !unnormalized.needsNormalNormalization(),
        "normalize clears needs-normalization flag");

    fuse::physics::narrowphase::ContactManifold ready = unnormalized;
    expectTrue(
        fuse::physics::narrowphase::normalize_contact_normal_before_friction_if_needed(ready),
        "normalize before friction succeeds for valid normal");
}

void testContactBufferWritePreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(3u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    const auto validWrite =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, valid);
    expectTrue(validWrite.canWrite(), "write preflight accepts valid manifold");
    expectTrue(
        buffer.writeSlotWithPreflight(0u, valid),
        "writeSlotWithPreflight stores valid manifold");

    fuse::physics::narrowphase::ContactManifold invalid{};
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 1u, invalid,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "write rejects_for_reason flags invalid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 1u, selfPair,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write rejects_for_reason flags self pair");
    expectTrue(
        fuse::physics::narrowphase::shouldSkipContactBufferWrite(buffer, 1u, selfPair),
        "shouldSkipContactBufferWrite rejects self pair");
    expectTrue(
        !buffer.writeSlotWithPreflight(1u, selfPair),
        "writeSlotWithPreflight no-ops on self pair");

    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 99u, valid,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
        "write rejects_for_reason flags out-of-range slot");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
            "SelfPair") == 0,
        "write reject reason name resolves SelfPair");
}

void testContactBufferCompactionPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    const auto emptyPreflight =
        fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(emptyPreflight.emptyBuffer, "compaction preflight marks empty buffer");
    expectTrue(!emptyPreflight.needsCompaction(), "compaction preflight skips empty buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipCompaction true on empty buffer");
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "compaction rejects_for_reason flags empty buffer");

    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(1u, manifold);

    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipCompaction true when slots already contiguous");
    expectTrue(buffer.compact() == 2u, "compact early-outs via preflight on contiguous slots");
}

void testContactBufferClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.setMaxCapacity(2u);
    buffer.preparePairSlots(3u);

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

    buffer.writeSlot(0u, shallow);
    buffer.writeSlot(1u, deep);
    buffer.compact();

    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferClamp(buffer),
        "canSkipClamp true when within capacity");
    expectTrue(
        fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
        "clamp rejects_for_reason flags within-capacity buffer");

    fuse::physics::narrowphase::ContactManifold medium = shallow;
    medium.bodyB = 3u;
    medium.penetrationDepth = 0.5f;
    medium.points[0].penetration = 0.5f;
    buffer.writeSlot(2u, medium);
    buffer.compact();

    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRunClamp true when over capacity");
    expectTrue(
        buffer.compactAndClampWithPreflight() == 2u,
        "compactAndClampWithPreflight truncates to max capacity");
    expectTrue(buffer.droppedCount == 1u, "preflight clamp tracks dropped contacts");
}

void testContactBufferCompactAndClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    const auto emptyPreflight =
        fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(buffer);
    expectTrue(emptyPreflight.emptyBuffer, "compact-and-clamp preflight marks empty buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompactAndClamp(buffer),
        "canSkipCompactAndClamp true on empty buffer");
    expectTrue(
        buffer.compactAndClampWithPreflight() == 0u,
        "compactAndClampWithPreflight early-outs on empty buffer");

    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(1u, manifold);
    buffer.compact();

    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactAndClampRejectsForReason(
            buffer,
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork),
        "compact-and-clamp rejects_for_reason flags no-work buffer");
    expectTrue(
        buffer.compactAndClampWithPreflight() == 2u,
        "compactAndClampWithPreflight no-ops when already compact");
}

void testContactPairPlanePlaneDeepenGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 planeA = bodies.addBody({0.f, -1.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 planeB = bodies.addBody({0.f, -2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeA, {0.f, 1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeB, {0.f, 1.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({planeA, planeB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::PlanePlane,
        "deepen reject reason flags plane-plane pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({planeA, planeB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedShapePair,
        "base reject reason unchanged for plane-plane pair");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::PlanePlane),
            "PlanePlane") == 0,
        "reject reason name resolves PlanePlane");

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{planeA, planeB}};
    expectTrue(
        fuse::physics::narrowphase::count_rejected_contact_pairs(pairs, bodies, shapes) == 1u,
        "count_rejected counts plane-plane pair");
    expectTrue(
        fuse::physics::narrowphase::should_skip_narrowphase_batch(pairs, bodies, shapes),
        "should_skip_narrowphase_batch true when all pairs rejected");
}

void testFinalizeContactManifoldIfNeededGuard() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::finalize_contact_manifold_if_needed(empty),
        "finalize_if_needed no-ops on empty manifold");

    fuse::physics::narrowphase::ContactManifold ready =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::finalize_contact_manifold_if_needed(ready),
        "finalize_if_needed finalizes valid manifold");
    expectTrue(ready.valid, "finalize_if_needed sets validity");
    expectTrue(ready.hasFrictionBasis(), "finalize_if_needed builds friction basis");
}

void testFrictionBasisStaleRejectGuards() {
    fuse::physics::narrowphase::ContactManifold stale{};
    stale.contactNormal = {0.f, 1.f, 0.f};
    stale.addPoint({0.f, 0.f, 0.f}, 0.2f);
    stale.buildFrictionBasis();
    stale.contactNormal = {1.f, 0.f, 0.f};

    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            stale, fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis),
        "friction rejects_for_reason flags stale basis");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::friction_basis_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis),
            "StaleBasis") == 0,
        "friction reject reason name resolves StaleBasis");
    expectTrue(
        fuse::physics::narrowphase::rebuild_friction_basis_with_preflight(stale),
        "rebuild_with_preflight refreshes stale basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(stale),
        "rebuild_with_preflight produces basis matching current normal");

    fuse::physics::narrowphase::compute_friction_tangents_with_preflight(stale);
    expectTrue(stale.hasFrictionBasis(), "compute_with_preflight preserves valid basis");
}

void testContactBufferWritePreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    const auto validPreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 0u, valid);
    expectTrue(validPreflight.canWrite(), "write preflight allows valid manifold");
    expectTrue(
        fuse::physics::narrowphase::writeContactBufferSlotWithPreflight(buffer, 0u, valid),
        "write with preflight stores valid slot");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    const auto selfPreflight =
        fuse::physics::narrowphase::preflightContactBufferWrite(buffer, 1u, selfPair);
    expectTrue(!selfPreflight.canWrite(), "write preflight rejects self pair");
    expectTrue(selfPreflight.selfPair, "write preflight flags self pair");
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 1u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write rejects_for_reason matches self pair");

    fuse::physics::narrowphase::ContactManifold invalid = valid;
    invalid.valid = false;
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer, 1u, invalid, fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "write rejects_for_reason matches invalid manifold");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
            "OutOfRangeSlot") == 0,
        "write reject reason name resolves OutOfRangeSlot");
}

void testContactBufferCompactionPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    const auto emptyPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(
        emptyPreflight.reason == fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer,
        "compaction preflight marks empty buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipCompaction true on empty buffer");
    expectTrue(
        !fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRunCompaction false on empty buffer");

    buffer.preparePairSlots(3u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.2f);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(2u, manifold);

    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRunCompaction true with sparse valid slots");
    expectTrue(buffer.compact() == 2u, "compaction gathers sparse valid slots");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipCompaction true after compact");
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "compaction rejects_for_reason matches all-valid buffer");
}

void testContactBufferClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.setMaxCapacity(2u);
    buffer.preparePairSlots(3u);

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

    buffer.writeSlot(0u, shallow);
    buffer.writeSlot(1u, deep);
    buffer.compact();

    const auto withinPreflight = fuse::physics::narrowphase::preflightContactBufferClamp(buffer);
    expectTrue(
        withinPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity,
        "clamp preflight within capacity after compact");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferClamp(buffer),
        "canSkipClamp true within capacity");

    buffer.preparePairSlots(3u);
    buffer.writeSlot(0u, shallow);
    buffer.writeSlot(1u, deep);
    buffer.writeSlot(2u, medium);
    buffer.compact();
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRunClamp true when active exceeds max capacity");
    expectTrue(
        fuse::physics::narrowphase::preflightContactBufferClamp(buffer).needsClamp(),
        "clamp preflight needs clamp when over capacity");
    expectTrue(buffer.applyMaxCapacityClamp() == 2u, "clamp truncates to max capacity");
    expectNear(buffer.manifoldAt(0u).penetrationDepth, 0.9f, 1e-4f, "clamp keeps deepest contact");
}

void testContactBufferCompactAndClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    const auto emptyPreflight = fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(buffer);
    expectTrue(
        emptyPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer,
        "compactAndClamp preflight marks empty buffer");
    expectTrue(buffer.compactAndClamp() == 0u, "compactAndClamp early-outs on empty buffer");

    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold manifold{};
    manifold.valid = true;
    manifold.bodyA = 0u;
    manifold.bodyB = 1u;
    manifold.contactNormal = {0.f, 1.f, 0.f};
    manifold.addPoint({0.f, 0.f, 0.f}, 0.25f);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(1u, manifold);
    buffer.compact();

    const auto noWorkPreflight =
        fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(buffer);
    expectTrue(
        noWorkPreflight.reason ==
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork,
        "compactAndClamp preflight no-ops when all slots valid and within capacity");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompactAndClamp(buffer),
        "canSkipCompactAndClamp true when no work needed");
    expectTrue(buffer.compactAndClamp() == 2u, "compactAndClamp preserves all-valid buffer");

    buffer.clear();
    buffer.setMaxCapacity(1u);
    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, manifold);
    buffer.writeSlot(1u, manifold);
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferCompactAndClamp(buffer),
        "shouldRunCompactAndClamp true when clamp needed");
    expectTrue(buffer.compactAndClamp() == 1u, "compactAndClamp compacts then clamps");
    expectTrue(buffer.droppedCount == 1u, "compactAndClamp tracks dropped contacts");
}

void testContactPairBothPlanesDeepenGuard() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 planeA = bodies.addBody({0.f, -1.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    const fuse::u32 planeB = bodies.addBody({0.f, -2.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeA, {0.f, 1.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, planeB, {0.f, 1.f, 0.f});

    expectTrue(
        fuse::physics::narrowphase::contact_pair_deepen_reject_reason({planeA, planeB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::BothPlanes,
        "deepen reject reason flags both-plane pair");
    expectTrue(
        fuse::physics::narrowphase::contact_pair_reject_reason({planeA, planeB}, bodies, shapes) ==
            fuse::physics::narrowphase::ContactPairRejectReason::UnsupportedShapePair,
        "base reject reason unchanged for both-plane pair");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_pair_reject_reason_name(
                fuse::physics::narrowphase::ContactPairRejectReason::BothPlanes),
            "BothPlanes") == 0,
        "reject reason name resolves BothPlanes");
}

void testNormalizeContactNormalIfNeededGuard() {
    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::normalize_contact_normal_if_needed(empty),
        "normalize no-ops on empty manifold");

    fuse::physics::narrowphase::ContactManifold scaled{};
    scaled.contactNormal = {0.f, 2.f, 0.f};
    scaled.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::normalize_contact_normal_if_needed(scaled),
        "normalize scales non-unit normal");
    expectNear(scaled.contactNormal.y, 1.f, 1e-4f, "normalize produces unit normal");

    fuse::physics::narrowphase::ContactManifold unit = scaled;
    expectTrue(
        fuse::physics::narrowphase::normalize_contact_normal_if_needed(unit),
        "normalize no-ops on already-unit normal");
    expectNear(unit.contactNormal.y, 1.f, 1e-4f, "normalize preserves unit normal");
}

void testNormalizeAndRebuildFrictionBasisPreflight() {
    fuse::physics::narrowphase::ContactManifold scaled{};
    scaled.contactNormal = {0.f, 2.f, 0.f};
    scaled.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::normalize_and_rebuild_friction_basis_with_preflight(scaled),
        "normalize+rebuild builds orthonormal basis");
    expectNear(scaled.contactNormal.y, 1.f, 1e-4f, "normalize+rebuild unitizes normal");
    expectTrue(
        fuse::physics::narrowphase::isOrthonormalTangentBasis(
            scaled.contactNormal, scaled.frictionBasis),
        "normalize+rebuild stores orthonormal friction basis");

    const auto cachedTangent1 = scaled.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::normalize_and_rebuild_friction_basis_with_preflight(scaled),
        "normalize+rebuild reuses valid cached basis");
    expectNear(
        scaled.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "normalize+rebuild preserves cached tangent1");

    fuse::physics::narrowphase::ContactManifold noNormal{};
    noNormal.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        !fuse::physics::narrowphase::normalize_and_rebuild_friction_basis_with_preflight(noNormal),
        "normalize+rebuild skips invalid normal");
}

void testContactBufferWritePreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.2f);

    const auto validPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_write(buffer, 0u, valid);
    expectTrue(validPreflight.canWrite(), "write preflight allows valid manifold");
    expectTrue(
        buffer.writeSlotIfPreflight(0u, valid),
        "writeSlotIfPreflight writes valid manifold");

    fuse::physics::narrowphase::ContactManifold invalid = valid;
    invalid.valid = false;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_reject_reason(buffer, 1u, invalid) ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold,
        "write reject reason flags invalid manifold");
    expectTrue(
        !buffer.writeSlotIfPreflight(1u, invalid),
        "writeSlotIfPreflight skips invalid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_rejects_for_reason(
            buffer, 0u, selfPair, fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
        "write rejects_for_reason flags self pair");
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_reject_reason(buffer, 99u, valid) ==
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot,
        "write reject reason flags out-of-range slot");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_buffer_write_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
            "InvalidManifold") == 0,
        "write reject reason name resolves InvalidManifold");
}

void testContactBufferCompactionClampPreflights() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    const auto emptyCompaction =
        fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(emptyCompaction.emptyBuffer, "compaction preflight flags empty buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipCompaction on empty buffer");

    buffer.preparePairSlots(3u);
    fuse::physics::narrowphase::ContactManifold first{};
    first.valid = true;
    first.bodyA = 0u;
    first.bodyB = 1u;
    first.contactNormal = {0.f, 1.f, 0.f};
    first.penetrationDepth = 0.4f;
    first.addPoint({0.f, 0.f, 0.f}, 0.4f);
    buffer.writeSlot(0u, first);

    fuse::physics::narrowphase::ContactManifold second = first;
    second.bodyB = 2u;
    second.penetrationDepth = 0.2f;
    second.points[0].penetration = 0.2f;
    buffer.writeSlot(2u, second);

    const auto dirtyCompaction =
        fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(dirtyCompaction.needsCompaction(), "compaction preflight needs work with hole");
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRunCompaction with invalid middle slot");
    expectTrue(buffer.compact() == 2u, "compact gathers valid slots");

    const auto cleanCompaction =
        fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(cleanCompaction.allValid, "compaction preflight all-valid after compact");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipCompaction after compact");

    buffer.setMaxCapacity(1u);
    const auto needsClamp = fuse::physics::narrowphase::preflight_contact_buffer_clamp(buffer);
    expectTrue(needsClamp.needsClamp(), "clamp preflight needs work above max capacity");
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRunClamp when active exceeds max");
    expectTrue(buffer.applyMaxCapacityClamp() == 1u, "clamp truncates to max capacity");
    expectTrue(buffer.droppedCount == 1u, "clamp tracks dropped contacts");

    const auto withinClamp = fuse::physics::narrowphase::preflight_contact_buffer_clamp(buffer);
    expectTrue(withinClamp.withinCapacity, "clamp preflight within capacity after truncate");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferClamp(buffer),
        "canSkipClamp after truncate");

    fuse::physics::narrowphase::ContactBufferSoA cleanBuffer;
    cleanBuffer.preparePairSlots(1u);
    cleanBuffer.writeSlot(0u, first);
    cleanBuffer.compact();
    const auto noWorkPreflight =
        fuse::physics::narrowphase::preflight_contact_buffer_compact_and_clamp(cleanBuffer);
    expectTrue(noWorkPreflight.noWork, "compact-and-clamp preflight no-work on clean buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompactAndClamp(cleanBuffer),
        "canSkipCompactAndClamp on clean buffer");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_buffer_compact_and_clamp_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork),
            "NoWork") == 0,
        "compact-and-clamp reject reason name resolves NoWork");
}

void testContactPairBufferDispatchGuards() {
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
        fuse::physics::narrowphase::should_skip_contact_pair_for_buffer(
            {sleepingA, sleepingB}, bodies, shapes),
        "buffer skip guard rejects both-sleeping pair");
    expectTrue(
        !fuse::physics::narrowphase::should_skip_contact_pair_for_buffer(
            {dynamicA, dynamicB}, bodies, shapes),
        "buffer skip guard allows valid pair");

    const auto sleepingDetect =
        fuse::physics::narrowphase::detect_contacts_pair_if_dispatchable(
            {sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingDetect.valid, "if_dispatchable returns invalid for rejected pair");

    const auto overlap =
        fuse::physics::narrowphase::detect_contacts_pair_if_dispatchable(
            {dynamicA, dynamicB}, bodies, shapes);
    expectTrue(overlap.valid, "if_dispatchable detects valid pair");

    const auto slotPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_slot({dynamicA, dynamicB}, bodies, shapes);
    expectTrue(slotPreflight.can_dispatch(), "slot preflight allows valid pair");
    expectTrue(slotPreflight.canWrite, "slot preflight can write valid pair");

    const auto rejectedPreflight =
        fuse::physics::narrowphase::preflight_narrowphase_slot({sleepingA, sleepingB}, bodies, shapes);
    expectTrue(rejectedPreflight.pairRejected, "slot preflight rejects sleeping pair");
    expectTrue(!rejectedPreflight.canWrite, "slot preflight cannot write rejected pair");
}

void testManifoldFinalizeDeepenPassGuards() {
    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.25f);
    expectTrue(
        fuse::physics::narrowphase::manifold_finalize_rejects_for_reason(
            unnormalized, fuse::physics::narrowphase::ManifoldFinalizeRejectReason::NeedsNormalNormalize),
        "finalize rejects_for_reason flags non-unit normal");
    expectTrue(
        fuse::physics::narrowphase::normalize_contact_normal_if_needed(unnormalized),
        "normalize_if_needed succeeds for valid scaled normal");
    expectNear(unnormalized.contactNormal.y, 1.f, 1e-4f, "normalize_if_needed produces unit normal");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(dirty),
        "prune_if_needed keeps penetrating slot");
    expectTrue(dirty.pointCount == 1u, "prune_if_needed removes separated slot");

    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.3f);
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(clean),
        "prune_if_needed no-ops on clean manifold");
    expectTrue(clean.pointCount == 1u, "prune_if_needed preserves clean slot");

    fuse::physics::narrowphase::ContactManifold manifold =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::finalize_contact_manifold_if_needed(manifold),
        "finalize_if_needed finalizes raw manifold");
    expectTrue(manifold.valid, "finalize_if_needed sets validity");
    expectTrue(manifold.hasFrictionBasis(), "finalize_if_needed builds friction basis");

    const auto cachedTangent1 = manifold.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::finalize_contact_manifold_if_needed(manifold),
        "finalize_if_needed reuses finalized manifold");
    expectNear(
        manifold.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "finalize_if_needed preserves cached basis");
}

void testFrictionBasisDeepenPassGuards() {
    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::normalize_contact_normal_for_friction(unnormalized),
        "normalize_for_friction succeeds for valid scaled normal");
    expectNear(unnormalized.contactNormal.y, 1.f, 1e-4f, "normalize_for_friction produces unit normal");

    fuse::physics::narrowphase::ContactManifold needsBuild{};
    needsBuild.contactNormal = {0.f, 1.f, 0.f};
    needsBuild.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::compute_friction_tangents_with_preflight(needsBuild),
        "compute_with_preflight builds missing basis");
    expectTrue(needsBuild.hasFrictionBasis(), "compute_with_preflight stores orthonormal basis");

    fuse::physics::narrowphase::ContactManifold stale = needsBuild;
    stale.contactNormal = {1.f, 0.f, 0.f};
    expectTrue(
        fuse::physics::narrowphase::friction_basis_rejects_for_reason(
            stale, fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis),
        "friction rejects_for_reason flags stale basis");
    expectTrue(
        fuse::physics::narrowphase::compute_friction_tangents_with_preflight(stale),
        "compute_with_preflight refreshes stale basis");
    expectTrue(
        fuse::physics::narrowphase::friction_basis_matches_normal(stale),
        "compute_with_preflight matches current normal");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::friction_basis_reject_reason_name(
                fuse::physics::narrowphase::FrictionBasisRejectReason::StaleBasis),
            "StaleBasis") == 0,
        "friction reject reason name resolves StaleBasis");
}

void testRunNarrowphaseIfDispatchableGuard() {
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

    fuse::physics::narrowphase::ContactBufferSoA rejectedBuffer;
    rejectedBuffer.preparePairSlots(4u);
    fuse::physics::narrowphase::runNarrowphaseIntoBufferIfDispatchable(
        {{sleepingA, sleepingB}}, bodies, shapes, rejectedBuffer);
    expectTrue(rejectedBuffer.isEmpty(), "if_dispatchable clears buffer when all pairs rejected");
    expectTrue(rejectedBuffer.pairSlotCount == 0u, "if_dispatchable resets pair slots on reject-all");

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{dynamicA, dynamicB}};
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBufferIfDispatchable(pairs, bodies, shapes, buffer);
    expectTrue(buffer.activeCount == 1u, "if_dispatchable produces contact for valid pair");
    expectTrue(buffer.manifoldAt(0u).hasFrictionBasis(), "if_dispatchable finalizes friction basis");
}

void testContactBufferPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(buffer.canSkipSoAIteration(), "empty buffer skips SoA iteration");
    expectEq(buffer.countValidSlots(), 0u, "countValidSlots early-outs when empty");

    const auto emptyCompaction =
        fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(emptyCompaction.emptyBuffer, "compaction preflight marks empty buffer");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_compaction(buffer),
        "can_skip compaction on empty buffer");

    const auto emptyClamp = fuse::physics::narrowphase::preflight_contact_buffer_clamp(buffer);
    expectTrue(emptyClamp.emptyBuffer, "clamp preflight marks empty buffer");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_clamp(buffer),
        "can_skip clamp on empty buffer");

    const auto emptyFriction =
        fuse::physics::narrowphase::preflight_contact_buffer_friction_bases(buffer);
    expectTrue(emptyFriction.emptyBuffer, "friction-bases preflight marks empty buffer");
    expectTrue(
        fuse::physics::narrowphase::can_skip_contact_buffer_friction_bases(buffer),
        "can_skip friction-bases on empty buffer");

    const auto emptyCompactClamp =
        fuse::physics::narrowphase::preflight_contact_buffer_compact_and_clamp(buffer);
    expectTrue(emptyCompactClamp.emptyBuffer, "compact-and-clamp preflight marks empty buffer");
    expectEq(
        fuse::physics::narrowphase::compact_and_clamp_contact_buffer_with_preflight(buffer),
        0u,
        "compact_and_clamp_with_preflight early-outs on empty buffer");

    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
    valid.addPoint({0.f, 0.f, 0.f}, 0.5f);

    const auto validWrite =
        fuse::physics::narrowphase::preflight_contact_buffer_write_slot(buffer, 0u, valid);
    expectTrue(validWrite.canWrite(), "write-slot preflight accepts valid manifold");

    fuse::physics::narrowphase::ContactManifold selfPair = valid;
    selfPair.bodyB = selfPair.bodyA;
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_write_slot_rejects_for_reason(
            buffer,
            1u,
            selfPair,
            fuse::physics::narrowphase::ContactBufferWriteSlotRejectReason::SelfPair),
        "write-slot rejects_for_reason flags self pair");
    expectTrue(
        fuse::physics::narrowphase::should_skip_contact_buffer_write_slot(buffer, 1u, selfPair),
        "should_skip write-slot on self pair");
    buffer.writeSlot(0u, valid);
    buffer.writeSlot(1u, selfPair);
    expectTrue(buffer.countValidSlots() == 1u, "writeSlot ignores self pair via preflight");

    const auto needsCompaction =
        fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(needsCompaction.needsCompaction(), "compaction preflight needs work with hole in slots");
    expectEq(buffer.compact(), 1u, "compact gathers valid slot via preflight gate");

    const auto allValidCompaction =
        fuse::physics::narrowphase::preflight_contact_buffer_compaction(buffer);
    expectTrue(allValidCompaction.allValid, "compaction preflight marks all-valid buffer");
    expectTrue(
        fuse::physics::narrowphase::contact_buffer_compaction_rejects_for_reason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::AllValid),
        "compaction rejects_for_reason flags all-valid buffer");

    buffer.setMaxCapacity(1u);
    fuse::physics::narrowphase::ContactManifold shallow = valid;
    shallow.bodyB = 2u;
    shallow.penetrationDepth = 0.1f;
    shallow.points[0].penetration = 0.1f;
    fuse::physics::narrowphase::ContactManifold deep = valid;
    deep.bodyB = 3u;
    deep.penetrationDepth = 0.9f;
    deep.points[0].penetration = 0.9f;

    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, shallow);
    buffer.writeSlot(1u, deep);
    buffer.compact();
    expectTrue(buffer.canApplyMaxCapacityClamp(), "overflow buffer requests post clamp");
    const auto needsClamp = fuse::physics::narrowphase::preflight_contact_buffer_clamp(buffer);
    expectTrue(needsClamp.needsClamp(), "clamp preflight needs work when over capacity");
    expectEq(buffer.applyMaxCapacityClamp(), 1u, "applyMaxCapacityClamp truncates via preflight gate");
    expectNear(buffer.manifoldAt(0u).penetrationDepth, 0.9f, 1e-4f, "clamp keeps deepest contact");

    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contact_buffer_clamp_reject_reason_name(
                fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
            "WithinCapacity") == 0,
        "clamp reject reason name resolves WithinCapacity");
}

void testNarrowphaseDeepenPassGuards() {
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

    const auto sleepingManifold = fuse::physics::narrowphase::detect_contacts_pair_deepen(
        {sleepingA, sleepingB}, bodies, shapes);
    expectTrue(!sleepingManifold.valid, "detect_contacts_pair_deepen rejects sleeping pair");

    const auto validManifold = fuse::physics::narrowphase::detect_contacts_pair_deepen(
        {dynamicA, dynamicB}, bodies, shapes);
    expectTrue(validManifold.valid, "detect_contacts_pair_deepen accepts valid pair");

    fuse::physics::narrowphase::ContactBufferSoA buffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer({{sleepingA, sleepingB}}, bodies, shapes, buffer);
    expectTrue(buffer.isEmpty(), "runNarrowphaseIntoBuffer early-outs when all pairs deepen-rejected");

    fuse::physics::narrowphase::ContactManifold ready =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::generate_contact_manifold_deepen(ready),
        "generate_contact_manifold_deepen finalizes valid manifold");
    expectTrue(ready.hasFrictionBasis(), "generate_contact_manifold_deepen builds friction basis");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::prune_and_finalize_contact_manifold_with_preflight(dirty),
        "prune_and_finalize_with_preflight keeps penetrating slots");
    expectTrue(dirty.valid, "prune_and_finalize_with_preflight sets validity");
    expectTrue(dirty.pointCount == 1u, "prune_and_finalize_with_preflight removes separated slot");

    fuse::physics::narrowphase::ContactManifold needsTangents{};
    needsTangents.contactNormal = {0.f, 1.f, 0.f};
    needsTangents.addPoint({0.f, 0.f, 0.f}, 0.2f);
    fuse::physics::narrowphase::compute_friction_tangents_with_preflight(needsTangents);
    expectTrue(needsTangents.hasFrictionBasis(), "compute_friction_tangents_with_preflight builds basis");

    needsTangents.buildFrictionBasis();
    const auto cachedTangent1 = needsTangents.frictionBasis.tangent1;
    fuse::physics::narrowphase::compute_friction_tangents_with_preflight(needsTangents);
    expectNear(
        needsTangents.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "compute_friction_tangents_with_preflight preserves cached basis");
}

void testNarrowphaseIntoBufferPreflightGuards() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f);
    const fuse::u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    const std::vector<fuse::physics::broadphase::CandidatePair> emptyPairs{};
    const auto emptyPreflight =
        fuse::physics::narrowphase::preflightNarrowphaseIntoBuffer(emptyPairs, bodies, shapes);
    expectTrue(emptyPreflight.emptyPairs, "into-buffer preflight marks empty pair list");
    expectTrue(
        fuse::physics::narrowphase::should_skip_narrowphase_into_buffer(emptyPairs, bodies, shapes),
        "should_skip_into_buffer on empty pair list");

    fuse::physics::narrowphase::ContactBufferSoA buffer;
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(emptyPairs, bodies, shapes, buffer);
    expectTrue(buffer.isEmpty(), "into-buffer early-out leaves empty contact buffer");

    const std::vector<fuse::physics::broadphase::CandidatePair> validPairs = {{bodyA, bodyB}};
    const auto validPreflight =
        fuse::physics::narrowphase::preflightNarrowphaseIntoBuffer(validPairs, bodies, shapes);
    expectTrue(validPreflight.can_run(), "into-buffer preflight can run with valid pairs");
    expectTrue(validPreflight.dispatchableCount == 1u, "into-buffer preflight counts dispatchable pair");

    const auto slotPreflight = fuse::physics::narrowphase::preflight_narrowphase_pair_slot(
        0u, {bodyA, bodyA}, bodies, shapes);
    expectTrue(!slotPreflight.can_dispatch(), "pair-slot preflight rejects self pair");
    expectTrue(
        slotPreflight.reason == fuse::physics::narrowphase::ContactPairRejectReason::SelfPair,
        "pair-slot preflight reports SelfPair");
    expectTrue(
        fuse::physics::narrowphase::should_skip_narrowphase_pair_slot(0u, {bodyA, bodyA}, bodies, shapes),
        "should_skip_pair_slot on self pair");
}

void testManifoldPruneFinalizeIfNeededGuards() {
    fuse::physics::narrowphase::ContactManifold clean{};
    clean.contactNormal = {0.f, 1.f, 0.f};
    clean.addPoint({0.f, 0.f, 0.f}, 0.4f);
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(clean),
        "prune_if_needed keeps clean manifold");
    expectTrue(clean.pointCount == 1u, "prune_if_needed does not remove penetrating point");

    fuse::physics::narrowphase::ContactManifold dirty{};
    dirty.contactNormal = {0.f, 1.f, 0.f};
    dirty.addPoint({0.f, 0.f, 0.f}, 0.4f);
    dirty.addPoint({1.f, 0.f, 0.f}, -0.2f);
    expectTrue(
        fuse::physics::narrowphase::prune_contact_manifold_if_needed(dirty),
        "prune_if_needed prunes separated slot");
    expectTrue(dirty.pointCount == 1u, "prune_if_needed removes separated slot");

    fuse::physics::narrowphase::ContactManifold ready =
        fuse::physics::narrowphase::collideSphereSphere({0.f, 0.f, 0.f}, 1.f, {1.5f, 0.f, 0.f}, 1.f, 0u, 1u);
    expectTrue(
        fuse::physics::narrowphase::finalize_contact_manifold_if_needed(ready),
        "finalize_if_needed finalizes valid manifold");
    expectTrue(ready.valid, "finalize_if_needed sets validity");
    expectTrue(ready.hasFrictionBasis(), "finalize_if_needed builds friction basis");

    fuse::physics::narrowphase::ContactManifold empty{};
    expectTrue(
        !fuse::physics::narrowphase::finalize_contact_manifold_if_needed(empty),
        "finalize_if_needed no-ops on empty manifold");
}

void testFrictionBasisEnsureWithPreflight() {
    fuse::physics::narrowphase::ContactManifold unnormalized{};
    unnormalized.contactNormal = {0.f, 2.f, 0.f};
    unnormalized.addPoint({0.f, 0.f, 0.f}, 0.2f);
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_with_preflight(unnormalized),
        "ensure_with_preflight builds basis for scaled normal");
    expectTrue(unnormalized.hasFrictionBasis(), "ensure_with_preflight stores orthonormal basis");
    expectNear(unnormalized.contactNormal.length(), 1.f, 1e-4f, "ensure_with_preflight normalizes contact normal");

    const auto cachedTangent1 = unnormalized.frictionBasis.tangent1;
    expectTrue(
        fuse::physics::narrowphase::ensure_friction_basis_with_preflight(unnormalized),
        "ensure_with_preflight reuses valid basis");
    expectNear(
        unnormalized.frictionBasis.tangent1.x,
        cachedTangent1.x,
        1e-4f,
        "ensure_with_preflight preserves cached tangent1");
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
    testContactPairDeepenPassLayerGuards();
    testManifoldPruneFinalizeDeepenPassGuards();
    testFrictionBasisDeepenPassRejectGuards();
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
    testContactPairPreflightDeepenGuards();
    testManifoldPruneShallowPreflightGuards();
    testContactPairKinematicSleepingRejectGuards();
    testContactPairDispatchPreflightGuards();
    testRunNarrowphaseDeepenDispatchGuards();
    testBuildFrictionTangentBasesReuseGuard();
    testNarrowphaseRejectReasonGuards();
    testContactPairDeepenRejectsForReasonGuards();
    testNarrowphasePairListPreflightGuards();
    testNarrowphaseBatchPreflightGuards();
    testManifoldPruneRejectReasonGuards();
    testManifoldFinalizeRejectReasonGuards();
    testFrictionBasisRejectReasonGuards();
    testZeroFrictionAndBatchPreflightGuards();
    testManifoldPruneAndFinalizePreflightHelpers();
    testFrictionBasisPreflightRebuildHelpers();
    testContactPairDeepenPassGuards();
    testFrictionBasisDeepenPassGuards();
    testContactPairDeepen2RejectGuards();
    testCanSkipNarrowphaseDeepen2Guards();
    testManifoldNormalizeAndFinalizeDeepenGuards();
    testFrictionBasisDeepenPreflightGuards();
    testContactPairBatchPreflightGuards();
    testContactPairPlanePlaneAndDeepenDispatchGuards();
    testManifoldFinalizeDeepenGuards();
    testContactBufferPreflightGuards();
    testContactBufferFrictionRebuildPreflightGuards();
    testWarmStartFrictionPreflightGuards();
    testNarrowphasePairBatchPreflightGuards();
    testManifoldPruneFinalizeDeepenGuards();
    testContactPairZeroInvMassDeepenGuards();
    testManifoldGeneratePreflightGuards();
    testManifoldPruneChainPreflightGuards();
    testManifoldFinalizeChainGuards();
    testFrictionBasisEnsurePreflightGuards();
    testContactPairDeepenFollowUpGuards();
    testManifoldPruneDispatchGuards();
    testDetectContactsPairIfNeededGuard();
    testFrictionBasisRejectAndPreflightDispatchGuards();
    testContactPairDeepenPassShouldRunGuards();
    testManifoldPruneDeepenPassRejectReasonGuards();
    testManifoldFinalizeDeepenPassRejectReasonGuards();
    testFrictionBasisDeepenPassShouldRunGuards();
    testContactPairDeepenPassDispatchGuards();
    testManifoldPruneFinalizeDeepenPassGuards();
    testFrictionBasisRebuildDeepenPassGuards();
    testContactPairDeepenPassRejectReasonGuards();
    testFrictionBasisRebuildRejectReasonGuards();
    testContactBufferWriteRejectGuards();
    testContactBufferCompactionClampRejectGuards();
    testContactPairShouldRunDispatchGuards();
    testFrictionBasisRebuildRejectGuards();
    testContactBufferDeepenPassPreflights();
    testNarrowphaseBatchDeepenPassGuards();
    testManifoldPruneFinalizeCombinedPreflights();
    testFrictionBasisNormalizeCombinedPreflights();
    testNarrowphaseDispatchPreflightGuards();
    testContactPairDeepenGuardPassHelpers();
    testManifoldPruneFinalizeGuardPassHelpers();
    testFrictionBasisRebuildGuardPassHelpers();
    testContactBufferGuardPassHelpers();
    testNarrowphasePairSlotGuardPassHelpers();
    testManifoldPruneFinalizeRejectReasonGuards();
    testContactPairGuardPassShouldRunHelpers();
    testManifoldPruneFinalizeGuardPassShouldRun();
    testFrictionBasisRebuildGuardPassShouldRun();
    testContactBufferGuardPassPreflights();
    testNarrowphasePairSlotPreflightGuards();
    testContactPairDeepenPassRejectHelpers();
    testManifoldRejectReasonGuards();
    testContactPairDeepenFollowUpRejectGuards();
    testManifoldPruneFinalizeFollowUpGuards();
    testFrictionBasisFollowUpPreflights();
    testRunNarrowphaseIntoBufferDeepenGuards();
    testContactPairGuardPassRejectGuards();
    testManifoldPruneFinalizeGuardPass();
    testFrictionBasisGuardPass();
    testContactBufferGuardPass();
    testContactPairBatchDeepenPreflightGuards();
    testManifoldProcessPreflightGuards();
    testFrictionBasisRebuildUsingPreflightGuards();
    testContactBufferWritePreflightGuards();
    testRunNarrowphaseDeepenPreflightGuards();
    testContactPairDeepenDispatchGuards();
    testRunNarrowphaseDeepenDispatch();
    testManifoldFinalizeWithPreflightGuards();
    testFrictionBasisRebuildWithPreflightGuards();
    testGjkSupportAndEpaStub();
    testContactPairDeepenFollowUpRejectGuards();
    testManifoldPruneFinalizeFollowUpGuards();
    testContactBufferPreflightGuards();
    testContactBufferCompactionClampPreflights();
    testNarrowphaseRunPreflightGuards();
    testManifoldShallowPrunePreflightGuards();
    testFrictionComputeTangentsPreflightGuard();
    testNarrowphaseIntoBufferPreflightGuards();
    testManifoldGenerateWithPreflightGuards();
    testFrictionBasisDeepenPassGuards();
    testContactBufferWritePreflightGuards();
    testContactBufferCompactionRejectReasonGuards();
    testContactBufferClampRejectReasonGuards();
    testContactBufferCompactAndClampPreflightGuards();
    testContactPairPlanePlaneDeepenRejectGuards();
    testDetectContactsPairWithPreflightGuard();
    testNormalizeContactNormalIfNeededGuard();
    testNarrowphaseBufferFinalizePreflightGuards();
    testFrictionBasisNormalizeBeforeRebuildGuard();
    testContactBufferWriteRejectReasonGuards();
    testDeepenFollowUpPreflightWrappers();
    testContactBufferCompactionPreflightGuards();
    testContactBufferClampPreflightGuards();
    testContactPairBothPlanesDeepenGuard();
    testManifoldNormalizeAndPruneIfNeededGuards();
    testFinalizeContactManifoldIfNeededGuard();
    testFrictionTangentsWithPreflightGuard();
    testContactPairDeepenRejectedGuards();
    testGenerateContactManifoldWithPreflightGuard();
    testComputeFrictionTangentsWithPreflightGuard();
    testContactPairUnionPreflightGuards();
    testFinalizeAndFrictionPreflightIfNeededGuards();
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
    testContactPairGuardedDispatch();
    testManifoldFinalizePreflightGuards();
    testManifoldFinalizeGuardedEntryPoints();
    testManifoldPrunePreflightShallowFlag();
    testFrictionBasisPreflightGuards();
    testContactBufferFrictionTangentBasesIfNeeded();
    testNarrowphasePairBatchPreflightGuards();
    testManifoldPruneFinalizeDeepenGuards();
    testFrictionBasisDeepenPreflightGuards();
    testContactPairDeepenShouldRunGuards();
    testManifoldPruneRejectReasonGuards();
    testManifoldFinalizeRejectReasonGuards();
    testFrictionBasisRebuildRejectReasonGuards();
    testContactBufferRejectReasonGuards();
    testContactPairDeepenDispatchSkipGuards();
    testContactBufferWriteRejectReasonGuards();
    testContactBufferCompactionRejectReasonGuards();
    testRunNarrowphaseDeepenIntoBufferGuards();
    testContactPairDeepenDispatchGuards();
    testManifoldPruneFinalizeIfNeededGuards();
    testFrictionBasisDeepenPassPreflightWrappers();
    testContactBufferPreflightGuards();
    testRunNarrowphaseDeepenPreflightDispatch();
    testContactBufferDeepenPassPreflights();
    testManifoldFrictionDeepenPassPredicates();
    testManifoldPruneFinalizePassGuards();
    testFrictionBasisRebuildPassGuards();
    testContactBufferCompactClampPassGuards();
    testContactBufferFrictionBuildPassGuards();
    testContactBufferWritePreflightGuards();
    testContactBufferCompactionPreflightGuards();
    testContactBufferClampPreflightGuards();
    testContactBufferCompactAndClampPreflightGuards();
    testContactPairDeepenPassGuards();
    testContactManifoldBufferWritePreflightGuards();
    testContactPairDeepenPass6Guards();
    testManifoldPruneFinalizePass6Guards();
    testFrictionBasisPass6Preflights();
    testNarrowphaseIntoBufferPreflightGuards();
    testFrictionBasisEnsureWithPreflight();
    testContactPairDispatchPreflightGuards();
    testManifoldShallowPrunePreflightGuards();
    testContactNormalNormalizePreflightGuards();
    testNarrowphaseDispatchPreflightGuards();
    testNarrowphaseDeepenPassGuards();
    testContactBufferCompactionClampPreflightGuards();
    testContactBufferFrictionRebuildPreflightGuards();
    testContactPairBothPlaneDeepenRejectGuards();
    testManifoldNormalizeContactNormalGuards();
    testContactPairPlanePlaneDeepenGuards();
    testFinalizeContactManifoldIfNeededGuard();
    testFrictionBasisStaleRejectGuards();
    testContactPairBothPlanesDeepenGuard();
    testNormalizeContactNormalIfNeededGuard();
    testNormalizeAndRebuildFrictionBasisPreflight();
    testContactBufferCompactionClampPreflights();
    testContactPairBufferDispatchGuards();
    testManifoldFinalizeDeepenPassGuards();
    testRunNarrowphaseIfDispatchableGuard();
    testContactBufferGuardedWriteAndCompact();
    testFrictionBasisDeepenGuards();

    if (g_failures == 0) {
        std::printf("fuse_physics_narrowphase_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_narrowphase_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
