#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/renderer_bootstrap.hpp>
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/types.hpp>

#include <cstdio>
#include <cstdlib>

using fuse::u32;

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testPhase2HeadlessIntegration() {
    fuse::core::initialize();
    expectTrue(fuse::platform::mayTouchGpuContext(), "render thread registered after core init");

    fuse::renderer::RendererBootstrapDesc desc{};
    desc.rhi.bootstrap.instance.enableValidation = false;
    desc.rhi.bootstrap.createSwapchain = false;
    desc.rhi.enableRasterPath = true;
    desc.rhi.enableCompositePass = true;
    desc.rhi.composite.defaultBlend = 0.55f;

    auto bootstrap = fuse::renderer::RendererBootstrap::create(desc);
    expectTrue(bootstrap != nullptr, "RendererBootstrap allocated");
    expectTrue(bootstrap->isReady(), "RendererBootstrap initialized on render thread");
    expectTrue(bootstrap->status().rhiContextReady, "RhiContext ready after bootstrap");

    fuse::renderer::RhiContext* rhi = bootstrap->rhiContext();
    expectTrue(rhi != nullptr, "shared RhiContext available");

#if defined(FUSE_VULKAN_BACKEND)
    expectTrue(bootstrap->status().deviceReady, "Vulkan device ready with ICD");
    expectTrue(bootstrap->status().frameManagerReady, "FrameManager ready with device");
    expectTrue(bootstrap->frameManager() != nullptr && bootstrap->frameManager()->isReady(),
               "FrameManager accessible and ready");

    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.1f, 0.2f, 0.3f);
    commands.drawSprite2D(0.f, 0.f, 0.f, 255, 128, 64);
    commands.drawSprite2D(12.f, 8.f, 0.f, 255, 200, 64);

    constexpr u32 kFramesToExercise = 3u;
    for (u32 frameIndex = 0; frameIndex < kFramesToExercise; ++frameIndex) {
        expectTrue(rhi->beginFrame(frameIndex), "beginFrame accepted on render thread");
        expectTrue(rhi->submitFrame(commands, frameIndex),
                   "submitFrame succeeds with full Phase 2 stack wired");

        expectTrue(rhi->lastGraphPassCount() >= 4u,
                   "render graph runs clear + sprites + composite + present");
        expectTrue(rhi->lastGraphBarrierCount() >= 1u, "render graph planned barriers");
        expectTrue(rhi->lastRecordedCommandCount() > 0u, "command recorder captured work");
        expectTrue(rhi->compositePass() != nullptr, "CompositePass lazy-created on submit");
        expectTrue(rhi->lastCompositeStats().framesRecorded == frameIndex + 1u,
                   "CompositePass stats advance per frame");
        expectTrue(rhi->rasterPath() != nullptr, "RasterPath lazy-created on submit");
        expectTrue(rhi->lastRasterStats().triangleDrawCount >= 1u,
                   "RasterPath records clear + triangle each frame");
        expectTrue(rhi->lastRasterStats().framesRecorded == frameIndex + 1u,
                   "RasterPath stats advance per frame");
        expectTrue(rhi->renderGraph().compileInfo().compiled,
                   "render graph compile succeeded each frame");
    }

    expectTrue(rhi->submittedFrameCount() == kFramesToExercise,
               "all integration frames submitted");
    expectTrue(rhi->currentFrameSlot() < 3u, "frame ring slot stays within triple buffer");
#else
    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.1f, 0.2f, 0.3f);
    expectTrue(!rhi->submitFrame(commands, 0u), "stub backend rejects GPU submit");
#endif

    bootstrap->shutdown();
    expectTrue(!bootstrap->isReady(), "shutdown clears bootstrap ready state");
    fuse::core::shutdown();
}

} // namespace

int main() {
    testPhase2HeadlessIntegration();

    if (g_failures == 0) {
        std::printf("fuse_vulkan_phase2_integration: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_vulkan_phase2_integration: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
