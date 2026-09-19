#include "hybrid_module_gates.hpp"

#include <fuse/core/init.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/log/logger.hpp>

#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
#endif

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "demo_hybrid_hud FAIL: %s\n", message);
        ++g_failures;
    }
}

} // namespace

int main() {
    fuse::log::info("demo_hybrid_hud: U4 hybrid frame + U5 prestarter §10 module gates");

    check(fuse::core::initialize(), "fuse_core initialize");

#if defined(FUSE_HAS_VULKAN_RHI)
    fuse::hybrid::HybridRendererBootstrapDesc bootstrapDesc{};
    bootstrapDesc.renderer.rhi.bootstrap.instance.enableValidation = false;
    auto runtime = fuse::hybrid::HybridRendererBootstrap::create(bootstrapDesc);
    check(runtime != nullptr, "HybridRendererBootstrap allocated");
    check(runtime->isReady(), "B2.10 renderer bootstrap initialized");
    fuse::hybrid::HybridComposer& composer = runtime->composer();
#else
    fuse::hybrid::HybridComposer composer;
#endif

    fuse::hybrid::gates::State gateState;
    fuse::hybrid::gates::setup(gateState, composer);

    fuse::frame::FrameCtx ctx;
    for (int frame = 0; frame < fuse::hybrid::gates::kFrameCount; ++frame) {
        ctx.dt = fuse::hybrid::gates::kDt;
        ctx.time = static_cast<float>(frame) * fuse::hybrid::gates::kDt;
        ctx.frameIndex = static_cast<fuse::u32>(frame);

        fuse::hybrid::gates::tickFrame(gateState, composer, ctx);

#if defined(FUSE_HAS_VULKAN_RHI)
        runtime->render(ctx);
#else
        composer.render(ctx);
#endif
    }

    const fuse::hybrid::gates::VerifyResult result = fuse::hybrid::gates::verify(gateState, composer);
    check(result.ok, result.message != nullptr ? result.message : "U5 module gates verified");

    fuse::log::info("demo_hybrid_hud: rendered %d frames at %ux%u (RGBA software buffer)",
                    fuse::hybrid::gates::kFrameCount,
                    composer.renderer().width(),
                    composer.renderer().height());
    fuse::log::info("demo_hybrid_hud: real GL/Vulkan presentation deferred to Track B RHI");

#if defined(FUSE_HAS_VULKAN_RHI)
    runtime->shutdown();
#endif
    fuse::core::shutdown();

    if (g_failures == 0) {
        fuse::log::info("demo_hybrid_hud: PASS");
        return EXIT_SUCCESS;
    }

    return EXIT_FAILURE;
}
