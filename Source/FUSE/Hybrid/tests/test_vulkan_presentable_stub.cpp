#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
#include <fuse/hybrid/vulkan_presentable.hpp>
#include <fuse/platform/window.hpp>
#include <fuse/platform/window_wsi.hpp>
#include <fuse/renderer/vk/present_path.hpp>
#include <fuse/renderer/vk/surface.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testDesktopPresentGateDefaultOff() {
    expectTrue(!fuse::renderer::desktopGlfwPresentEnabled(),
               "desktop GLFW present gate OFF by default for headless CI");
    expectTrue(!fuse::renderer::desktopQtPresentEnabled(),
               "desktop Qt present gate OFF by default for headless CI");
    expectTrue(!fuse::renderer::realQtPresentEligible(nullptr, 0u, nullptr),
               "realQtPresentEligible rejects null swapchain on headless CI");
#if defined(FUSE_ENABLE_GLFW_PRESENT) && defined(FUSE_PLATFORM_WINDOW_GLFW)
    if (fuse::platform::windowWsiAvailable()) {
        expectTrue(fuse::renderer::desktopGlfwPresentRuntimeReady(),
                   "display+GLFW present runtime ready when gate enabled");
    } else {
        expectTrue(!fuse::renderer::desktopGlfwPresentRuntimeReady(),
                   "no display keeps desktop present runtime unavailable");
    }
#else
    expectTrue(!fuse::renderer::desktopGlfwPresentRuntimeReady(),
               "desktop GLFW present runtime unavailable without gate");
#endif
#if defined(FUSE_ENABLE_QT_PRESENT)
    if (fuse::platform::displayServerAvailable()) {
        expectTrue(fuse::renderer::desktopQtPresentRuntimeReady(),
                   "display+Qt present runtime ready when gate enabled");
        expectTrue(fuse::renderer::desktopPresentRuntimeReady(),
                   "unified desktop present runtime ready when Qt gate enabled");
    } else {
        expectTrue(!fuse::renderer::desktopQtPresentRuntimeReady(),
                   "no display keeps Qt present runtime unavailable");
    }
#else
    expectTrue(!fuse::renderer::desktopQtPresentRuntimeReady(),
               "desktop Qt present runtime unavailable without gate");
#endif
}

void testNullWsiBackendScaffold() {
    expectTrue(fuse::platform::activeWindowWsiKind() == fuse::platform::WindowWsiKind::Null ||
                   fuse::platform::activeWindowWsiKind() == fuse::platform::WindowWsiKind::Glfw,
               "window WSI kind is known");
    expectTrue(fuse::platform::windowWsiBackendName() != nullptr, "WSI backend name available");

    std::vector<const char*> extensions;
    fuse::platform::requiredVulkanInstanceExtensions(extensions);
#if defined(FUSE_PLATFORM_WINDOW_GLFW)
    if (fuse::platform::windowWsiAvailable()) {
        expectTrue(!extensions.empty(), "GLFW WSI publishes instance extensions");
    } else {
        expectTrue(extensions.empty(), "GLFW unavailable without display — null WSI extensions");
    }
#else
    expectTrue(extensions.empty(), "default null WSI requires no instance extensions");
#endif
}

void testGameWindowStub() {
    fuse::platform::WindowDesc desc{};
    desc.width = 640;
    desc.height = 480;

    fuse::platform::Window window(desc);
    expectTrue(window.isValid(), "B1.7 window stub is valid");
    expectTrue(window.nativeHandle().value == nullptr, "stub window has no native handle");
    expectTrue(window.width() == 640u, "stub window records width");

    const fuse::platform::VulkanSurfaceWire wire = window.vulkanSurfaceWire();
    expectTrue(!wire.presentable, "stub window is not presentable");
    expectTrue(wire.nativeSurface == nullptr, "stub window has no VkSurfaceKHR");
}

void testHeadlessPresentableSurface() {
    fuse::hybrid::VulkanPresentableDesc desc{};
    desc.backend = fuse::hybrid::PresentableBackend::Headless;

    auto presentable = fuse::hybrid::VulkanPresentable::create(desc);
    expectTrue(presentable != nullptr, "headless VulkanPresentable allocated");
    expectTrue(presentable->requiredInstanceExtensions().empty(),
               "headless path requires no WSI instance extensions");

    const fuse::renderer::SurfaceDesc surface = presentable->surfaceDesc();
    expectTrue(surface.kind == fuse::renderer::SurfaceKind::Headless, "headless surface kind");
    expectTrue(!presentable->vulkanSurface().isPresentable(), "headless surface is not presentable");
}

void testExternalSurfaceWiring() {
    fuse::renderer::SurfaceDesc external{};
    external.kind = fuse::renderer::SurfaceKind::External;
    external.nativeSurface = reinterpret_cast<void*>(0x1000);

    const fuse::renderer::VulkanSurface surface = fuse::renderer::VulkanSurface::fromDesc(external);
    expectTrue(surface.info().valid, "external surface with handle is valid");
    expectTrue(surface.isPresentable(), "external surface is presentable");
    expectTrue(surface.nativeHandle() == external.nativeSurface, "opaque handle preserved");
}

