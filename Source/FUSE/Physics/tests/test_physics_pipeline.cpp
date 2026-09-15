#include <fuse/physics/physics_pipeline.hpp>

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

void testPipelineStepProducesContacts() {
    fuse::physics::PhysicsPipeline pipeline;
    pipeline.init({});

    pipeline.addSphereBody({0.f, 0.5f, 0.f}, 1.f, 1.f);
    pipeline.addStaticPlane({0.f, 1.f, 0.f}, 0.f);

    pipeline.step(1.f / 60.f);

    expectTrue(pipeline.bodyCount() == 2u, "pipeline tracks dynamic and static bodies");
    expectTrue(pipeline.contactCount() >= 1u, "pipeline reports sphere-plane contact after step");
}

} // namespace

int main() {
    testPipelineStepProducesContacts();

    if (g_failures == 0) {
        std::printf("fuse_physics_pipeline_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_pipeline_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
