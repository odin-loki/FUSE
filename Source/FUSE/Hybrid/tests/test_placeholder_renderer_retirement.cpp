#include <fuse/core/init.hpp>
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
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

void testSoftwarePlaceholderSkippedFrameAccumulation() {
    fuse::core::initialize();

    fuse::hybrid::HybridRendererBootstrapDesc desc{};
    desc.renderer.rhi.bootstrap.instance.enableValidation = false;

    auto runtime = fuse::hybrid::HybridRendererBootstrap::create(desc);
    expectTrue(runtime != nullptr, "hybrid runtime allocated for skipped-frame test");
    runtime->composer().setSoftwarePlaceholderEnabled(false);

    fuse::world2d::World2D world2D;
    fuse::world3d::World3D world3D;
    fuse::SceneObject2D sprite("sprite");
    world2D.addSprite(&sprite);
    runtime->composer().attachWorld2D(&world2D);
    runtime->composer().attachWorld3D(&world3D);

    constexpr fuse::u32 kFrames = 4u;
    for (fuse::u32 frame = 0; frame < kFrames; ++frame) {
        fuse::frame::FrameCtx ctx{};
        ctx.frameIndex = frame + 1u;
        runtime->runFrame(ctx);
    }

    expectTrue(runtime->composer().softwarePlaceholderSkippedFrames() == kFrames,
               "disabled software placeholder accumulates skipped frame count");
    expectTrue(runtime->composer().renderer().pixelCount() == 0u,
               "disabled software placeholder leaves RGBA buffer empty");

#if defined(FUSE_HAS_VULKAN_RHI)
    expectTrue(runtime->composer().lastCommandList().commandCount() > 0u,
               "RHI mirror still records with software placeholder disabled");
#endif

    runtime->composer().setSoftwarePlaceholderEnabled(true);
    fuse::frame::FrameCtx reenableCtx{};
    reenableCtx.frameIndex = kFrames + 1u;
    runtime->runFrame(reenableCtx);
    expectTrue(runtime->composer().softwarePlaceholderEnabled(), "software placeholder can be re-enabled");
    expectTrue(runtime->composer().renderer().pixelCount() > 0u,
               "re-enabled software placeholder writes RGBA again");

    runtime->shutdown();
    fuse::core::shutdown();
}

} // namespace

int main() {
    testSoftwarePlaceholderSkippedFrameAccumulation();

    if (g_failures == 0) {
        std::printf("fuse_placeholder_renderer_retirement: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_placeholder_renderer_retirement: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
