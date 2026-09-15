#include <fuse/core/init.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <cstdio>
#include <cstdlib>

int runFramePipelineMtTests();
int runFrameBarrierIntegrationTests();

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testHybridFrameProducesPixels() {
    fuse::hybrid::HybridComposer composer;
    fuse::world2d::World2D world2D;
    fuse::world3d::World3D world3D;
    fuse::SceneObject2D hudSprite("hud");

    hudSprite.setPosition(0.f, 0.f);
    world2D.addSprite(&hudSprite);
    world3D.setClearColor(0.1f, 0.15f, 0.25f);

    composer.attachWorld2D(&world2D);
    composer.attachWorld3D(&world3D);

    fuse::frame::FrameCtx ctx;
    ctx.dt = 1.f / 60.f;
    ctx.time = 1.f;
    ctx.frameIndex = 1;

    composer.tick(ctx);
    composer.render(ctx);

    expectTrue(composer.renderer().width() == 320u, "renderer width set");
    expectTrue(composer.renderer().pixelCount() == 320u * 240u * 4u, "RGBA buffer allocated");

    const fuse::u8 centerR = composer.renderer().sample(160, 120);
    expectTrue(centerR > 0, "3D clear visible at frame center");

    const fuse::u8 spriteChannel = composer.renderer().sample(172, 132);
    expectTrue(spriteChannel > 0, "2D sprite drawn over 3D clear");
}

void testDimensionDisableFlags() {
    fuse::hybrid::HybridComposer composer;
    fuse::world2d::World2D world2D;
    fuse::world3d::World3D world3D;

    composer.attachWorld2D(&world2D);
    composer.attachWorld3D(&world3D);

    fuse::hybrid::DimensionFlags flags;
    flags.enable3D = false;
    flags.enable2D = true;
    composer.setProjectFlags(flags);

    expectTrue(!world3D.isEnabled(), "3D disabled by project flag");
    expectTrue(world2D.isEnabled(), "2D still enabled");

    fuse::frame::FrameCtx ctx;
    composer.tick(ctx);
    composer.render(ctx);

    const fuse::u8 center = composer.renderer().sample(160, 120);
    expectTrue(center == 0, "3D clear skipped when dimension disabled");
}

} // namespace

int main() {
    fuse::core::initialize();

    testHybridFrameProducesPixels();
    testDimensionDisableFlags();

    if (runFramePipelineMtTests() != EXIT_SUCCESS) {
        ++g_failures;
    }

    if (runFrameBarrierIntegrationTests() != EXIT_SUCCESS) {
        ++g_failures;
    }

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_hybrid_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_hybrid_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
