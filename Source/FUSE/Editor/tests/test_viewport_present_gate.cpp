#include <fuse/editor/viewport_present_gate.hpp>
#if defined(FUSE_TEST_HAS_RHI) && FUSE_TEST_HAS_RHI
// fuse_rhi exists only with FUSE_BUILD_VULKAN=ON; the editor-side gate matrix runs either way.
#include <fuse/renderer/vk/swapchain_util.hpp>
#endif

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

fuse::editor::ViewportSwapchainHandoff makeQtHandoff(bool consumed, bool qtReal, bool qtStub, void* surface) {
    fuse::editor::ViewportSwapchainHandoff handoff{};
    handoff.consumed = consumed;
    handoff.qtRealSurface = qtReal;
    handoff.qtStubSurface = qtStub;
    handoff.nativeSurface = surface;
    return handoff;
}

void testViewportPresentGateMatrix() {
    const auto pending = makeQtHandoff(false, true, false, reinterpret_cast<void*>(0x1000u));
    expectTrue(!fuse::editor::viewportQtPresentEligible(pending),
               "pending handoff is not Qt present eligible");
    expectTrue(!fuse::editor::viewportQtPresentPathReady(pending, true),
               "pending handoff is not Qt present path ready");
    expectTrue(!fuse::editor::shouldDisableSoftwarePlaceholderForEmbed(pending, true),
               "pending handoff keeps software placeholder");

    const auto stubSurface = makeQtHandoff(true, false, true, reinterpret_cast<void*>(0x2000u));
    expectTrue(!fuse::editor::viewportQtPresentEligible(stubSurface),
               "Qt stub surface is not present eligible");
    expectTrue(!fuse::editor::shouldDisableSoftwarePlaceholderForEmbed(stubSurface, true),
               "Qt stub surface keeps software placeholder");

    const auto ready = makeQtHandoff(true, true, false, reinterpret_cast<void*>(0x3000u));
    expectTrue(fuse::editor::viewportQtPresentPathReady(ready, true),
               "consumed real Qt surface is path ready when swapchain presentable");
    expectTrue(!fuse::editor::viewportQtPresentPathReady(ready, false),
               "path ready requires presentable swapchain");
    expectTrue(!fuse::editor::viewportQtPresentPathEligible(ready, true),
               "full eligibility requires compile-time Qt gate on headless CI");
    expectTrue(fuse::editor::shouldDisableSoftwarePlaceholderForEmbed(ready, true),
               "path-ready handoff retires software placeholder when swapchain wired");
    expectTrue(!fuse::editor::shouldDisableSoftwarePlaceholderForEmbed(ready, false),
               "path-ready handoff keeps placeholder until swapchain presentable");

    const auto surfaceOnly = makeQtHandoff(true, true, false, reinterpret_cast<void*>(0x4000u));
    // Same inputs as `ready` above: a consumed surface without a wired swapchain keeps the placeholder.
    expectTrue(!fuse::editor::shouldDisableSoftwarePlaceholderForEmbed(surfaceOnly, false),
               "real Qt surface alone keeps placeholder until swapchain wired");
}

void testViewportHeadlessWsiProbeSkippedOnCi() {
    expectTrue(fuse::editor::viewportHeadlessWsiProbeSkipped(),
               "headless CI skips Qt WSI probe until runtime gate enabled");
}

void testDesktopPresentGatesHeadlessSafe() {
#if defined(FUSE_TEST_HAS_RHI) && FUSE_TEST_HAS_RHI
    expectTrue(!fuse::renderer::desktopQtPresentEnabled(),
               "Qt present gate OFF by default on headless CI");
    expectTrue(!fuse::renderer::desktopQtPresentRuntimeReady(),
               "Qt present runtime unavailable without gate");
    expectTrue(!fuse::renderer::realQtPresentEligible(nullptr, 0u, nullptr),
               "realQtPresentEligible rejects null swapchain");
#endif
}

} // namespace

int main() {
    testViewportPresentGateMatrix();
    testViewportHeadlessWsiProbeSkippedOnCi();
    testDesktopPresentGatesHeadlessSafe();

    if (g_failures == 0) {
        std::printf("fuse_editor_viewport_present_gate: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_viewport_present_gate: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
