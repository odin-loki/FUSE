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
    expectTrue(runtime->composer().renderer().sample(160, 120) > 0, "software path still produces pixels");

    runtime->shutdown();
    expectTrue(!runtime->isReady(), "shutdown clears ready state");
    expectTrue(!runtime->status().composerAttached, "composer detached on shutdown");

    runtime->shutdown();
    expectTrue(!runtime->isReady(), "double shutdown is idempotent");

    fuse::core::shutdown();
}

} // namespace

int main() {
    testHybridBootstrapInitShutdownOrder();

    if (g_failures == 0) {
        std::printf("fuse_hybrid_renderer_bootstrap: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_hybrid_renderer_bootstrap: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
