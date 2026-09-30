#include <fuse/core/init.hpp>
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
#include <fuse/platform/gl_context.hpp>
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

void testHybridBootstrapInitShutdownOrder() {
    fuse::core::initialize();

    fuse::hybrid::HybridRendererBootstrapDesc desc{};
    desc.renderer.rhi.bootstrap.instance.enableValidation = false;

    auto runtime = fuse::hybrid::HybridRendererBootstrap::create(desc);
    expectTrue(runtime != nullptr, "HybridRendererBootstrap allocated");
    expectTrue(runtime->isReady(), "hybrid runtime initialized");
    expectTrue(runtime->status().rendererReady, "renderer stack ready");
    expectTrue(runtime->status().composerAttached, "composer wired to shared RhiContext");

#if defined(FUSE_HAS_VULKAN_RHI)
    expectTrue(runtime->composer().rhiContext() == runtime->rendererBootstrap().rhiContext(),
               "composer uses shared RhiContext from RendererBootstrap");
#endif

    fuse::world2d::World2D world2D;
    fuse::world3d::World3D world3D;
    fuse::SceneObject2D sprite("sprite");
    sprite.setPosition(0.f, 0.f);
    world2D.addSprite(&sprite);
    world3D.setClearColor(0.1f, 0.15f, 0.25f);

    runtime->composer().attachWorld2D(&world2D);
    runtime->composer().attachWorld3D(&world3D);

    fuse::frame::FrameCtx ctx;
    ctx.frameIndex = 1u;
    runtime->runFrame(ctx);

    expectTrue(runtime->composer().frameCount() == 1u, "runFrame ticked one frame");
    // E03: with a Vulkan device the frame is GPU-rendered (SceneRenderer) and the placeholder is off; without one
    // (stub backend / no ICD) the PlaceholderRenderer fallback draws it.
    if (runtime->composer().gpuSceneActive()) {
#if defined(FUSE_HAS_VULKAN_RHI)
        expectTrue(runtime->composer().gpuSceneFrames() == 1u, "GPU scene path rendered the frame");
        expectTrue(runtime->gpuScene() != nullptr && runtime->gpuScene()->waitIdle() &&
                       runtime->gpuScene()->readbackPixels() != nullptr,
                   "GPU frame read back from the headless target");
        const fuse::u8* px = runtime->gpuScene()->readbackPixels();
        if (px != nullptr) {
            const fuse::u32 w = runtime->gpuScene()->width();
            const fuse::u32 h = runtime->gpuScene()->height();
            const fuse::u8* centre = px + (static_cast<std::size_t>(h / 2u) * w + w / 2u) * 4u;
            // The World2D sprite sits at the frame centre: (255, 200, 64) over the 3D frame.
            expectTrue(centre[0] >= 254u && centre[1] >= 199u && centre[1] <= 201u && centre[2] >= 63u && centre[2] <= 65u,
                       "GPU path draws the 2D sprite over the 3D frame");
        }
        expectTrue(!runtime->composer().softwarePlaceholderEnabled(), "placeholder off while the GPU scene renders");
#endif
    } else {
        expectTrue(runtime->composer().renderer().sample(160, 120) > 0, "software path still produces pixels");
    }

#if defined(FUSE_HAS_VULKAN_RHI)
    expectTrue(runtime->presentPath() != nullptr, "present path wired through hybrid bootstrap");
    expectTrue(runtime->presentPath()->status().presentedFrames >= 1u,
               "present path exercised during runFrame");
    expectTrue(runtime->composer().lastCommandList().commandCount() > 0u,
               "RHI command mirror recorded on render thread");
#endif

    runtime->shutdown();
    expectTrue(!runtime->isReady(), "shutdown clears ready state");
    expectTrue(!runtime->status().composerAttached, "composer detached on shutdown");

    runtime->shutdown();
    expectTrue(!runtime->isReady(), "double shutdown is idempotent");

    fuse::core::shutdown();
}

void testHybridComposerSoftwarePlaceholderToggle() {
    fuse::core::initialize();

    fuse::hybrid::HybridRendererBootstrapDesc desc{};
    desc.renderer.rhi.bootstrap.instance.enableValidation = false;

    auto runtime = fuse::hybrid::HybridRendererBootstrap::create(desc);
    expectTrue(runtime != nullptr, "HybridRendererBootstrap allocated for placeholder toggle");
    {
        fuse::frame::FrameCtx first;
        first.frameIndex = 1u;
        runtime->runFrame(first);
    }
    expectTrue(runtime->composer().softwarePlaceholderEnabled() == !runtime->composer().gpuSceneActive(),
               "software placeholder on by default only without the GPU scene (no Vulkan device)");

    runtime->composer().setSoftwarePlaceholderEnabled(false);
    expectTrue(!runtime->composer().softwarePlaceholderEnabled(), "software placeholder can be disabled");

    fuse::frame::FrameCtx ctx;
    ctx.frameIndex = 2u;
    runtime->runFrame(ctx);

    expectTrue(runtime->composer().renderer().pixelCount() == 0u,
               "disabled software placeholder skips RGBA buffer writes");
    expectTrue(runtime->composer().softwarePlaceholderSkippedFrames() ==
                   (runtime->composer().gpuSceneActive() ? 2u : 1u),
               "disabled software placeholder records skipped frame count");

#if defined(FUSE_HAS_VULKAN_RHI)
    expectTrue(runtime->composer().lastCommandList().commandCount() > 0u,
               "RHI mirror still records when software placeholder disabled");
#endif

    runtime->shutdown();
    fuse::core::shutdown();
}

} // namespace

int main() {
    testHybridBootstrapInitShutdownOrder();
    testHybridComposerSoftwarePlaceholderToggle();

    if (g_failures == 0) {
        std::printf("fuse_hybrid_renderer_bootstrap: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_hybrid_renderer_bootstrap: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
