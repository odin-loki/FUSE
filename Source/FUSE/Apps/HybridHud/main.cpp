#include "demo_present.hpp"
#include "hybrid_module_gates.hpp"

#include <fuse/core/init.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/log/logger.hpp>

#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
#endif
#if defined(FUSE_HYBRID_HUD_HAS_UI)
#include "hud_ui.hpp"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "demo_hybrid_hud FAIL: %s\n", message);
        ++g_failures;
    }
}

} // namespace

int main(int argc, char** argv) {
    fuse::log::info("demo_hybrid_hud: outpost hybrid vertical slice");

    const bool presentGameWindow = fuse::demo::present::requestedFromArgv(argc, argv);
    const int frameTarget = presentGameWindow ? 3 : fuse::hybrid::gates::kFrameCount;

    check(fuse::core::initialize(), "fuse_core initialize");

#if defined(FUSE_HAS_VULKAN_RHI)
    fuse::demo::present::HostPresentUnlock presentUnlock;
    fuse::hybrid::HybridRendererBootstrapDesc bootstrapDesc{};
    bootstrapDesc.renderer.rhi.bootstrap.instance.enableValidation = false;
    bootstrapDesc.scene.width = 320;
    bootstrapDesc.scene.height = 240;
    if (presentGameWindow && fuse::demo::present::gameWindowPresentAvailable()) {
        fuse::demo::present::applyGameWindowToBootstrapDesc(bootstrapDesc, 320, 240, "demo_hybrid_hud");
    }
    auto runtime = fuse::hybrid::HybridRendererBootstrap::create(bootstrapDesc);
    check(runtime != nullptr, "HybridRendererBootstrap allocated");
    check(runtime->isReady(), "B2.10 renderer bootstrap initialized");
    fuse::hybrid::HybridComposer& composer = runtime->composer();
#else
    fuse::hybrid::HybridComposer composer;
#endif

    fuse::hybrid::gates::State gateState;
    fuse::hybrid::gates::setup(gateState, composer);

#if defined(FUSE_HYBRID_HUD_HAS_UI)
    // The HUD on a FUSE UI context (real text + widgets); --legacy-hud keeps the placeholder UI quad.
    bool legacyHud = false;
    for (int i = 1; i < argc; ++i) {
        legacyHud = legacyHud || std::strcmp(argv[i], "--legacy-hud") == 0;
    }
    fuse::hybrid::hud::HudUi hudUi;
    if (!legacyHud) {
        (void)hudUi.init(composer, 320, 240);
    }
#endif

    fuse::frame::FrameCtx ctx;
    for (int frame = 0; frame < frameTarget; ++frame) {
        ctx.dt = fuse::hybrid::gates::kDt;
        ctx.time = static_cast<float>(frame) * fuse::hybrid::gates::kDt;
        ctx.frameIndex = static_cast<fuse::u32>(frame);

        fuse::hybrid::gates::tickFrame(gateState, composer, ctx);
#if defined(FUSE_HYBRID_HUD_HAS_UI)
        hudUi.update(composer, gateState, ctx);
#endif

#if defined(FUSE_HAS_VULKAN_RHI)
        if (presentGameWindow) {
            runtime->runFrame(ctx);
        } else {
            runtime->render(ctx);
        }
#else
        composer.render(ctx);
#endif
    }

    if (!presentGameWindow) {
        const fuse::hybrid::gates::VerifyResult result = fuse::hybrid::gates::verify(gateState, composer);
        check(result.ok, result.message != nullptr ? result.message : "outpost slice verified");
#if defined(FUSE_HYBRID_HUD_HAS_UI)
        if (hudUi.active() && composer.gpuSceneActive()) {
            const fuse::hybrid::hud::HudCheck hud = hudUi.check(*composer.gpuScene());
            fuse::log::info("demo_hybrid_hud: FUSE UI HUD: %u glyph quads, %u opaque UI pixels (%u off by > 1/255), "
                            "%u text pixels",
                            hud.glyphs, hud.covered, hud.mismatched, hud.textPixels);
            check(hud.overlayRan, "FUSE UI HUD drawn in the UI stage");
            check(hud.glyphs > 0u && hud.textPixels > 0u, "HUD text visible in the GPU readback");
            check(hud.covered > 0u && hud.mismatched == 0u, "HUD pixels == the CPU rasteriser (<= 1/255)");
        }
#endif
    }

#if defined(FUSE_HAS_VULKAN_RHI)
    if (composer.gpuSceneActive()) {
        fuse::log::info("demo_hybrid_hud: rendered %d frames at %ux%u on the GPU (SceneRenderer + sprite/UI layer, "
                        "%llu GPU frames, checked by readback)",
                        fuse::hybrid::gates::kFrameCount, composer.gpuScene()->width(), composer.gpuScene()->height(),
                        static_cast<unsigned long long>(composer.gpuSceneFrames()));
        if (presentGameWindow) {
            check(fuse::demo::present::realPresentCallCount(*runtime) >= 1u,
                  "vkQueuePresentKHR via GameWindow present path");
        }
    } else
#endif
    {
        fuse::log::info("demo_hybrid_hud: rendered %d frames at %ux%u (PlaceholderRenderer fallback: %s)",
                        fuse::hybrid::gates::kFrameCount, composer.renderer().width(), composer.renderer().height(),
#if defined(FUSE_HAS_VULKAN_RHI)
                        composer.gpuSceneStatus()
#else
                        "no Vulkan RHI"
#endif
        );
    }

#if defined(FUSE_HYBRID_HUD_HAS_UI)
    hudUi.shutdown(composer);
#endif
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
