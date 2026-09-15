#include <fuse/core/init.hpp>
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
#include <fuse/hybrid/vulkan_presentable.hpp>
#include <fuse/platform/window.hpp>
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

void testHybridBootstrapHeadlessPresentable() {
    fuse::core::initialize();

    fuse::hybrid::HybridRendererBootstrapDesc desc{};
    desc.presentable.backend = fuse::hybrid::PresentableBackend::Headless;
    desc.renderer.rhi.bootstrap.instance.enableValidation = false;

    auto runtime = fuse::hybrid::HybridRendererBootstrap::create(desc);
    expectTrue(runtime != nullptr, "HybridRendererBootstrap allocated");
    expectTrue(runtime->isReady(), "hybrid runtime initialized with headless presentable");
    expectTrue(runtime->status().presentableReady, "presentable path initialized");
    expectTrue(!runtime->status().presentableSurface, "CI headless path has no WSI surface");

    runtime->shutdown();
    fuse::core::shutdown();
}

} // namespace

int main() {
    testGameWindowStub();
    testHeadlessPresentableSurface();
    testExternalSurfaceWiring();
    testHybridBootstrapHeadlessPresentable();

    if (g_failures == 0) {
        std::printf("fuse_hybrid_vulkan_presentable: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_hybrid_vulkan_presentable: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
