#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

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
    desc.createSwapchainPlaceholder = true;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(desc);
    expectTrue(bootstrap != nullptr, "bootstrap object allocated");

    const fuse::renderer::VulkanBootstrapStatus& status = bootstrap->status();
#if defined(FUSE_VULKAN_BACKEND)
    expectTrue(status.instanceReady, "Vulkan instance created when loader available");
    expectTrue(status.deviceReady, "Vulkan device created when GPU/ICD available");
#else
    expectTrue(!status.instanceReady, "stub mode keeps instance unavailable");
    expectTrue(status.mode == fuse::renderer::VulkanBackendMode::Stub, "stub backend mode");
#endif

    expectTrue(!status.swapchainPlaceholderReady,
               "swapchain remains placeholder until B2.2 surface wiring");
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
    const bool submitted = context->submitFrame(commands);
#if defined(FUSE_VULKAN_BACKEND)
    expectTrue(submitted, "command list accepted when Vulkan device ready");
    expectTrue(context->lastSubmittedCommandCount() == 2u, "two commands recorded");
#else
    expectTrue(!submitted, "stub mode rejects GPU submit");
#endif
}

} // namespace

int main() {
    fuse::core::initialize();

    testBootstrapHeadless();
    testRhiContextSubmitOnRenderThread();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_vulkan_bootstrap: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_vulkan_bootstrap: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
