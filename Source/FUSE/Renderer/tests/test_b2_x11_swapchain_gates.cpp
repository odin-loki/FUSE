// B1.8 / B2 carry-forward gates on the native X11 window backend (Lavapipe + Xvfb):
//   * "get_vulkan_surface returns a valid VkSurfaceKHR — verified by Vulkan validation layers"
//   * "Swapchain creates at 1920x1080, triple-buffered — resize correctly rebuilds without crash"
//
// Flow: X11 Window 1920x1080 -> VulkanInstance (VK_LAYER_KHRONOS_validation + WSI extensions)
// -> fuse::platform::createVulkanSurface (VK_KHR_xlib_surface) -> VulkanDevice(requirePresentation)
// -> VulkanSwapchain 1920x1080 x3 -> 10 presented frames (clear + vkQueuePresentKHR) ->
// Window::resize(1280x720) -> WindowResized via EventPump -> VulkanSwapchain::rebuild -> 10 more
// frames. A test-owned debug messenger counts validation errors; any error fails the gate.
// Exit 77 (ctest SKIP) without the X11 backend, a display, a Vulkan ICD or the validation layer.

#include <fuse/core/init.hpp>
#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/window.hpp>
#include <fuse/platform/window_wsi.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/swapchain.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace {

constexpr int kSkip = 77;
int g_failures = 0;

[[maybe_unused]] void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    } else {
        std::printf("  ok: %s\n", message);
    }
}

#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_PLATFORM_WINDOW_X11)

using fuse::u32;

u32 g_validationErrors = 0;
u32 g_validationWarnings = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL countingCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                                const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                void* /*user*/) {
    const char* msg = (data != nullptr && data->pMessage != nullptr) ? data->pMessage : "";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        ++g_validationErrors;
        std::fprintf(stderr, "VALIDATION ERROR: %s\n", msg);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        ++g_validationWarnings;
        std::fprintf(stderr, "validation warning: %s\n", msg);
    }
    return VK_FALSE;
}

bool hasName(const std::vector<const char*>& list, const char* name) {
    for (const char* entry : list) {
        if (entry != nullptr && std::strcmp(entry, name) == 0) {
            return true;
        }
    }
    return false;
}

/// Per-frame-in-flight + per-image sync for the present loop.
struct PresentLoop {
    static constexpr u32 kFramesInFlight = 2;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd[kFramesInFlight]{};
    VkFence inFlight[kFramesInFlight]{};
    VkSemaphore acquired[kFramesInFlight]{};
    std::vector<VkSemaphore> renderDone; // one per swapchain image
    u32 frame = 0;

