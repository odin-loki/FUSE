#include <fuse/core/init.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/ccd/ccd.hpp>
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

} // namespace

int main() {
    fuse::core::initialize();
    testSweptSphereSphereFindsImpact();
    testSweptSphereSphereRejectsMiss();
    testCcdPipelineFiltersRbCcdFlag();
    testCcdPipelineSkipsUnflaggedBodies();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_physics_ccd_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_ccd_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
