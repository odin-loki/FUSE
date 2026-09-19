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

void testContactBufferWritePreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.preparePairSlots(2u);

    fuse::physics::narrowphase::ContactManifold valid{};
    valid.valid = true;
    valid.bodyA = 0u;
    valid.bodyB = 1u;
    valid.contactNormal = {0.f, 1.f, 0.f};
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
    expectTrue(
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer,
            0u,
            invalid,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::InvalidManifold),
        "write rejects_for_reason flags invalid manifold");
    expectTrue(
        fuse::physics::narrowphase::shouldSkipContactBufferWrite(buffer, 0u, invalid),
        "shouldSkipContactBufferWrite on invalid manifold");

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
        fuse::physics::narrowphase::contactBufferWriteRejectsForReason(
            buffer,
            4u,
            valid,
            fuse::physics::narrowphase::ContactBufferWriteRejectReason::OutOfRangeSlot),
        "write rejects_for_reason flags out-of-range slot");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferWriteRejectReasonName(
                fuse::physics::narrowphase::ContactBufferWriteRejectReason::SelfPair),
            "SelfPair") == 0,
        "write reject reason name resolves SelfPair");

    buffer.writeSlot(0u, valid);
    expectTrue(buffer.slotIsValid(0u), "writeSlot stores valid slot");
    buffer.invalidateSlot(0u);
    expectTrue(!buffer.slotIsValid(0u), "invalidateSlot clears valid flag");
}

void testContactBufferCompactionPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectTrue(buffer.canSkipSoAIteration(), "empty buffer skips SoA iteration");
    expectTrue(buffer.canSkipCompaction(), "empty buffer skips compaction");
    expectEq(buffer.countValidSlots(), 0u, "countValidSlots early-outs when empty");

    const auto emptyPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(emptyPreflight.emptyBuffer, "compaction preflight marks empty buffer");
    expectTrue(!emptyPreflight.needsCompaction(), "compaction preflight skips empty buffer");
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactionRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferCompactionRejectReason::EmptyBuffer),
        "compaction rejects_for_reason flags empty buffer");

    buffer.preparePairSlots(3u);
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
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferCompaction(buffer),
        "shouldRunContactBufferCompaction on sparse slots");
    expectEq(buffer.compact(), 2u, "compact gathers sparse valid slots");
    expectTrue(buffer.canSkipCompaction(), "compacted all-valid buffer skips compaction");

    const auto allValidPreflight = fuse::physics::narrowphase::preflightContactBufferCompaction(buffer);
    expectTrue(allValidPreflight.allValid, "compaction preflight marks all-valid buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompaction(buffer),
        "canSkipContactBufferCompaction on all-valid buffer");
}

void testContactBufferClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.setMaxCapacity(4u);
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
    buffer.compact();

    const auto withinPreflight = fuse::physics::narrowphase::preflightContactBufferClamp(buffer);
    expectTrue(withinPreflight.withinCapacity, "clamp preflight marks within-capacity buffer");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferClamp(buffer),
        "canSkipContactBufferClamp before overflow");

    buffer.setMaxCapacity(1u);
    expectTrue(buffer.canApplyMaxCapacityClamp(), "canApplyMaxCapacityClamp when over capacity");
    expectTrue(
        fuse::physics::narrowphase::shouldRunContactBufferClamp(buffer),
        "shouldRunContactBufferClamp when over capacity");
    expectTrue(
        !fuse::physics::narrowphase::contactBufferClampRejectsForReason(
            buffer, fuse::physics::narrowphase::ContactBufferClampRejectReason::WithinCapacity),
        "clamp rejects_for_reason does not false-positive overflow buffer");
    expectEq(buffer.applyMaxCapacityClamp(), 1u, "applyMaxCapacityClamp truncates via preflight gate");
    expectTrue(buffer.hasDroppedContacts(), "hasDroppedContacts after clamp");
}

void testContactBufferCompactAndClampPreflightGuards() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    expectEq(
        buffer.compactAndClamp(),
        0u,
        "compactAndClamp early-outs via preflight on empty buffer");
    expectTrue(
        fuse::physics::narrowphase::contactBufferCompactAndClampRejectsForReason(
            buffer,
            fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::EmptyBuffer),
        "compactAndClamp rejects_for_reason flags empty buffer");

    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold contact{};
    contact.valid = true;
    contact.bodyA = 0u;
    contact.bodyB = 1u;
    contact.contactNormal = {0.f, 1.f, 0.f};
    contact.penetrationDepth = 0.3f;
    contact.addPoint({0.f, 0.f, 0.f}, 0.3f);
    buffer.writeSlot(0u, contact);
    buffer.writeSlot(1u, contact);
    buffer.compact();

    const auto noWorkPreflight = fuse::physics::narrowphase::preflightContactBufferCompactAndClamp(buffer);
    expectTrue(noWorkPreflight.noWork, "compactAndClamp preflight marks no-work after compact");
    expectTrue(
        fuse::physics::narrowphase::canSkipContactBufferCompactAndClamp(buffer),
        "canSkipContactBufferCompactAndClamp after compact");

    buffer.clear();
    buffer.setMaxCapacity(1u);
    buffer.preparePairSlots(2u);
    buffer.writeSlot(0u, contact);
    fuse::physics::narrowphase::ContactManifold deeper = contact;
    deeper.bodyB = 2u;
    deeper.penetrationDepth = 0.8f;
    deeper.points[0].penetration = 0.8f;
    buffer.writeSlot(1u, deeper);
    expectEq(
        buffer.compactAndClamp(),
        1u,
        "compactAndClamp gathers then clamps via preflight gate");
    expectNear(buffer.manifoldAt(0u).penetrationDepth, 0.8f, 1e-4f, "compactAndClamp keeps deepest contact");
    expectTrue(
        std::strcmp(
            fuse::physics::narrowphase::contactBufferCompactAndClampRejectReasonName(
                fuse::physics::narrowphase::ContactBufferCompactAndClampRejectReason::NoWork),
            "NoWork") == 0,
        "compactAndClamp reject reason name resolves NoWork");
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
    testContactBufferWritePreflightGuards();
    testContactBufferCompactionPreflightGuards();
    testContactBufferClampPreflightGuards();
    testContactBufferCompactAndClampPreflightGuards();

    if (g_failures == 0) {
        std::printf("fuse_physics_narrowphase_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_narrowphase_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
