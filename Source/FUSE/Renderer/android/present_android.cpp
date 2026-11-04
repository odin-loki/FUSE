// fuse_runtime_android: the engine renderer (fuse_rhi) on an Android NativeActivity window.
// RhiContext renders the B2.11 frame (clear + fixture triangle) into its raster target; each frame is blitted into
// a VkSwapchainKHR on the activity's window (VK_KHR_android_surface, fuse/renderer/vk/surface_android.hpp) and
// presented. At the end the raster target is read back and checked (black background, white triangle covering
// 12.5 %). The gate fuse_runtime_present_android (App/android/android_gate.py --gate runtime) reads the state file
// it writes (files/fuse_runtime_state.txt).
//   adb shell am start -n org.fuse.runtime/android.app.NativeActivity [--es args "--frames=120"]
#include <fuse/app/native.hpp>
#include <fuse/core/init.hpp>
#include <fuse/core/track_b.hpp>
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/vk/raster_path.hpp>
#include <fuse/renderer/vk/surface_android.hpp>
#include <fuse/renderer/vk/swapchain.hpp>

#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {
using fuse::u32;
using fuse::u8;

struct State {
    std::string device, error, readback = "pending", surfaceFormat;
    u32 width = 0, height = 0, rendered = 0, presented = 0, swapImages = 0;
    double coverage = 0.0, initMs = 0.0, frameMs = 0.0;
    bool done = false;
};

void writeState(const State& s) {
    char buf[1024];
    std::snprintf(buf, sizeof buf,
                  "device=%s\nwindow=%ux%u\nswapchain_images=%u\nswapchain_format=%s\nframes_rendered=%u\n"
                  "frames_presented=%u\nreadback=%s\ncoverage=%.4f\ninit_ms=%.1f\nframe_ms=%.2f\nerror=%s\ndone=%d\n",
                  s.device.c_str(), s.width, s.height, s.swapImages, s.surfaceFormat.c_str(), s.rendered, s.presented,
                  s.readback.c_str(), s.coverage, s.initMs, s.frameMs, s.error.c_str(), s.done ? 1 : 0);
    if (std::FILE* f = std::fopen("fuse_runtime_state.tmp", "wb")) {
        std::fputs(buf, f);
        std::fclose(f);
        std::rename("fuse_runtime_state.tmp", "fuse_runtime_state.txt");
    }
    std::printf("%s", buf);
}

/// Drains the glue's looper (window / lifecycle commands); false once the activity is being destroyed.
bool pump(android_app* app, int timeoutMs) {
    int events = 0;
    android_poll_source* src = nullptr;
    for (int id = ALooper_pollOnce(timeoutMs, nullptr, &events, reinterpret_cast<void**>(&src)); id >= 0;
         id = ALooper_pollOnce(0, nullptr, &events, reinterpret_cast<void**>(&src))) {
        if (src) src->process(app, src);
        src = nullptr;
    }
    return !app->destroyRequested;
}

VkImageMemoryBarrier barrier(VkImage img, VkAccessFlags src, VkAccessFlags dst, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

/// Black background, white fixture triangle (apex (w/2, h/4), base y = 3h/4 from w/4 to 3w/4): the B2.11 checks.
std::string checkTriangle(const std::vector<u8>& rgba, u32 w, u32 h, double& coverage) {
    auto px = [&](u32 x, u32 y) { return &rgba[(std::size_t(y) * w + x) * 4u]; };
    auto white = [](const u8* p) { return p[0] == 255 && p[1] == 255 && p[2] == 255; };
    auto black = [](const u8* p) { return p[0] == 0 && p[1] == 0 && p[2] == 0; };
    if (rgba.size() != std::size_t(w) * h * 4u) return "size";
    if (!black(px(1, 1)) || !black(px(w - 2, 1)) || !black(px(1, h - 2)) || !black(px(w - 2, h - 2))) return "corners";
    if (!white(px(w / 2, h / 2)) || !white(px(w / 2, h * 3 / 10))) return "centre";
    if (!black(px(w * 3 / 10, h * 3 / 10)) || !white(px(w * 3 / 10, h * 7 / 10))) return "orientation";
    std::size_t on = 0;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) on += white(px(x, y));
    coverage = double(on) / (double(w) * h);
    return coverage > 0.115 && coverage < 0.135 ? "pass" : "coverage";
}
} // namespace

