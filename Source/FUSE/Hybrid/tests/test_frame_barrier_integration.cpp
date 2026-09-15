// HybridComposer + FrameBarrier integration — compiled into fuse_hybrid_tests.

#include <fuse/core/init.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/world_3d.hpp>

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

void expectEq(fuse::u32 actual, fuse::u32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %u, got %u)\n", message, expected, actual);
        ++g_failures;
    }
}

void testHybridTickSignalsFrameBarrier() {
    fuse::hybrid::HybridComposer composer;
    fuse::world2d::World2D world2D;
    fuse::world3d::World3D world3D;
    fuse::SceneObject2D sprite("sprite");

    sprite.setPosition(5.f, 10.f);
    world2D.addSprite(&sprite);
    composer.attachWorld2D(&world2D);
    composer.attachWorld3D(&world3D);

    fuse::frame::FrameCtx ctx;
    ctx.dt = 1.f / 60.f;
    ctx.time = 0.5f;
    ctx.frameIndex = 9u;

    composer.tick(ctx);

    expectEq(composer.frameBarrier().frameIndex(), 9u, "barrier records tick frame index");
    expectTrue(composer.frameBarrier().tickJobsComplete(), "barrier complete after JobScheduler cull join");
    expectEq(composer.frameCount(), 1u, "frame counter advanced");
    expectEq(world2D.readSnapshot().sprites().size(), 1u, "2D snapshot built before cull");
    expectTrue(world2D.readSnapshot().visibleCount() == 1u, "parallel cull ran via composer tick");
}

void testDisabledDimensionsSkipCullJobs() {
    fuse::hybrid::HybridComposer composer;
    fuse::world2d::World2D world2D;
    fuse::world3d::World3D world3D;

    composer.attachWorld2D(&world2D);
    composer.attachWorld3D(&world3D);

    fuse::hybrid::DimensionFlags flags;
    flags.enable3D = false;
    flags.enable2D = false;
    composer.setProjectFlags(flags);

    fuse::frame::FrameCtx ctx;
    ctx.frameIndex = 3u;
    composer.tick(ctx);

    expectTrue(composer.frameBarrier().tickJobsComplete(), "barrier still signals when no cull jobs");
    expectEq(world2D.readSnapshot().sprites().size(), 0u, "disabled 2D skipped snapshot build");
}

} // namespace

int runFrameBarrierIntegrationTests() {
    testHybridTickSignalsFrameBarrier();
    testDisabledDimensionsSkipCullJobs();

    if (g_failures == 0) {
        std::printf("fuse hybrid frame barrier tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse hybrid frame barrier tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
