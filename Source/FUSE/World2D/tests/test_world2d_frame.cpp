#include <fuse/core/init.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/world_2d.hpp>

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
}

} // namespace

int main() {
    fuse::core::initialize();
    testWorld2DTickBuildsSnapshot();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_world2d_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_world2d_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