int main(int argc, char** argv) {
    auto* app = static_cast<android_app*>(fuse::app::NativePlatform::androidApp());
    u32 frames = 120;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--frames=", 9) == 0) frames = u32(std::atoi(argv[i] + 9));
    State st;
    std::remove("fuse_runtime_state.txt");
    if (!app) {
        st.error = "not started through android_main";
        st.done = true;
        writeState(st);
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200 && !app->window && pump(app, 50); ++i) {
    }
    ANativeWindow* window = app->window;
    if (!window) {
        st.error = "no window";
        st.done = true;
        writeState(st);
        return 1;
    }
    ANativeWindow_acquire(window);
    st.width = u32(ANativeWindow_getWidth(window));
    st.height = u32(ANativeWindow_getHeight(window));

    fuse::core::initialize();
    fuse::core::setTrackBHostFeature(fuse::core::TrackBHostFeature::AppPresent, true); // an app: vkQueuePresentKHR on
    int rc = 1;
    {
        fuse::renderer::RhiContext::Desc desc{};
        desc.bootstrap.instance.enableValidation = false;
        desc.bootstrap.createSwapchain = false;
        desc.enableCompositePass = false;
        desc.enableComputePipeline = false;
        desc.raster.width = st.width;
        desc.raster.height = st.height;
        desc.raster.vertexSpirvPath = "shaders/minimal.vert.spv"; // APK assets, extracted to the files dir
        desc.raster.fragmentSpirvPath = "shaders/minimal.frag.spv";
        auto ctx = fuse::renderer::RhiContext::create(desc);
        const fuse::renderer::RasterPath* raster = nullptr; // created by the first submitFrame
        fuse::renderer::VulkanBootstrap* boot = ctx ? &ctx->bootstrap() : nullptr;
        void* surface = nullptr;
        std::unique_ptr<fuse::renderer::VulkanSwapchain> swapchain;
        if (!boot || !boot->status().deviceReady) {
            st.error = boot ? "no Vulkan device: " + boot->status().message : "no RHI context";
        } else if (!fuse::renderer::createAndroidSurface(boot->instance()->nativeHandle(), window, &surface, &st.error)) {
            // st.error set
        } else {
            fuse::renderer::VulkanDevice& dev = *boot->device();
            st.device = dev.info().deviceName;
            VkBool32 canPresent = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(static_cast<VkPhysicalDevice>(dev.nativePhysicalDevice()),
                                                 dev.queues().graphicsFamily, reinterpret_cast<VkSurfaceKHR>(surface), &canPresent);
            fuse::renderer::SwapchainDesc sd{};
            sd.surface.kind = fuse::renderer::SurfaceKind::External;
            sd.surface.nativeSurface = surface;
            sd.width = st.width;
            sd.height = st.height;
            sd.imageCount = 3;
            sd.preferredFormat = VK_FORMAT_R8G8B8A8_UNORM; // Android surfaces offer RGBA
            if (canPresent) swapchain = fuse::renderer::VulkanSwapchain::create(dev, sd);
            if (!canPresent) st.error = "graphics queue cannot present to the window";
            else if (!swapchain || !swapchain->isReady() || swapchain->isHeadless())
                st.error = "swapchain: " + (swapchain ? swapchain->info().message : std::string("null"));
        }
        if (swapchain && swapchain->isReady() && !swapchain->isHeadless()) {
            fuse::renderer::VulkanDevice& dev = *boot->device();
            const auto device = static_cast<VkDevice>(dev.nativeHandle());
            const auto queue = static_cast<VkQueue>(dev.queues().graphics);
            st.swapImages = u32(swapchain->images().size());
            st.surfaceFormat = std::to_string(swapchain->info().format) + " " + std::to_string(swapchain->info().width) +
                               "x" + std::to_string(swapchain->info().height);
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pci.queueFamilyIndex = dev.queues().graphicsFamily;
            VkCommandPool pool = VK_NULL_HANDLE;
            vkCreateCommandPool(device, &pci, nullptr, &pool);
            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool = pool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            vkAllocateCommandBuffers(device, &cai, &cmd);
            VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore acquired = VK_NULL_HANDLE;
            vkCreateSemaphore(device, &semi, nullptr, &acquired);
            std::vector<VkSemaphore> done(st.swapImages, VK_NULL_HANDLE); // one per image: reuse waits on its present
            for (VkSemaphore& s : done) vkCreateSemaphore(device, &semi, nullptr, &s);
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkFence fence = VK_NULL_HANDLE;
            vkCreateFence(device, &fci, nullptr, &fence);
            st.initMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

            const auto tf = std::chrono::steady_clock::now();
            for (u32 f = 0; f < frames && pump(app, 0) && app->window == window; ++f) {
                fuse::renderer::RenderCommandList commands;
                commands.clear3D(0.f, 0.f, 0.f);
                commands.drawSprite2D(0.f, 0.f, 0.f, 255, 255, 255); // the fixture triangle
                if (!ctx->beginFrame(f) || !ctx->submitFrame(commands, f)) {
                    st.error = "RhiContext frame failed";
                    break;
                }
                raster = ctx->rasterPath();
                if (!raster || !raster->isReady()) {
                    st.error = "raster path not ready: " + (raster ? raster->lastStats().message : std::string("none"));
                    break;
                }
                ++st.rendered;
                vkQueueWaitIdle(queue); // the raster target is complete
                const auto colour = static_cast<VkImage>(raster->barrierImageHandle());
                const u32 index = swapchain->acquireNextImage(acquired);
                if (index == UINT32_MAX) continue;
                const auto sw = static_cast<VkImage>(swapchain->imageHandleForIndex(index));
                const auto keep = static_cast<VkImageLayout>(raster->colorImageLayout());
                vkResetCommandBuffer(cmd, 0);
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cmd, &bi);
                VkImageMemoryBarrier pre[2] = {
                    barrier(colour, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, keep,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                    barrier(sw, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)};
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                     0, nullptr, 2, pre);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.dstSubresource = blit.srcSubresource;
                blit.srcOffsets[1] = {int(st.width), int(st.height), 1};
                blit.dstOffsets[1] = {int(swapchain->info().width), int(swapchain->info().height), 1};
                vkCmdBlitImage(cmd, colour, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sw, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &blit, VK_FILTER_NEAREST);
                VkImageMemoryBarrier post[2] = {
                    barrier(colour, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, keep == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_GENERAL : keep),
                    barrier(sw, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)};
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                     nullptr, 2, post);
                vkEndCommandBuffer(cmd);
                const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                si.waitSemaphoreCount = 1;
                si.pWaitSemaphores = &acquired;
                si.pWaitDstStageMask = &waitStage;
                si.commandBufferCount = 1;
                si.pCommandBuffers = &cmd;
                si.signalSemaphoreCount = 1;
                si.pSignalSemaphores = &done[index];
                if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) {
                    st.error = "blit submit failed";
                    break;
                }
                vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
                vkResetFences(device, 1, &fence);
                if (!swapchain->present(done[index], index)) {
                    st.error = "vkQueuePresentKHR failed";
                    break;
                }
                ++st.presented;
                if (f % 30 == 0) writeState(st);
            }
            vkDeviceWaitIdle(device);
            st.frameMs = st.rendered ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tf).count() / st.rendered : 0.0;
            std::vector<u8> rgba;
            st.readback = raster && raster->readbackColor(rgba) ? checkTriangle(rgba, st.width, st.height, st.coverage) : "failed";
            rc = st.readback == "pass" && st.presented > 0 && st.error.empty() ? 0 : 1;
            vkDestroyFence(device, fence, nullptr);
            for (VkSemaphore s : done) vkDestroySemaphore(device, s, nullptr);
            vkDestroySemaphore(device, acquired, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
        }
        swapchain.reset();
        if (surface) fuse::renderer::destroyAndroidSurface(boot->instance()->nativeHandle(), surface);
    }
    fuse::core::shutdown();
    ANativeWindow_release(window);
    st.done = true;
    writeState(st);
    // stay up until the gate (or the user) closes the activity
    while (pump(app, 100)) {
    }
    return rc;
}
