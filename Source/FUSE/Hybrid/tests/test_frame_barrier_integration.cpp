// HybridComposer + FrameBarrier integration — compiled into fuse_hybrid_tests.

#include <fuse/core/init.hpp>
#include <fuse/hybrid/barrier_step.hpp>
#include <fuse/hybrid/hybrid_barrier.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <cstdint>
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

void testHybridBarrierOk() {
    expectTrue(fuse::hybrid::hybrid_barrier_ok(0), "generation 0 is a valid hybrid barrier");
    expectTrue(fuse::hybrid::hybrid_barrier_ok(3), "generation 3 is a valid hybrid barrier");
    expectTrue(!fuse::hybrid::hybrid_barrier_ok(-1), "generation -1 is not a valid hybrid barrier");
    expectTrue(!fuse::hybrid::hybrid_barrier_ok(4), "generation 4 is not a valid hybrid barrier");
}

void testNextBarrierGeneration() {
    expectTrue(fuse::hybrid::next_barrier_generation(0ull) == 1ull, "next_barrier_generation(0) == 1");
    expectTrue(fuse::hybrid::next_barrier_generation(41ull) == 42ull, "next_barrier_generation increments by one");
    expectTrue(fuse::hybrid::next_barrier_generation(UINT64_MAX - 1ull) == UINT64_MAX,
               "next_barrier_generation advances to UINT64_MAX");
    expectTrue(fuse::hybrid::next_barrier_generation(UINT64_MAX) == UINT64_MAX,
               "next_barrier_generation saturates at UINT64_MAX");
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
    expectTrue(composer.frameBarrier().tickJobsComplete(), "barrier complete after dimension cull joins");
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
    testHybridBarrierOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse hybrid frame barrier tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testNextBarrierGeneration();
    testHybridTickSignalsFrameBarrier();
    testDisabledDimensionsSkipCullJobs();

    if (g_failures == 0) {
        std::printf("fuse hybrid frame barrier tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse hybrid frame barrier tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
