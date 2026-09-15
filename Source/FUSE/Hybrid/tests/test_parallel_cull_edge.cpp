// Parallel cull batch edge cases — empty snapshots, partial batches, frustum bounds.
// Compiled into fuse_hybrid_tests.

#include <fuse/core/init.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
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

fuse::frame::FrameCtx makeCtx(fuse::u32 frameIndex = 0u) {
    fuse::frame::FrameCtx ctx;
    ctx.dt = 1.f / 60.f;
    ctx.time = 0.f;
    ctx.frameIndex = frameIndex;
    return ctx;
}

void testWorld2DEmptySnapshotCull() {
    fuse::world2d::World2D world;
    fuse::frame::FrameCtx ctx = makeCtx();

    world.tickGameThread(ctx);
    world.runParallelCull();

    expectEq(world.readSnapshot().sprites().size(), 0u, "empty world produces empty snapshot");
    expectEq(world.readSnapshot().visibleCount(), 0u, "empty snapshot cull yields zero visible");
}

void testWorld2DSingleSpritePartialBatch() {
    fuse::world2d::World2D world;
    fuse::SceneObject2D sprite("solo");
    sprite.setPosition(12.f, 34.f);
    world.addSprite(&sprite);

    fuse::frame::FrameCtx ctx = makeCtx();
    world.tickGameThread(ctx);
    world.runParallelCull();

    expectEq(world.readSnapshot().sprites().size(), 1u, "single sprite snapshot size");
    expectEq(world.readSnapshot().visibleCount(), 1u, "single in-view sprite survives cull");
}

void testWorld2DAllSpritesOutOfView() {
    fuse::world2d::World2D world;
    fuse::SceneObject2D offscreenA("offA");
    fuse::SceneObject2D offscreenB("offB");
    offscreenA.setPosition(20000.f, 0.f);
    offscreenB.setPosition(0.f, -20000.f);
    world.addSprite(&offscreenA);
    world.addSprite(&offscreenB);

    fuse::frame::FrameCtx ctx = makeCtx();
    world.tickGameThread(ctx);
    world.runParallelCull();

    expectEq(world.readSnapshot().sprites().size(), 2u, "offscreen sprites still in snapshot");
    expectEq(world.readSnapshot().visibleCount(), 0u, "offscreen sprites culled to zero visible");
}

void testWorld2DMixedVisibilityBatchBoundary() {
    fuse::world2d::World2D world;
    fuse::SceneObject2D sprites[9] = {
        fuse::SceneObject2D("sprite0"), fuse::SceneObject2D("sprite1"), fuse::SceneObject2D("sprite2"),
        fuse::SceneObject2D("sprite3"), fuse::SceneObject2D("sprite4"), fuse::SceneObject2D("sprite5"),
        fuse::SceneObject2D("sprite6"), fuse::SceneObject2D("sprite7"), fuse::SceneObject2D("sprite8"),
    };

    for (fuse::u32 i = 0; i < 9u; ++i) {
        const float x = (i % 2u == 0u) ? static_cast<float>(i) : 15000.f;
        sprites[i].setPosition(x, static_cast<float>(i));
        world.addSprite(&sprites[i]);
    }

    fuse::frame::FrameCtx ctx = makeCtx();
    world.tickGameThread(ctx);
    world.runParallelCull();

    expectEq(world.readSnapshot().sprites().size(), 9u, "mixed batch snapshot size");
    expectEq(world.readSnapshot().visibleCount(), 5u, "mixed batch keeps even-index in-view sprites");
}

void testWorld3DFrustumEdgeCull() {
    fuse::world3d::World3D world;
    fuse::SceneObject3D inside("inside");
    fuse::SceneObject3D outside("outside");
    inside.setPosition(0.f, 0.f);
    inside.setZ(0.f);
    outside.setPosition(0.f, 0.f);
    outside.setZ(-600.f);
    world.addObject(&inside);
    world.addObject(&outside);

    fuse::frame::FrameCtx ctx = makeCtx();
    world.tickGameThread(ctx);
    world.runParallelCull();

    expectEq(world.readSnapshot().objects().size(), 2u, "frustum edge snapshot size");
    expectEq(world.readSnapshot().visibleCount(), 1u, "only in-frustum object survives cull");
}

void testHybridComposerParallelDimensionCull() {
    fuse::hybrid::HybridComposer composer;
    fuse::world2d::World2D world2D;
    fuse::world3d::World3D world3D;

    fuse::SceneObject2D sprite("2d");
    fuse::SceneObject3D object("3d");
    sprite.setPosition(1.f, 2.f);
    object.setPosition(0.f, 0.f);
    object.setZ(10.f);
    world2D.addSprite(&sprite);
    world3D.addObject(&object);

    composer.attachWorld2D(&world2D);
    composer.attachWorld3D(&world3D);

    fuse::frame::FrameCtx ctx = makeCtx(12u);
    composer.tick(ctx);

    expectTrue(composer.frameBarrier().tickJobsComplete(), "barrier complete after dual-dimension culls");
    expectEq(composer.frameBarrier().frameIndex(), 12u, "barrier records composer tick frame");
    expectEq(world2D.readSnapshot().visibleCount(), 1u, "2D cull finished before barrier signal");
    expectEq(world3D.readSnapshot().visibleCount(), 1u, "3D cull finished before barrier signal");
}

void testHybridComposerSequentialTicksResetBarrier() {
    fuse::hybrid::HybridComposer composer;
    fuse::world2d::World2D world2D;
    composer.attachWorld2D(&world2D);

    for (fuse::u32 frame = 0; frame < 3u; ++frame) {
        fuse::frame::FrameCtx ctx = makeCtx(frame);
        composer.tick(ctx);

        expectEq(composer.frameBarrier().frameIndex(), frame, "barrier tracks each tick frame index");
        expectTrue(composer.frameBarrier().tickJobsComplete(), "barrier complete every tick");
    }

    expectEq(composer.frameCount(), 3u, "composer frame counter advanced across ticks");
}

} // namespace

int runParallelCullEdgeTests() {
    testWorld2DEmptySnapshotCull();
    testWorld2DSingleSpritePartialBatch();
    testWorld2DAllSpritesOutOfView();
    testWorld2DMixedVisibilityBatchBoundary();
    testWorld3DFrustumEdgeCull();
    testHybridComposerParallelDimensionCull();
    testHybridComposerSequentialTicksResetBarrier();

    if (g_failures == 0) {
        std::printf("fuse hybrid parallel cull edge tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse hybrid parallel cull edge tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