    bool init(VkDevice dev, VkQueue q, u32 family) {
        device = dev;
        queue = q;
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = kFramesInFlight;
        if (vkAllocateCommandBuffers(device, &alloc, cmd) != VK_SUCCESS) {
            return false;
        }
        for (u32 i = 0; i < kFramesInFlight; ++i) {
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VkSemaphoreCreateInfo semInfo{};
            semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateFence(device, &fenceInfo, nullptr, &inFlight[i]) != VK_SUCCESS ||
                vkCreateSemaphore(device, &semInfo, nullptr, &acquired[i]) != VK_SUCCESS) {
                return false;
            }
        }
        return true;
    }

    void resizeImageSemaphores(size_t imageCount) {
        for (VkSemaphore s : renderDone) {
            vkDestroySemaphore(device, s, nullptr);
        }
        renderDone.assign(imageCount, VK_NULL_HANDLE);
        for (VkSemaphore& s : renderDone) {
            VkSemaphoreCreateInfo semInfo{};
            semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            vkCreateSemaphore(device, &semInfo, nullptr, &s);
        }
    }

    /// Acquire -> clear via the swapchain's present render pass -> submit -> present.
    bool drawFrame(fuse::renderer::VulkanSwapchain& swapchain, float r, float g, float b) {
        const u32 slot = frame % kFramesInFlight;
        vkWaitForFences(device, 1, &inFlight[slot], VK_TRUE, UINT64_MAX);

        const u32 imageIndex = swapchain.acquireNextImage(acquired[slot]);
        if (fuse::renderer::isEmptyAcquireResult(imageIndex) || imageIndex >= renderDone.size()) {
            std::fprintf(stderr, "acquireNextImage failed (frame %u)\n", frame);
            return false;
        }
        vkResetFences(device, 1, &inFlight[slot]);

        VkCommandBuffer cb = cmd[slot];
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &begin);

        const fuse::renderer::SwapchainInfo& info = swapchain.info();
        VkRenderPassBeginInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass = static_cast<VkRenderPass>(swapchain.presentRenderPass());
        rp.framebuffer = static_cast<VkFramebuffer>(swapchain.framebufferForImage(imageIndex));
        rp.renderArea.extent = {info.width, info.height};
        vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear.colorAttachment = 0;
        clear.clearValue.color = {{r, g, b, 1.f}};
        VkClearRect rect{};
        rect.rect.extent = {info.width, info.height};
        rect.layerCount = 1;
        vkCmdClearAttachments(cb, 1, &clear, 1, &rect);
        vkCmdEndRenderPass(cb);
        vkEndCommandBuffer(cb);

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired[slot];
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderDone[imageIndex];
        if (vkQueueSubmit(queue, 1, &submit, inFlight[slot]) != VK_SUCCESS) {
            return false;
        }

        bool presented = false;
        if (fuse::renderer::productionPresentAllowed()) {
            presented = swapchain.present(renderDone[imageIndex], imageIndex);
        } else {
            // Track B production present is compile-time locked in CI builds; present the real
            // VkSwapchainKHR directly so the gate still exercises vkQueuePresentKHR.
            VkSwapchainKHR handle = static_cast<VkSwapchainKHR>(swapchain.nativeHandle());
            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &renderDone[imageIndex];
            present.swapchainCount = 1;
            present.pSwapchains = &handle;
            present.pImageIndices = &imageIndex;
            const VkResult result = vkQueuePresentKHR(queue, &present);
            presented = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
        }
        ++frame;
        return presented;
    }

    void destroy() {
        if (device == VK_NULL_HANDLE) {
            return;
        }
        vkDeviceWaitIdle(device);
        for (VkSemaphore s : renderDone) {
            vkDestroySemaphore(device, s, nullptr);
        }
        renderDone.clear();
        for (u32 i = 0; i < kFramesInFlight; ++i) {
            vkDestroyFence(device, inFlight[i], nullptr);
            vkDestroySemaphore(device, acquired[i], nullptr);
        }
        vkDestroyCommandPool(device, pool, nullptr);
        device = VK_NULL_HANDLE;
    }
};

