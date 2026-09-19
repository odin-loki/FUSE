#include <fuse/core/init.hpp>
#include <fuse/editor/editor_host.hpp>

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

} // namespace

int main() {
    fuse::core::initialize();
    testRuntimeViewportHeadlessTick();
    testRuntimeEmbedSessionCounters();
    testRuntimeEmbedSwapchainHandoff();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_runtime_embed_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_runtime_embed_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
