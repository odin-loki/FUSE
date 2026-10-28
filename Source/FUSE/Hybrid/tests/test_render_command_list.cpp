#include <fuse/core/init.hpp>
#include <fuse/hybrid/cmd_count.hpp>
#include <fuse/hybrid/command_cap.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/render_command_list.hpp>
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

void testCmdCountOk() {
    expectTrue(fuse::hybrid::cmd_count_ok(0), "cmd_count_ok(0) is true");
    expectTrue(fuse::hybrid::cmd_count_ok(1), "cmd_count_ok(1) is true");
    expectTrue(fuse::hybrid::cmd_count_ok(1 << 20), "cmd_count_ok(1<<20) is true");
    expectTrue(!fuse::hybrid::cmd_count_ok(-1), "cmd_count_ok(-1) is false");
    expectTrue(!fuse::hybrid::cmd_count_ok((1 << 20) + 1), "cmd_count_ok((1<<20)+1) is false");
}

void testCommandCountOk() {
    expectTrue(fuse::hybrid::command_count_ok(3u, 4u), "count below cap is ok");
    expectTrue(fuse::hybrid::command_count_ok(4u, 4u), "count equal to cap is ok");
    expectTrue(!fuse::hybrid::command_count_ok(5u, 4u), "count above cap is not ok");
    expectTrue(!fuse::hybrid::command_count_ok(0u, 0u), "zero cap is not ok");
}

void testHybridMirrorsCommandList() {
    fuse::hybrid::HybridComposer composer;
    fuse::world2d::World2D world2D;
    fuse::world3d::World3D world3D;
    fuse::SceneObject2D sprite("sprite");

    sprite.setPosition(0.f, 0.f);
    world2D.addSprite(&sprite);
    world3D.setClearColor(0.2f, 0.3f, 0.4f);

    composer.attachWorld2D(&world2D);
    composer.attachWorld3D(&world3D);

    fuse::frame::FrameCtx ctx;
    composer.tick(ctx);
    composer.render(ctx);

    expectTrue(fuse::platform::mayTouchGpuContext(), "render path stays on render thread");

#if defined(FUSE_HAS_VULKAN_RHI)
    expectTrue(composer.hasRhiRecording(), "HybridComposer exposes RHI recording when enabled");
    expectTrue(composer.lastCommandList().commandCount() >= 2u,
               "clear + sprite commands mirrored to command list");
    expectTrue(composer.rhiContext() != nullptr, "lazy RHI context created");
#else
    expectTrue(!composer.hasRhiRecording(), "software-only build skips RHI recording");
#endif

    expectTrue(composer.renderer().pixelCount() > 0u, "placeholder renderer still produces pixels");
}

} // namespace

int main() {
    testCmdCountOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_render_command_list: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testCommandCountOk();
    fuse::core::initialize();
    testHybridMirrorsCommandList();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_render_command_list: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_render_command_list: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