int runGates() {
    if (!fuse::platform::windowWsiAvailable()) {
        std::printf("SKIP: no X display reachable\n");
        return kSkip;
    }

    fuse::platform::WindowDesc windowDesc{};
    windowDesc.title = "FUSE B2 X11 Swapchain Gate";
    windowDesc.width = 1920;
    windowDesc.height = 1080;
    windowDesc.createNative = true;
    fuse::platform::Window window(windowDesc);
    expectTrue(window.nativeHandle().value != nullptr, "X11 Window 1920x1080 opened");
    if (window.nativeHandle().value == nullptr) {
        return EXIT_FAILURE;
    }

    std::vector<const char*> wsiExtensions;
    fuse::platform::requiredVulkanInstanceExtensions(wsiExtensions);
    expectTrue(hasName(wsiExtensions, "VK_KHR_surface") && hasName(wsiExtensions, "VK_KHR_xlib_surface"),
               "X11 WSI requests VK_KHR_surface + VK_KHR_xlib_surface");

    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "fuse_b2_x11_swapchain_gates";
    instanceDesc.enableValidation = true;
    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    if (instance == nullptr || !instance->isValid()) {
        std::printf("SKIP: no Vulkan ICD (%s)\n", instance ? instance->info().message.c_str() : "");
        return kSkip;
    }
    const auto& layers = instance->info().enabledLayers;
    if (!hasName(layers, "VK_LAYER_KHRONOS_validation")) {
        std::printf("SKIP: VK_LAYER_KHRONOS_validation not installed\n");
        return kSkip;
    }
    expectTrue(instance->info().instanceHasExtension("VK_KHR_xlib_surface"),
               "instance enabled VK_KHR_xlib_surface");
    expectTrue(instance->info().instanceHasExtension("VK_EXT_debug_utils"),
               "instance enabled VK_EXT_debug_utils");

    auto vkInstance = static_cast<VkInstance>(instance->nativeHandle());
    auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkCreateDebugUtilsMessengerEXT"));
    auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkDestroyDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (createMessenger != nullptr) {
        VkDebugUtilsMessengerCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = countingCallback;
        createMessenger(vkInstance, &info, nullptr, &messenger);
    }
    expectTrue(messenger != VK_NULL_HANDLE, "validation counting messenger installed");

    // --- surface ---------------------------------------------------------------------------
    void* surface = nullptr;
    const bool surfaceOk = fuse::platform::createVulkanSurface(vkInstance, window, &surface);
    expectTrue(surfaceOk && surface != nullptr, "createVulkanSurface returns a VkSurfaceKHR (xlib)");
    if (!surfaceOk || surface == nullptr) {
        return EXIT_FAILURE;
    }

    fuse::renderer::VulkanDeviceDesc deviceDesc{};
    deviceDesc.requirePresentation = true;
    deviceDesc.presentSurface = surface;
    auto device = fuse::renderer::VulkanDevice::create(*instance, deviceDesc);
    expectTrue(device != nullptr && device->isValid(), "present-capable VulkanDevice created");
    if (device == nullptr || !device->isValid()) {
        return EXIT_FAILURE;
    }
    std::printf("  device: %s\n", device->info().deviceName.c_str());

    auto vkPhysical = static_cast<VkPhysicalDevice>(device->nativePhysicalDevice());
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(vkPhysical, device->queues().graphicsFamily,
                                         static_cast<VkSurfaceKHR>(surface), &supported);
    expectTrue(supported == VK_TRUE, "graphics queue family supports present to the surface");
    VkSurfaceCapabilitiesKHR caps{};
    const VkResult capsResult = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        vkPhysical, static_cast<VkSurfaceKHR>(surface), &caps);
    expectTrue(capsResult == VK_SUCCESS, "surface capabilities query succeeds (surface is valid)");
    std::printf("  surface caps: currentExtent %ux%u, images min %u max %u\n", caps.currentExtent.width,
                caps.currentExtent.height, caps.minImageCount, caps.maxImageCount);
    expectTrue(caps.currentExtent.width == 1920u && caps.currentExtent.height == 1080u,
               "surface currentExtent matches the 1920x1080 X11 window");

    // --- swapchain 1920x1080 x3 ------------------------------------------------------------
    fuse::renderer::SwapchainDesc swapDesc{};
    swapDesc.surface.kind = fuse::renderer::SurfaceKind::External;
    swapDesc.surface.nativeSurface = surface;
    swapDesc.width = 1920;
    swapDesc.height = 1080;
    swapDesc.imageCount = 3;
    swapDesc.vsyncMode = fuse::renderer::VsyncMode::Fifo;
    auto swapchain = fuse::renderer::VulkanSwapchain::create(*device, swapDesc);
    expectTrue(swapchain != nullptr && swapchain->isReady() && !swapchain->isHeadless(),
               "real VkSwapchainKHR created on the X11 surface");
    if (swapchain == nullptr || !swapchain->isReady()) {
        std::fprintf(stderr, "swapchain: %s\n", swapchain ? swapchain->info().message.c_str() : "null");
        return EXIT_FAILURE;
    }
    expectTrue(swapchain->nativeHandle() != nullptr, "VkSwapchainKHR handle is non-null");
    const size_t initialImages = swapchain->images().size();
    std::printf("  swapchain: %ux%u, %zu images (requested 3, surface min %u), format %u\n",
                swapchain->info().width, swapchain->info().height, initialImages, caps.minImageCount,
                swapchain->info().format);
    expectTrue(swapchain->info().width == 1920u && swapchain->info().height == 1080u,
               "swapchain extent is 1920x1080");
    if (caps.minImageCount <= 3u && (caps.maxImageCount == 0u || caps.maxImageCount >= 3u)) {
        expectTrue(initialImages == 3u, "swapchain is triple-buffered (3 images)");
    } else {
        std::printf("  note: surface cannot do exactly 3 images; got %zu\n", initialImages);
        expectTrue(initialImages >= caps.minImageCount, "swapchain honours surface minImageCount");
    }

    PresentLoop loop;
    expectTrue(loop.init(static_cast<VkDevice>(device->nativeHandle()),
                         static_cast<VkQueue>(device->queues().graphics),
                         device->queues().graphicsFamily),
               "present loop sync objects created");
    loop.resizeImageSemaphores(initialImages);

    u32 presented = 0;
    for (u32 i = 0; i < 10u; ++i) {
        if (loop.drawFrame(*swapchain, 0.1f * static_cast<float>(i), 0.2f, 0.4f)) {
            ++presented;
        }
    }
    expectTrue(presented == 10u, "10 frames presented at 1920x1080");
    std::printf("  presented %u/10 frames at 1920x1080\n", presented);

    // --- resize to 1280x720 through the window + event pump --------------------------------
    fuse::platform::EventPump pump;
    pump.processOsEvents();
    std::vector<fuse::platform::PlatformEvent> drained;
    pump.drainEvents(drained);
    drained.clear();

    window.resize(1280, 720, &pump);
    pump.processOsEvents();
    pump.drainEvents(drained);
    u32 resizeW = 0;
    u32 resizeH = 0;
    for (const auto& e : drained) {
        if (e.type == fuse::platform::PlatformEventType::WindowResized && e.window == &window) {
            resizeW = e.width;
            resizeH = e.height;
        }
    }
    expectTrue(resizeW == 1280u && resizeH == 720u, "WindowResized 1280x720 dispatched");

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vkPhysical, static_cast<VkSurfaceKHR>(surface), &caps);
    expectTrue(caps.currentExtent.width == 1280u && caps.currentExtent.height == 720u,
               "surface currentExtent follows the X11 resize (1280x720)");

    device->waitIdle();
    const bool rebuilt = swapchain->rebuild(*device, resizeW, resizeH);
    expectTrue(rebuilt && swapchain->isReady(), "VulkanSwapchain::rebuild succeeds after resize");
    expectTrue(swapchain->info().width == 1280u && swapchain->info().height == 720u,
               "rebuilt swapchain extent is 1280x720");
    expectTrue(swapchain->info().recreateCount == 1u, "recreateCount == 1");
    std::printf("  rebuilt swapchain: %ux%u, %zu images\n", swapchain->info().width,
                swapchain->info().height, swapchain->images().size());
    loop.resizeImageSemaphores(swapchain->images().size());

    presented = 0;
    for (u32 i = 0; i < 10u; ++i) {
        if (loop.drawFrame(*swapchain, 0.4f, 0.1f * static_cast<float>(i), 0.2f)) {
            ++presented;
        }
    }
    expectTrue(presented == 10u, "10 frames presented at 1280x720 after recreate");
    std::printf("  presented %u/10 frames at 1280x720\n", presented);

    // --- teardown (validation keeps listening through destruction) --------------------------
    loop.destroy();
    swapchain.reset();
    vkDestroySurfaceKHR(vkInstance, static_cast<VkSurfaceKHR>(surface), nullptr);
    device.reset();
    if (messenger != VK_NULL_HANDLE && destroyMessenger != nullptr) {
        destroyMessenger(vkInstance, messenger, nullptr);
    }

    std::printf("  validation: %u error(s), %u warning(s)\n", g_validationErrors, g_validationWarnings);
    expectTrue(g_validationErrors == 0u, "zero VK_LAYER_KHRONOS_validation errors");
    return EXIT_SUCCESS;
}

#endif

} // namespace

int main() {
#if !defined(FUSE_VULKAN_BACKEND)
    std::printf("SKIP: Vulkan backend not compiled in\n");
    return kSkip;
#elif !defined(FUSE_PLATFORM_WINDOW_X11)
    std::printf("SKIP: X11 window backend not compiled in\n");
    return kSkip;
#else
    fuse::core::initialize();
    const int rc = runGates();
    fuse::core::shutdown();
    if (rc == kSkip) {
        return kSkip;
    }
    if (g_failures == 0 && rc == EXIT_SUCCESS) {
        std::printf("fuse_b2_x11_swapchain_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b2_x11_swapchain_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
#endif
}
