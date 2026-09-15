#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/renderer_bootstrap.hpp>
#include <fuse/renderer/rhi_context.hpp>

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

void testInitRequiresCore() {
    fuse::renderer::RendererBootstrapDesc desc{};
    desc.requireCoreInitialized = true;

    auto bootstrap = fuse::renderer::RendererBootstrap::create(desc);
    expectTrue(bootstrap != nullptr, "bootstrap object allocated");
    expectTrue(!bootstrap->isReady(), "bootstrap rejects init before core::initialize");
    expectTrue(!bootstrap->status().initialized, "status reflects failed init");
}

void testInitShutdownOrder() {
    fuse::core::initialize();
    expectTrue(fuse::platform::mayTouchGpuContext(), "main thread is render thread after core init");

    {
        fuse::renderer::RendererBootstrapDesc desc{};
        desc.rhi.bootstrap.instance.enableValidation = false;

        auto bootstrap = fuse::renderer::RendererBootstrap::create(desc);
        expectTrue(bootstrap != nullptr, "bootstrap allocated after core init");
        expectTrue(bootstrap->isReady(), "bootstrap initialized on render thread");
        expectTrue(bootstrap->status().rhiContextReady, "RhiContext created");
        expectTrue(bootstrap->rhiContext() != nullptr, "RhiContext pointer valid");

#if defined(FUSE_VULKAN_BACKEND)
        expectTrue(bootstrap->status().deviceReady, "Vulkan device ready when loader available");
        expectTrue(bootstrap->status().frameManagerReady, "FrameManager ready with device");
        expectTrue(bootstrap->frameManager() != nullptr, "FrameManager accessible");
        expectTrue(bootstrap->frameManager()->isReady(), "FrameManager reports ready");
#else
        expectTrue(!bootstrap->status().deviceReady, "stub backend keeps device unavailable");
#endif

        bootstrap->shutdown();
        expectTrue(!bootstrap->isReady(), "shutdown clears ready state");
        expectTrue(bootstrap->rhiContext() == nullptr, "RhiContext destroyed on shutdown");
        expectTrue(bootstrap->frameManager() == nullptr, "FrameManager gone after shutdown");
    }

    fuse::core::shutdown();
}

void testDoubleShutdownSafe() {
    fuse::core::initialize();

    auto bootstrap = fuse::renderer::RendererBootstrap::create({});
    expectTrue(bootstrap != nullptr, "bootstrap allocated");
    expectTrue(bootstrap->isReady(), "bootstrap initialized");

    bootstrap->shutdown();
    bootstrap->shutdown();
    expectTrue(!bootstrap->isReady(), "double shutdown is idempotent");

    fuse::core::shutdown();
}

void testSubmitAfterBootstrapInit() {
    fuse::core::initialize();

    auto bootstrap = fuse::renderer::RendererBootstrap::create({});
    expectTrue(bootstrap != nullptr && bootstrap->isReady(), "bootstrap ready for frame submit");

    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.1f, 0.2f, 0.3f);

    fuse::renderer::RhiContext* rhi = bootstrap->rhiContext();
    expectTrue(rhi != nullptr, "shared RhiContext available");

#if defined(FUSE_VULKAN_BACKEND)
    expectTrue(rhi->beginFrame(0u), "beginFrame on render thread");
    expectTrue(rhi->submitFrame(commands, 0u), "submitFrame succeeds with Vulkan backend");
    expectTrue(rhi->submittedFrameCount() == 1u, "one frame submitted");
#else
    expectTrue(!rhi->submitFrame(commands, 0u), "stub backend rejects GPU submit");
#endif

    bootstrap->shutdown();
    fuse::core::shutdown();
}

} // namespace

int main() {
    testInitRequiresCore();
    testInitShutdownOrder();
    testDoubleShutdownSafe();
    testSubmitAfterBootstrapInit();

    if (g_failures == 0) {
        std::printf("fuse_renderer_bootstrap: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_renderer_bootstrap: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
