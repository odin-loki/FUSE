#include <fuse/core/init.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/viewport_vulkan_surface.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testRuntimeViewportHeadlessTick() {
    fuse::editor::EditorHost host;
    host.runtimeViewport().setProjectLabel("embed_test");
    host.runtimeViewport().requestResize(640, 480);
    host.gameTick();

    expectTrue(host.runtimeViewport().panel().tickCount() == 1u, "viewport panel ticked on game thread");
    expectTrue(host.runtimeViewport().panel().width() == 640u, "viewport resize applied");
    expectTrue(host.runtimeViewport().runtimeTickCount() == 1u, "runtime viewport hook ticked");
}

void testRuntimeEmbedSessionCounters() {
    fuse::editor::EditorHost host;
    host.runtimeViewport().setProjectRoot("Samples/unification/demo_3d_empty");
    host.runtimeViewport().setProjectLabel("embed_test");
    host.gameTick();
    host.gameTick();

    const fuse::editor::RuntimeEmbedSession& session = host.runtimeViewport().embedSession();
    expectTrue(session.headlessPresentTicks >= 1u, "embed session records headless present ticks");
    expectTrue(!session.wsiBackendName.empty(), "embed session records active WSI backend");
    expectTrue(session.usesHeadlessGpuPath, "embed session marks null WSI headless GPU path in CI");
    expectTrue(session.wsiPresentPathTicks >= 1u || session.headlessPresentTicks >= 1u,
               "embed session records WSI present path ticks");
#if defined(FUSE_VULKAN_BACKEND)
    if (session.headlessGpuReady) {
        expectTrue(session.submittedFrames >= 1u, "headless GPU path submits frames when device ready");
    }
#endif
}

void testRuntimeEmbedSwapchainHandoff() {
    fuse::editor::EditorHost host;
    host.runtimeViewport().requestResize(800, 600);
    host.runtimeViewport().setExternalSurfaceHandle(reinterpret_cast<void*>(0x1u), 800, 600,
                                                    "unit_test_stub", true);
    host.gameTick();

    const fuse::editor::RuntimeEmbedSession& session = host.runtimeViewport().embedSession();
    expectTrue(session.surfaceHandoffCount >= 1u, "surface handoff recorded");
    expectTrue(session.swapchainWiringAttempts >= 1u, "swapchain wiring attempted");
    expectTrue(session.surfaceHandoffConsumed, "surface handoff consumed on game thread");
}

void testRuntimeViewportSwapchainRecreateStub() {
    fuse::editor::EditorHost host;
    host.runtimeViewport().setProjectLabel("recreate_test");
    host.runtimeViewport().requestResize(640, 480);
    host.gameTick();

    host.runtimeViewport().requestResize(1024, 768);
    host.gameTick();

    const fuse::editor::RuntimeEmbedSession& session = host.runtimeViewport().embedSession();
    expectTrue(session.swapchainRecreateAttempts >= 1u, "viewport resize queues swapchain recreate");
#if defined(FUSE_VULKAN_BACKEND)
    if (session.headlessGpuReady) {
        expectTrue(session.swapchainRecreateCount >= 1u,
                   "headless viewport swapchain recreate applied when GPU ready");
        expectTrue(session.consumedSwapchainPresentTicks >= 1u,
                   "consumed swapchain present cycle after recreate when GPU ready");
    }
#endif
    expectTrue(host.runtimeViewport().panel().width() == 1024u, "final viewport width applied");
    expectTrue(host.runtimeViewport().panel().height() == 768u, "final viewport height applied");
}

void testRuntimeEmbedTeardownStress() {
    fuse::editor::EditorHost host;
    constexpr fuse::u32 kCycles = 4u;

    for (fuse::u32 cycle = 0; cycle < kCycles; ++cycle) {
        const fuse::u64 winId = 9000u + cycle;
        const fuse::editor::ViewportVulkanSurfaceResult surface =
            fuse::editor::createViewportVulkanSurfaceFromWinId(winId, 640u, 360u);
        expectTrue(surface.valid, "embed teardown stress bootstraps viewport surface");

        fuse::editor::EditorCommand widthCmd;
        widthCmd.kind = fuse::editor::CommandKind::SetProperty;
        widthCmd.propertyName = "viewport.width";
        widthCmd.propertyValue = "640";
        host.postFromUi(std::move(widthCmd));

        fuse::editor::EditorCommand heightCmd;
        heightCmd.kind = fuse::editor::CommandKind::SetProperty;
        heightCmd.propertyName = "viewport.height";
        heightCmd.propertyValue = "360";
        host.postFromUi(std::move(heightCmd));

        fuse::editor::EditorCommand surfaceCmd;
        surfaceCmd.kind = fuse::editor::CommandKind::SetProperty;
        surfaceCmd.propertyName = "viewport.vk_surface_handle";
        surfaceCmd.propertyValue = std::to_string(winId);
        host.postFromUi(std::move(surfaceCmd));

        host.gameTick();

        expectTrue(host.runtimeViewport().embedSession().surfaceHandoffConsumed,
                   "embed teardown stress consumes surface handoff");

        fuse::editor::ViewportVulkanSurfaceResult mutableSurface = surface;
        fuse::editor::destroyViewportVulkanSurface(mutableSurface);
        expectTrue(!mutableSurface.valid, "embed teardown stress destroys bootstrap surface");
    }

    expectTrue(host.runtimeViewport().embedSession().surfaceHandoffCount >= kCycles,
               "embed teardown stress records repeated handoffs");
}

} // namespace

int main() {
    fuse::core::initialize();
    testRuntimeViewportHeadlessTick();
    testRuntimeEmbedSessionCounters();
    testRuntimeEmbedSwapchainHandoff();
    testRuntimeViewportSwapchainRecreateStub();
    testRuntimeEmbedTeardownStress();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_runtime_embed_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_runtime_embed_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
