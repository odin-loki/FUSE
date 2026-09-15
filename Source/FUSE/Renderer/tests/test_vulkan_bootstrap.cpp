#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/surface.hpp>

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

void testBootstrapHeadless() {
    fuse::renderer::VulkanBootstrapDesc desc{};
    desc.instance.enableValidation = false;
    desc.createSwapchain = true;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(desc);
    expectTrue(bootstrap != nullptr, "bootstrap object allocated");

    const fuse::renderer::VulkanBootstrapStatus& status = bootstrap->status();
#if defined(FUSE_VULKAN_BACKEND)
    expectTrue(status.instanceReady, "Vulkan instance created when loader available");
    expectTrue(status.deviceReady, "Vulkan device created when GPU/ICD available");
    expectTrue(status.frameManagerReady, "frame ring created when device ready");
#else
    expectTrue(!status.instanceReady, "stub mode keeps instance unavailable");
    expectTrue(status.mode == fuse::renderer::VulkanBackendMode::Stub, "stub backend mode");
#endif

    expectTrue(status.swapchainHeadless, "CI headless path has no VkSurfaceKHR");
#if defined(FUSE_VULKAN_BACKEND)
    expectTrue(!status.swapchainReady, "headless swapchain is not presentable");
#else
    expectTrue(!status.swapchainReady, "stub mode has no swapchain");
#endif
}

void testSurfaceAbstraction() {
    fuse::renderer::SurfaceDesc headless{};
    headless.kind = fuse::renderer::SurfaceKind::Headless;
    const fuse::renderer::VulkanSurface headlessSurface =
        fuse::renderer::VulkanSurface::fromDesc(headless);
    expectTrue(headlessSurface.info().valid, "headless surface is valid");
    expectTrue(!headlessSurface.isPresentable(), "headless surface is not presentable");

    fuse::renderer::SurfaceDesc external{};
    external.kind = fuse::renderer::SurfaceKind::External;
    const fuse::renderer::VulkanSurface invalidExternal =
        fuse::renderer::VulkanSurface::fromDesc(external);
    expectTrue(!invalidExternal.info().valid, "external surface without handle is invalid");
}

void testRhiContextSubmitOnRenderThread() {
    fuse::renderer::RhiContext::Desc desc{};
    desc.bootstrap.instance.enableValidation = false;

    auto context = fuse::renderer::RhiContext::create(desc);
    expectTrue(context != nullptr, "RHI context allocated");

    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.1f, 0.2f, 0.3f);
    commands.drawSprite2D(0.f, 0.f, 0.f, 255, 128, 64);

    expectTrue(fuse::platform::mayTouchGpuContext(), "main thread registered as render thread");
#if defined(FUSE_VULKAN_BACKEND)
    expectTrue(context->beginFrame(0u), "beginFrame accepted on render thread");
#endif
    const bool submitted = context->submitFrame(commands, 0u);
#if defined(FUSE_VULKAN_BACKEND)
    expectTrue(submitted, "command list accepted when Vulkan device ready");
    expectTrue(context->lastSubmittedCommandCount() == 2u, "two commands recorded");
    expectTrue(context->submittedFrameCount() == 1u, "one frame submitted");
#else
    expectTrue(!submitted, "stub mode rejects GPU submit");
#endif
}

} // namespace

int main() {
    fuse::core::initialize();

    testBootstrapHeadless();
    testSurfaceAbstraction();
    testRhiContextSubmitOnRenderThread();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_vulkan_bootstrap: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_vulkan_bootstrap: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
