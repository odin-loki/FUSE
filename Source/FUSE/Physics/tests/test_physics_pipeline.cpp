#include <fuse/physics/forces/ballistic.hpp>
#include <fuse/physics/physics_pipeline.hpp>
#include <fuse/physics/pipeline/phys_stage.hpp>
#include <fuse/physics/solver/substep_count.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testPhysStageOk() {
    expectTrue(fuse::physics::phys_stage_ok(0), "phys_stage_ok(0) is true");
    expectTrue(fuse::physics::phys_stage_ok(7), "phys_stage_ok(7) is true");
    expectTrue(!fuse::physics::phys_stage_ok(-1), "phys_stage_ok(-1) is false");
    expectTrue(!fuse::physics::phys_stage_ok(8), "phys_stage_ok(8) is false");
}

void testSubstepCountOk() {
    expectTrue(fuse::physics::substep_count_ok(1), "substep_count_ok(1) is true");
    expectTrue(fuse::physics::substep_count_ok(16), "substep_count_ok(16) is true");
    expectTrue(!fuse::physics::substep_count_ok(0), "substep_count_ok(0) is false");
    expectTrue(!fuse::physics::substep_count_ok(17), "substep_count_ok(17) is false");
}

void testPipelineStepProducesContacts() {
    fuse::physics::PhysicsPipeline pipeline;
    pipeline.init({});

    pipeline.addSphereBody({0.f, 0.5f, 0.f}, 1.f, 1.f);
    pipeline.addStaticPlane({0.f, 1.f, 0.f}, 0.f);

    pipeline.step(1.f / 60.f);

    expectTrue(pipeline.bodyCount() == 2u, "pipeline tracks dynamic and static bodies");
    expectTrue(pipeline.contactCount() >= 1u, "pipeline reports sphere-plane contact after step");
}

void testBallisticPosition() {
    const fuse::physics::vec3 origin{};
    const fuse::physics::vec3 velocity{0.f, 10.f, 0.f};
    const fuse::physics::vec3 gravity{0.f, -10.f, 0.f};

    const fuse::physics::vec3 atTwo =
        fuse::physics::ballistic_position(origin, velocity, gravity, 2.f);
    expectTrue(atTwo.y == 0.f, "ballistic position y is 0 at t=2");

    const fuse::physics::vec3 atZero =
        fuse::physics::ballistic_position(origin, velocity, gravity, 0.f);
    expectTrue(atZero.x == origin.x && atZero.y == origin.y && atZero.z == origin.z,
               "ballistic position at t=0 returns the origin");
}

} // namespace

int main() {
    testPhysStageOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_physics_pipeline_tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testSubstepCountOk();
    testPipelineStepProducesContacts();
    testBallisticPosition();

    if (g_failures == 0) {
        std::printf("fuse_physics_pipeline_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_pipeline_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