void testPresentPathThroughHybridBootstrap() {
    fuse::hybrid::HybridRendererBootstrapDesc desc{};
    desc.presentable.backend = fuse::hybrid::PresentableBackend::Headless;
    desc.presentable.vsyncMode = fuse::renderer::VsyncMode::Fifo;
    desc.renderer.rhi.bootstrap.instance.enableValidation = false;

    auto runtime = fuse::hybrid::HybridRendererBootstrap::create(desc);
    expectTrue(runtime != nullptr, "hybrid runtime allocated");
    expectTrue(runtime->presentPath() != nullptr, "present path wired through bootstrap");

    fuse::renderer::PresentPath* presentPath = runtime->presentPath();
    expectTrue(presentPath->waitInFlightFence(), "hybrid present path fence wait");
    expectTrue(presentPath->acquireImage() == UINT32_MAX, "headless hybrid acquire stub");
    expectTrue(presentPath->presentImage(), "headless hybrid present stub");
    expectTrue(presentPath->status().presentedFrames == 1u, "hybrid present frame counted");

    runtime->presentable()->requestResize(800, 600);
    expectTrue(runtime->presentable()->needsResizeRecreate(), "presentable resize queued");
    fuse::frame::FrameCtx frameCtx{};
    runtime->render(frameCtx);
    expectTrue(!presentPath->hasPendingResize(), "render applies resize on fence wait");
    expectTrue(presentPath->status().width == 800u, "resize width applied through render");
    expectTrue(presentPath->status().height == 600u, "resize height applied through render");
    expectTrue(presentPath->status().presentedFrames >= 2u, "second present frame after resize render");
#if defined(FUSE_VULKAN_BACKEND)
    if (runtime->rendererBootstrap().rhiContext()->bootstrap().status().deviceReady) {
        expectTrue(presentPath->status().queueSubmitCount >= 1u,
                   "hybrid render path records vkQueueSubmit through RHI mirror");
    } else {
        std::printf("SKIP: no Vulkan device — queue submit assertion deferred\n");
    }
#else
    std::printf("SKIP: stub Vulkan backend — queue submit assertion deferred\n");
#endif
    expectTrue(presentPath->status().presentSkippedNoWsiCount >= 1u,
               "headless CI uses honest no-WSI present sink");
    expectTrue(!presentPath->status().desktopPresentEnabled,
               "desktop GLFW present gate OFF by default");
    expectTrue(!presentPath->status().qtPresentEnabled, "desktop Qt present gate OFF by default");
    expectTrue(!presentPath->status().qtPresentRuntimeReady,
               "Qt present runtime unavailable without gate on headless CI");
    expectTrue(presentPath->status().qtRealPresentCallCount == 0u,
               "headless CI records no Qt vkQueuePresentKHR calls");

    runtime->shutdown();
}

void testVsyncModeOnPresentableDesc() {
    fuse::hybrid::VulkanPresentableDesc desc{};
    desc.vsyncMode = fuse::renderer::VsyncMode::Mailbox;

    auto presentable = fuse::hybrid::VulkanPresentable::create(desc);
    expectTrue(presentable->vsyncMode() == fuse::renderer::VsyncMode::Mailbox,
               "presentable stores vsync mode");
    expectTrue(presentable->swapchainDesc().vsyncMode == fuse::renderer::VsyncMode::Mailbox,
               "swapchain desc carries vsync mode");
}

void testHybridBootstrapHeadlessPresentable() {
    fuse::hybrid::HybridRendererBootstrapDesc desc{};
    desc.presentable.backend = fuse::hybrid::PresentableBackend::Headless;
    desc.renderer.rhi.bootstrap.instance.enableValidation = false;

    auto runtime = fuse::hybrid::HybridRendererBootstrap::create(desc);
    expectTrue(runtime != nullptr, "HybridRendererBootstrap allocated");
    expectTrue(runtime->isReady(), "hybrid runtime initialized with headless presentable");
    expectTrue(runtime->status().presentableReady, "presentable path initialized");
    expectTrue(!runtime->status().presentableSurface, "CI headless path has no WSI surface");

    runtime->shutdown();
}

void testHybridBootstrapRapidTeardownRerun() {
    constexpr fuse::u32 kCycles = 8u;
    fuse::hybrid::HybridRendererBootstrapDesc desc{};
    desc.presentable.backend = fuse::hybrid::PresentableBackend::Headless;
    desc.presentable.vsyncMode = fuse::renderer::VsyncMode::Fifo;
    desc.renderer.rhi.bootstrap.instance.enableValidation = false;

    for (fuse::u32 cycle = 0; cycle < kCycles; ++cycle) {
        auto runtime = fuse::hybrid::HybridRendererBootstrap::create(desc);
        expectTrue(runtime != nullptr, "rapid rerun allocates hybrid runtime");
        expectTrue(runtime->isReady(), "rapid rerun initializes hybrid runtime");

        fuse::frame::FrameCtx frameCtx{};
        frameCtx.frameIndex = cycle;
        runtime->render(frameCtx);

        expectTrue(runtime->presentPath() != nullptr, "rapid rerun wires present path");
        expectTrue(runtime->presentPath()->status().presentedFrames >= 1u,
                   "rapid rerun presents at least one frame");

        runtime->shutdown();
        expectTrue(!runtime->isReady(), "rapid rerun shutdown clears ready state");
    }
}

} // namespace

int main() {
    fuse::core::initialize();

    testDesktopPresentGateDefaultOff();
    testNullWsiBackendScaffold();
    testGameWindowStub();
    testHeadlessPresentableSurface();
    testExternalSurfaceWiring();
    testVsyncModeOnPresentableDesc();
    testPresentPathThroughHybridBootstrap();
    testHybridBootstrapHeadlessPresentable();
    testHybridBootstrapRapidTeardownRerun();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_hybrid_vulkan_presentable: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_hybrid_vulkan_presentable: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
