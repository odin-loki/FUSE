#include <fuse/core/init.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/world_2d.hpp>

#include <cmath>
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

} // namespace

int main() {
    fuse::core::initialize();
    testWorld2DTickBuildsSnapshot();
    testWorld2DHierarchySnapshotSoA();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_world2d_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_world2d_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
