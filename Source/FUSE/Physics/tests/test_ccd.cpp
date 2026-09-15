#include <fuse/core/init.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/ccd/ccd.hpp>
#include <fuse/physics/ccd/toi_buffer.hpp>
#include <fuse/physics/physics_data.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

using namespace fuse::physics;
using fuse::u32;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(f32 actual, f32 expected, f32 epsilon, const char* message) {
    if (std::fabs(actual - expected) > epsilon) {
        std::fprintf(stderr,
                     "FAIL: %s (expected %.5f, got %.5f)\n",
                     message,
                     expected,
                     actual);
        ++g_failures;
    }
}

void testSweptSphereSphereFindsImpact() {
    const TOIResult result = sweptSphereSphere(
        {0.f, 0.f, 0.f}, {10.f, 0.f, 0.f}, 0.5f, {5.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, 0.5f);
    expectTrue(result.valid, "fast sphere detects impact against static sphere");
    expectNear(result.toi, 0.4f, 0.02f, "TOI matches closed-form root");
}

void testSweptSphereSphereRejectsMiss() {
    const TOIResult result = sweptSphereSphere(
        {0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, 0.5f, {0.f, 5.f, 0.f}, {0.f, 0.f, 0.f}, 0.5f);
    expectTrue(!result.valid, "parallel miss returns invalid TOI");
}

void testSweptSpherePlaneFindsWallImpact() {
    const TOIResult result = sweptSpherePlane(
        {0.f, 0.f, 0.f}, {0.f, 0.f, 50.f}, 0.5f, {0.f, 0.f, 1.f}, 5.f);
    expectTrue(result.valid, "fast sphere detects plane wall impact");
    expectNear(result.toi, 0.11f, 0.02f, "plane TOI prevents tunneling through wall at z=5");
}

void testSweptSphereSlabFindsThinWallImpact() {
    const TOIResult result = sweptSphereSlabZ(
        {0.f, 0.f, 0.f}, {0.f, 0.f, 100.f}, 0.5f, 5.f, 0.05f);
    expectTrue(result.valid, "fast sphere detects thin slab wall impact");
    expectTrue(result.toi < 0.15f, "slab TOI occurs before discrete end-of-step tunnel");
}

void testToiBufferPushSortOrder() {
    ToiBufferSoA buffer;
    buffer.reserve(4u);

    TOIResult late{};
    late.valid = true;
    late.toi = 0.75f;
    late.bodyA = 1u;
    late.bodyB = 2u;

    TOIResult early = late;
    early.toi = 0.1f;
    early.bodyA = 3u;

    TOIResult mid = late;
    mid.toi = 0.4f;
    mid.bodyA = 5u;

    expectTrue(buffer.push(late), "push accepts valid TOI");
    expectTrue(buffer.push(early), "push accepts second TOI");
    expectTrue(buffer.push(mid), "push accepts third TOI");
    expectTrue(buffer.activeCount == 3u, "push grows active count");

    buffer.sortByToi();
    expectNear(buffer.resultAt(0u).toi, 0.1f, 1e-5f, "sort places earliest TOI first");
    expectNear(buffer.resultAt(1u).toi, 0.4f, 1e-5f, "sort orders middle TOI");
    expectNear(buffer.resultAt(2u).toi, 0.75f, 1e-5f, "sort places latest TOI last");
    expectTrue(buffer.resultAt(0u).bodyA == 3u, "sort preserves body metadata");
}

void testToiBufferEmpty() {
    ToiBufferSoA buffer;
    expectTrue(buffer.activeCount == 0u, "default buffer is empty");

    TOIResult invalid{};
    expectTrue(!buffer.push(invalid), "push rejects invalid TOI on empty buffer");
    expectTrue(buffer.resultAt(0u).valid == false, "resultAt on empty buffer is invalid");

    buffer.sortByToi();
    expectTrue(buffer.activeCount == 0u, "sort on empty buffer is no-op");
    expectTrue(buffer.toVector().empty(), "toVector on empty buffer returns empty");
}

void testToiBufferCapacityClamp() {
    ToiBufferSoA buffer;
    buffer.setMaxCapacity(2u);

    TOIResult first{};
    first.valid = true;
    first.toi = 0.2f;

    TOIResult second = first;
    second.toi = 0.5f;

    TOIResult third = first;
    third.toi = 0.8f;

    expectTrue(buffer.push(first), "first push fits capacity");
    expectTrue(buffer.push(second), "second push fits capacity");
    expectTrue(!buffer.push(third), "third push is clamped at max capacity");
    expectTrue(buffer.activeCount == 2u, "active count stops at max capacity");
    expectTrue(buffer.droppedCount == 1u, "dropped count tracks clamped pushes");
}

void testSweptSphereAabbFindsImpact() {
    const aabb box{{-1.f, -1.f, 4.f}, {1.f, 1.f, 6.f}};
    const TOIResult result =
        sweptSphereAabb({0.f, 0.f, 0.f}, {0.f, 0.f, 10.f}, 0.5f, box);
    expectTrue(result.valid, "sphere sweep detects AABB entry");
    expectTrue(result.toi < 0.5f, "AABB TOI occurs before end of segment");
}

void testSweptSphereAabbRejectsMiss() {
    const aabb box{{5.f, 5.f, 5.f}, {6.f, 6.f, 6.f}};
    const TOIResult result =
        sweptSphereAabb({0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, 0.5f, box);
    expectTrue(!result.valid, "parallel miss against distant AABB is invalid");
}

void testToiBufferClearReuse() {
    ToiBufferSoA buffer;
    buffer.reserve(8u);
    buffer.preparePairSlots(2u);
    TOIResult first{};
    first.valid = true;
    first.toi = 0.25f;
    first.bodyA = 0u;
    first.bodyB = 1u;
    buffer.writeSlot(0u, first);
    expectTrue(buffer.compact() == 1u, "compact keeps valid TOI slot");

    buffer.clear();
    expectTrue(buffer.activeCount == 0u, "clear resets active count");
    expectTrue(buffer.pairSlotCount == 0u, "clear resets pair slots");

    buffer.preparePairSlots(4u);
    TOIResult second = first;
    second.bodyB = 2u;
    buffer.writeSlot(1u, second);
    buffer.writeSlot(3u, first);
    expectTrue(buffer.compact() == 2u, "reuse after clear compacts new TOIs");
}

void testRunCcdIntoBufferJobSafe() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 fastSphere = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_CCD);
    const u32 wallBody = bodies.addBody({0.f, 0.f, 5.f}, 0.f, RB_STATIC);
    bodies.linearVelocities[fastSphere] = {0.f, 0.f, 50.f};
    shapes.addShape(CollisionShapeType::Sphere, fastSphere, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Box, wallBody, {10.f, 10.f, 0.05f});

    const std::vector<broadphase::CandidatePair> pairs = {{fastSphere, wallBody}};

    ToiBufferSoA buffer;
    buffer.reserve(1u);
    runCcdIntoBuffer(pairs, bodies, shapes, 1.f, buffer);

    expectTrue(buffer.activeCount == 1u, "job-safe CCD resolves sphere-thin-wall pair");
    const TOIResult result = buffer.resultAt(0u);
    expectTrue(result.valid, "buffer TOI valid");
    expectTrue(result.bodyA == fastSphere && result.bodyB == wallBody, "buffer preserves body indices");
    expectTrue(result.toi < 0.15f, "buffer TOI catches fast mover before tunneling");
}

void testCcdPipelineFiltersRbCcdFlag() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_CCD);
    const u32 bodyB = bodies.addBody({5.f, 0.f, 0.f}, 1.f, 0);
    bodies.linearVelocities[bodyA] = {10.f, 0.f, 0.f};
    shapes.addShape(CollisionShapeType::Sphere, bodyA, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, bodyB, {0.5f, 0.f, 0.f});

    std::vector<broadphase::CandidatePair> pairs = {{bodyA, bodyB}};
    std::vector<TOIResult> results;

    CcdPipeline pipeline;
    const u32 count = pipeline.sweepPairs(bodies, shapes, pairs, 1.f, results);
    expectTrue(count == 1u, "CCD pipeline emits one TOI for flagged fast body");
    expectTrue(!results.empty() && results[0].valid, "pipeline TOI is valid");
    expectNear(results[0].toi, 0.4f, 0.03f, "pipeline TOI matches analytic sweep");
}

