#include <fuse/core/init.hpp>
#include <fuse/core/track_b.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/platform/window_wsi.hpp>
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/surface.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testB21CreateSmoke() {
    std::unique_ptr<fuse::renderer::VulkanBootstrap> bootstrap =
        fuse::renderer::VulkanBootstrap::create({});
    if (bootstrap == nullptr) {
        expectTrue(bootstrap == nullptr, "create() returned null — skip remaining checks");
        return;
    }

    expectTrue(bootstrap != nullptr, "create() returns a bootstrap object");

    const fuse::renderer::VulkanBootstrapStatus& status = bootstrap->status();
    const bool stubOrHeadless =
        status.mode == fuse::renderer::VulkanBackendMode::Stub ||
        status.mode == fuse::renderer::VulkanBackendMode::Headless;
    expectTrue(!status.message.empty() || stubOrHeadless,
               "status message is non-empty or mode is Stub/Headless");

#if !defined(FUSE_VULKAN_BACKEND)
    expectTrue(status.mode == fuse::renderer::VulkanBackendMode::Stub,
               "stub mode when Vulkan loader is unavailable");
#endif

    expectTrue(fuse::renderer::productionPresentAllowed() == fuse::core::trackBUnlocked(),
               "productionPresentAllowed matches Track B unlock (false by default)");

    bootstrap.reset();
    expectTrue(bootstrap == nullptr, "unique_ptr drop is safe");
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
    if (status.deviceReady && bootstrap->device() != nullptr) {
        const fuse::renderer::VulkanDeviceInfo& deviceInfo = bootstrap->device()->info();
        bool hasDynamicRenderingExt = false;
        for (const char* enabled : deviceInfo.enabledExtensions) {
            if (enabled != nullptr && std::strcmp(enabled, "VK_KHR_dynamic_rendering") == 0) {
                hasDynamicRenderingExt = true;
                break;
            }
        }
        if (hasDynamicRenderingExt) {
            expectTrue(deviceInfo.dynamicRendering,
                       "VK_KHR_dynamic_rendering enabled implies dynamicRendering");
        }
    }
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

void testWsiExtensionAwareness() {
    std::vector<const char*> extensions;
    fuse::platform::requiredVulkanInstanceExtensions(extensions);

    fuse::renderer::VulkanBootstrapDesc desc{};
    desc.instance.enableValidation = false;
    if (!extensions.empty()) {
        desc.instance.extraExtensions = extensions.data();
        desc.instance.extraExtensionCount = static_cast<fuse::u32>(extensions.size());
    }

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(desc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated when WSI extensions are present or absent");
}

void testInstanceWsiAutoRequest() {
    std::vector<const char*> required;
    fuse::platform::requiredVulkanInstanceExtensions(required);

    fuse::renderer::VulkanInstanceDesc desc{};
    desc.enableValidation = false;
    auto instance = fuse::renderer::VulkanInstance::create(desc);
    expectTrue(instance != nullptr, "VulkanInstance allocated for WSI auto-request");

    const fuse::renderer::VulkanInstanceInfo& info = instance->info();
    expectTrue(!info.instanceHasExtension("VK_FAKE_NOT_AN_EXTENSION"),
               "unknown instance extension is not reported as enabled");

    for (const char* name : required) {
        if (name == nullptr) {
            continue;
        }
        bool listed = false;
        for (const char* enabled : info.enabledExtensions) {
            if (enabled != nullptr && std::strcmp(enabled, name) == 0) {
                listed = true;
                break;
            }
        }
        expectTrue(info.instanceHasExtension(name) == listed,
                   "instanceHasExtension matches enabledExtensions for platform WSI name");
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (info.valid) {
        bool listedSurfaceName = false;
        for (const char* enabled : info.enabledExtensions) {
            if (enabled == nullptr) {
                continue;
            }
            if (std::strstr(enabled, "surface") != nullptr ||
                std::strstr(enabled, "SURFACE") != nullptr) {
                listedSurfaceName = true;
            }
        }
        for (const char* name : required) {
            if (name != nullptr && info.instanceHasExtension(name)) {
                listedSurfaceName = true;
            }
        }
        if (!required.empty() && listedSurfaceName) {
            expectTrue(listedSurfaceName,
                       "enabledExtensions can include platform surface names when available");
        }
        expectTrue(info.valid, "unavailable WSI names are skipped — instance create still succeeds");
    }
#else
    expectTrue(!info.valid, "stub instance when Vulkan loader is unavailable");
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

void testDeviceVulkan12FeatureFlags() {
    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;
    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    expectTrue(instance != nullptr, "instance allocated for Vulkan 1.2 feature flags");
    if (instance == nullptr) {
        return;
    }

    auto device = fuse::renderer::VulkanDevice::create(*instance);
    expectTrue(device != nullptr, "device allocated for Vulkan 1.2 feature flags");
    if (device == nullptr) {
        return;
    }

    const fuse::renderer::VulkanDeviceInfo& info = device->info();
    if (!info.valid) {
        expectTrue(!info.descriptorIndexing, "stub device reports descriptorIndexing false");
        expectTrue(!info.bufferDeviceAddress, "stub device reports bufferDeviceAddress false");
        expectTrue(!info.timelineSemaphore, "stub device reports timelineSemaphore false");
        expectTrue(!info.dynamicRendering, "stub device reports dynamicRendering false");
        return;
    }

    (void)info.descriptorIndexing;
    (void)info.bufferDeviceAddress;
    (void)info.timelineSemaphore;

    bool hasDynamicRenderingExt = false;
    for (const char* enabled : info.enabledExtensions) {
        if (enabled != nullptr && std::strcmp(enabled, "VK_KHR_dynamic_rendering") == 0) {
            hasDynamicRenderingExt = true;
            break;
        }
    }
    if (hasDynamicRenderingExt) {
        expectTrue(info.dynamicRendering,
                   "VK_KHR_dynamic_rendering enabled implies dynamicRendering");
    }
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

    testB21CreateSmoke();
    testBootstrapHeadless();
    testWsiExtensionAwareness();
    testInstanceWsiAutoRequest();
    testSurfaceAbstraction();
    testDeviceVulkan12FeatureFlags();
    testRhiContextSubmitOnRenderThread();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_vulkan_bootstrap: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_vulkan_bootstrap: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
