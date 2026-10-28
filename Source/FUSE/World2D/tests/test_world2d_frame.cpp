#include <fuse/core/init.hpp>
#include <fuse/world2d/frame_rate.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/sprite_frame.hpp>
#include <fuse/world2d/tile_coord.hpp>
#include <fuse/world2d/world_2d.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(float value, float expected, float epsilon, const char* message) {
    if (std::fabs(value - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f expected %f)\n", message, value, expected);
        ++g_failures;
    }
}

void testWorld2DTickBuildsSnapshot() {
    fuse::world2d::World2D world;
    fuse::SceneObject2D sprite("sprite");
    sprite.setPosition(10.f, 20.f);
    world.addSprite(&sprite);

    fuse::frame::FrameCtx ctx;
    ctx.time = 2.f;
    ctx.frameIndex = 1;

    world.tick(ctx);

    expectTrue(world.readSnapshot().sprites().size() == 1u, "snapshot contains sprite");
    expectTrue(world.readSnapshot().visibleCount() == 1u, "parallel cull kept sprite visible");
    expectNear(world.readSnapshot().sprites()[0].x, 10.f, 1e-4f, "snapshot uses world x from hierarchy");
    expectNear(world.readSnapshot().sprites()[0].y, 20.f, 1e-4f, "snapshot uses world y from hierarchy");
    expectNear(world.readSnapshot().sprites()[0].rotation, 3.f, 1e-4f, "snapshot applies per-frame rotation");
    expectTrue(world.readTransformSoA().object.size() == 1u, "SoA parallel to snapshot");
    expectTrue(world.readTransformSoA().object[0] == world.readSnapshot().sprites()[0].object,
               "SoA handle matches draw cmd");
}

void testAnimatedSpriteAtlasFrameAdvances() {
    fuse::world2d::World2D world;
    fuse::SceneObject2D hero("Hero");
    hero.setSpriteAnimation(8u, 12.f);
    world.addSprite(&hero);

    fuse::frame::FrameCtx early;
    early.time = 0.f;
    world.tickGameThread(early);
    expectTrue(world.readSnapshot().sprites()[0].atlasFrameCount == 8u, "animated sprite exports frame count");
    expectTrue(world.readSnapshot().sprites()[0].atlasFrame == 0u, "frame 0 at t=0");

    fuse::frame::FrameCtx later;
    later.time = 0.5f;
    world.tickGameThread(later);
    expectTrue(world.readSnapshot().sprites()[0].atlasFrame == 6u, "frame advances with animationFPS");
}

void testWorld2DHierarchySnapshotSoA() {
    fuse::world2d::World2D world;
    fuse::SceneObject2D group("group");
    fuse::SceneObject2D child("child");
    group.setPosition(5.f, 0.f);
    child.setPosition(2.f, 3.f);
    group.addChild(&child);
    world.addSprite(&group);

    fuse::frame::FrameCtx ctx;
    ctx.time = 0.f;
    world.tickGameThread(ctx);

    expectTrue(world.readSnapshot().sprites().size() == 2u, "hierarchy snapshot includes group and child");
    expectTrue(world.readTransformSoA().worldX.size() == 2u, "SoA x column matches hierarchy size");
    expectNear(world.readTransformSoA().worldX[1], 7.f, 1e-4f, "child world x accumulates parent offset");
    expectNear(world.readSnapshot().sprites()[1].x, 7.f, 1e-4f, "draw cmd mirrors SoA world x");
}

void testTileCoord() {
    expectTrue(fuse::world2d::tile_coord(1, 2, 4) == 9, "(1, 2, 4) is 9");
    expectTrue(fuse::world2d::tile_coord(0, 0, 1) == 0, "(0, 0, 1) is 0");
    expectTrue(fuse::world2d::tile_coord(1, 0, 0) == -1, "(1, 0, 0) is -1");
    expectTrue(fuse::world2d::tile_coord(4, 0, 4) == -1, "(4, 0, 4) is -1");
}

void testWorld2dFpsOk() {
    expectTrue(fuse::world2d::world2d_fps_ok(30), "30 fps is supported");
    expectTrue(fuse::world2d::world2d_fps_ok(60), "60 fps is supported");
    expectTrue(!fuse::world2d::world2d_fps_ok(24), "24 fps is not supported");
    expectTrue(!fuse::world2d::world2d_fps_ok(0), "0 fps is not supported");
}

void testSpriteFrameIndex() {
    expectTrue(fuse::world2d::sprite_frame_index(0.f, 8.f, 4) == 0, "t=0 fps=8 count=4 is frame 0");
    expectTrue(fuse::world2d::sprite_frame_index(0.25f, 8.f, 4) == 2, "t=0.25 fps=8 count=4 is frame 2");
    expectTrue(fuse::world2d::sprite_frame_index(0.5f, 8.f, 4) == 0, "t=0.5 fps=8 count=4 wraps to frame 0");

    expectTrue(fuse::world2d::sprite_frame_index(1.f, 8.f, 0) == 0, "frameCount 0 returns 0");
    expectTrue(fuse::world2d::sprite_frame_index(1.f, 8.f, -3) == 0, "negative frameCount returns 0");
    expectTrue(fuse::world2d::sprite_frame_index(0.25f, 0.f, 4) == 0, "fps 0 returns 0");
    expectTrue(fuse::world2d::sprite_frame_index(0.25f, -8.f, 4) == 0, "negative fps returns 0");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    expectTrue(fuse::world2d::sprite_frame_index(nan, 8.f, 4) == 0, "non-finite time returns 0");
    expectTrue(fuse::world2d::sprite_frame_index(0.25f, nan, 4) == 0, "non-finite fps returns 0");
    expectTrue(fuse::world2d::sprite_frame_index(inf, 8.f, 4) == 0, "infinite time returns 0");
    expectTrue(fuse::world2d::sprite_frame_index(0.25f, inf, 4) == 0, "infinite fps returns 0");

    expectTrue(fuse::world2d::sprite_frame_index(-0.125f, 8.f, 4) == 3, "negative time wraps to a non-negative index");
    expectTrue(fuse::world2d::sprite_frame_index(-0.5f, 8.f, 4) == 0, "negative time on a period boundary wraps to 0");
}

} // namespace

int main() {
    testTileCoord();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_world2d_tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    testWorld2dFpsOk();
    testSpriteFrameIndex();
    fuse::core::initialize();
    testWorld2DTickBuildsSnapshot();
    testAnimatedSpriteAtlasFrameAdvances();
    testWorld2DHierarchySnapshotSoA();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_world2d_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_world2d_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