void testCcdPipelineSkipsUnflaggedBodies() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 bodyB = bodies.addBody({5.f, 0.f, 0.f}, 1.f, 0);
    bodies.linearVelocities[bodyA] = {10.f, 0.f, 0.f};
    shapes.addShape(CollisionShapeType::Sphere, bodyA, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, bodyB, {0.5f, 0.f, 0.f});

    std::vector<broadphase::CandidatePair> pairs = {{bodyA, bodyB}};
    std::vector<TOIResult> results;

    CcdPipeline pipeline;
    const u32 count = pipeline.sweepPairs(bodies, shapes, pairs, 1.f, results);
    expectTrue(count == 0u, "CCD pipeline skips pairs without RB_CCD flag");
}

void testCcdPipelineSpherePlanePair() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 fastSphere = bodies.addBody({0.f, 5.f, 0.f}, 1.f, RB_CCD);
    const u32 ground = bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    bodies.linearVelocities[fastSphere] = {0.f, -10.f, 0.f};
    shapes.addShape(CollisionShapeType::Sphere, fastSphere, {1.f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Plane, ground, {0.f, 1.f, 0.f}, 0.f);

    std::vector<broadphase::CandidatePair> pairs = {{fastSphere, ground}};
    std::vector<TOIResult> results;

    CcdPipeline pipeline;
    const u32 count = pipeline.sweepPairs(bodies, shapes, pairs, 1.f, results);
    expectTrue(count == 1u, "CCD pipeline handles sphere-plane sweep");
    expectTrue(!results.empty() && results[0].valid, "sphere-plane TOI is valid");
    expectNear(results[0].toi, 0.4f, 0.03f, "sphere-plane TOI matches analytic sweep");
}

} // namespace

int main() {
    fuse::core::initialize();
    testSweptSphereSphereFindsImpact();
    testSweptSphereSphereRejectsMiss();
    testSweptSpherePlaneFindsWallImpact();
    testSweptSphereSlabFindsThinWallImpact();
    testToiBufferPushSortOrder();
    testToiBufferEmpty();
    testToiBufferCapacityClamp();
    testSweptSphereAabbFindsImpact();
    testSweptSphereAabbRejectsMiss();
    testToiBufferClearReuse();
    testRunCcdIntoBufferJobSafe();
    testCcdPipelineFiltersRbCcdFlag();
    testCcdPipelineSkipsUnflaggedBodies();
    testCcdPipelineSpherePlanePair();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_physics_ccd_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_ccd_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
