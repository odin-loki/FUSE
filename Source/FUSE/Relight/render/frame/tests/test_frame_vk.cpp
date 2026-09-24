// FUSE Relight RL-4.1: the composite logic on a real Vulkan device (Lavapipe), with the Khronos validation layer
// and synchronization validation when available (native Linux; under Wine the layer is absent: --no-validation).
//
// SimHost plays DXVK: it owns a "back buffer", game textures and a command stream on the device's graphics queue,
// and implements tap::IFrameHost exactly as the DXVK dispatcher does (signal acquire + flush + submitted, FUSE
// submits under the queue lock, wait release + copy FUSE's image over the back buffer, passthrough texture
// swap). The FUSE side is the production code: FrameOrchestrator, FrameGpu (RG v2-planned barriers, own dispatch),
// BindlessImageRegistry on the renderer's GPU bindless heap (WP-0.4, real descriptors and views).
//
// Per frame the host draws a "scene" (coloured bands), samples a game texture into the back buffer (a copy
// from the texture, or from its FUSE twin when swapped), then FUSE injects, then the host draws a "HUD" rect
// and reads the back buffer back.
//   passthrough  every frame bit-identical to the reference (no FUSE), including after a back-buffer resize
//                and a texture update (the twin follows the texture);
//   solid        FUSE's colour everywhere but the HUD rect, which keeps the HUD colour (composite below the UI);
//   bindless     external / owned registrations write descriptors; released slots go stale at once and are
//                reclaimed once their acquire value completed; views are destroyed then;
//   validation   0 messages (errors, warnings, synchronization hazards).
// Exit: 0 pass, 1 fail, 77 skip (no Vulkan device, or --validation required but no layer).
#include <fuse/relight/render/frame/bindless_images.hpp>
#include <fuse/relight/render/frame/bindless_renderer_heap.hpp>
#include <fuse/relight/render/frame/frame_orchestrator.hpp>
#include <fuse/relight/render/frame/vk_dispatch.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rf = fuse::relight::render::frame;
namespace rt = fuse::relight::tap;
namespace rr = fuse::renderer;

namespace {

constexpr int kSkip = 77;
int g_failures = 0;
int g_validation = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) &&
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT))) {
        ++g_validation;
        std::printf("VALIDATION: %s\n  %s\n", data && data->pMessageIdName ? data->pMessageIdName : "(no id)",
                    data && data->pMessage ? data->pMessage : "");
    }
    return VK_FALSE;
}

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

bool layerAvailable(const char* name) {
    std::uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    for (const VkLayerProperties& l : layers) {
        if (std::strcmp(l.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

std::uint64_t u64(VkImage i) { return rf::vkValue(i); }

/// A zero-initialised Vulkan struct with sType (and pNext) set.
template <typename T>
T vks(VkStructureType type, const void* next = nullptr) {
    T t{};
    t.sType = type;
    t.pNext = const_cast<void*>(next);
    return t;
}

constexpr VkFormat kFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr std::uint32_t kHudColor = 0x30e060u;

// ---- the simulated DXVK ----------------------------------------------------------------------------------------

struct HostImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint32_t width = 0, height = 0;
    VkImageUsageFlags usage = 0;
    bool owned = true;       ///< the host created it
    bool initialized = true; ///< false: FUSE twin, first host use discards (UNDEFINED)
};

class SimHost final : public rt::IFrameHost {
public:
    SimHost(VkInstance instance, VkPhysicalDevice pd, VkDevice device, VkQueue queue, std::uint32_t family)
        : m_instance(instance), m_pd(pd), m_device(device), m_queue(queue), m_family(family) {
        auto pool = vks<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = family;
        vkCreateCommandPool(device, &pool, nullptr, &m_pool);
        auto type = vks<VkSemaphoreTypeCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        auto sem = vks<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type);
        vkCreateSemaphore(device, &sem, nullptr, &m_acquireSem);
        vkCreateSemaphore(device, &sem, nullptr, &m_releaseSem);
        auto buf = vks<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        buf.size = 512u * 512u * 4u;
        buf.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCreateBuffer(device, &buf, nullptr, &m_readback);
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, m_readback, &req);
        auto mem = vks<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        mem.allocationSize = req.size;
        mem.memoryTypeIndex = memoryType(req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &mem, nullptr, &m_readbackMemory);
        vkBindBufferMemory(device, m_readback, m_readbackMemory, 0);
        vkMapMemory(device, m_readbackMemory, 0, VK_WHOLE_SIZE, 0, &m_mapped);
        m_hud = createImage(16, 8, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }

    ~SimHost() override {
        vkDeviceWaitIdle(m_device);
        for (auto& [id, t] : m_textures) {
            destroyImage(t.image);
        }
        destroyImage(m_backBuffer);
        destroyImage(m_hud);
        vkUnmapMemory(m_device, m_readbackMemory);
        vkDestroyBuffer(m_device, m_readback, nullptr);
        vkFreeMemory(m_device, m_readbackMemory, nullptr);
        for (VkFence f : m_fences) {
            vkDestroyFence(m_device, f, nullptr);
        }
        vkDestroySemaphore(m_device, m_acquireSem, nullptr);
        vkDestroySemaphore(m_device, m_releaseSem, nullptr);
        vkDestroyCommandPool(m_device, m_pool, nullptr);
    }

    // ---- the host's own frame ----------------------------------------------------------------------------
    void resize(std::uint32_t w, std::uint32_t h) {
        vkDeviceWaitIdle(m_device);
        destroyImage(m_backBuffer);
        m_backBuffer = createImage(w, h, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    rt::ResourceId createTexture(std::uint32_t w, std::uint32_t h, std::uint32_t rgb) {
        const rt::ResourceId id = m_nextTexture++;
        Texture& t = m_textures[id];
        t.image = createImage(w, h,
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        updateTexture(id, rgb);
        return id;
    }
    rt::TextureDesc textureDesc(rt::ResourceId id) const {
        const Texture& t = m_textures.at(id);
        rt::TextureDesc d;
        d.id = id;
        d.type = 3; // D3DRTYPE_TEXTURE
        d.width = t.image.width;
        d.height = t.image.height;
        d.depth = 1;
        d.mipLevels = 1;
        d.arraySize = 1;
        d.format = 21; // D3DFMT_A8R8G8B8
        d.pool = 1;    // D3DPOOL_MANAGED
        d.vkImage = u64(t.image.image);
        return d;
    }
    /// The application uploads new content (the dispatcher's contentChanged).
    void updateTexture(rt::ResourceId id, std::uint32_t rgb) {
        Texture& t = m_textures.at(id);
        begin();
        fullBarrier();
        clear(t.image.image, rgb);
        ++t.version;
        endSubmit(false);
    }
    void destroyTexture(rt::ResourceId id) {
        // DXVK keeps the image alive until its work completed; here the frame's submissions are waited.
        vkDeviceWaitIdle(m_device);
        destroyImage(m_textures.at(id).image);
        m_textures.erase(id);
        m_swaps.erase(id);
    }

    void beginFrame(std::uint32_t frame) {
        begin();
        fullBarrier();
        // "Scene": four horizontal bands, their colours changing per frame.
        const std::uint32_t colors[4] = {0x102030u + frame * 0x010203u, 0x804020u, 0x20a040u + frame, 0xc0c0c0u};
        for (std::uint32_t b = 0; b < 4; ++b) {
            clearRect(m_backBuffer, colors[b], 0, b * m_backBuffer.height / 4, m_backBuffer.width, m_backBuffer.height / 4);
        }
    }
    /// A draw sampling `texture` into a rect of the back buffer (the swap binds the twin instead).
    void drawTextured(rt::ResourceId id, std::int32_t x, std::int32_t y) {
        Texture& t = m_textures.at(id);
        HostImage* src = &t.image;
        auto sw = m_swaps.find(id);
        if (sw != m_swaps.end()) {
            HostImage& twin = sw->second.twin;
            if (sw->second.copied != t.version) { // passthrough: DXVK copies the texture into the twin
                fullBarrier(&twin);
                copy(t.image, twin, 0, 0);
                sw->second.copied = t.version;
            }
            src = &twin;
        }
        fullBarrier();
        copy(*src, m_backBuffer, x, y);
    }
    void drawHud() {
        fullBarrier();
        copy(m_hud, m_backBuffer, 8, static_cast<std::int32_t>(m_backBuffer.height) - 16);
    }
    std::vector<std::uint8_t> present() {
        fullBarrier();
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {m_backBuffer.width, m_backBuffer.height, 1};
        vkCmdCopyImageToBuffer(m_cmd, m_backBuffer.image, VK_IMAGE_LAYOUT_GENERAL, m_readback, 1, &region);
        auto host = vks<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
        host.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        auto dep = vks<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &host;
        vkCmdPipelineBarrier2(m_cmd, &dep);
        endSubmit(true);
        const auto* p = static_cast<const std::uint8_t*>(m_mapped);
        return std::vector<std::uint8_t>(p, p + m_backBuffer.width * m_backBuffer.height * 4u);
    }
    std::uint32_t width() const { return m_backBuffer.width; }
    std::uint32_t height() const { return m_backBuffer.height; }
    std::uint32_t pendingWaits() const { return static_cast<std::uint32_t>(m_waits.size()); }

    // ---- IFrameHost --------------------------------------------------------------------------------------
    std::uint64_t getInstanceProcAddr() const override {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(vkGetInstanceProcAddr));
    }
    rt::VulkanDevice vulkan() const override {
        rt::VulkanDevice v;
        v.instance = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(m_instance));
        v.physicalDevice = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(m_pd));
        v.device = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(m_device));
        v.queue = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(m_queue));
        v.queueFamily = m_family;
        return v;
    }
    std::uint64_t acquireSemaphore() override { return rf::vkValue(m_acquireSem); }
    std::uint64_t releaseSemaphore() override { return rf::vkValue(m_releaseSem); }
    bool backBufferInfo(rt::HostImageInfo& out) const override {
        out = info(m_backBuffer);
        return true;
    }
    bool textureInfo(rt::ResourceId texture, rt::HostImageInfo& out) const override {
        auto it = m_textures.find(texture);
        if (it == m_textures.end()) {
            return false;
        }
        out = info(it->second.image);
        return true;
    }
    rt::HostImageHandle importImage(const rt::FuseImage& image) override {
        HostImage h;
        h.image = rf::vkHandle<VkImage>(image.vkImage);
        h.width = image.info.width;
        h.height = image.info.height;
        h.usage = image.info.usage;
        h.owned = false;
        const rt::HostImageHandle handle = m_nextImport++;
        m_imports[handle] = h;
        return handle;
    }
    void releaseImage(rt::HostImageHandle image) override { m_imports.erase(image); }
    bool copyBackBuffer(rt::HostImageHandle dst) override {
        auto it = m_imports.find(dst);
        if (it == m_imports.end()) {
            return false;
        }
        fullBarrier();
        copy(m_backBuffer, it->second, 0, 0);
        return true;
    }
    bool flushAndSignal(std::uint64_t value) override {
        fullBarrier(); // DXVK leaves shared images in their default layout (GENERAL) with their writes available
        m_signal = value;
        endSubmit(false);
        begin();
        return true;
    }
    bool composite(rt::HostImageHandle src, std::uint64_t value) override {
        auto it = m_imports.find(src);
        if (it == m_imports.end()) {
            return false;
        }
        m_waits.push_back(value); // waited by the first submission of the current command stream
        fullBarrier();
        copy(it->second, m_backBuffer, 0, 0);
        return true;
    }
    bool setTextureSwap(rt::ResourceId texture, const rt::FuseImage* image) override {
        if (!m_textures.count(texture)) {
            return false;
        }
        if (!image) {
            m_swaps.erase(texture);
            return true;
        }
        Swap s;
        s.twin.image = rf::vkHandle<VkImage>(image->vkImage);
        s.twin.width = image->info.width;
        s.twin.height = image->info.height;
        s.twin.owned = false;
        s.twin.initialized = false;
        m_swaps[texture] = s;
        return true;
    }
    void lockQueue() override { m_queueLock.lock(); }
    void unlockQueue() override { m_queueLock.unlock(); }
    bool waitIdle() override {
        std::lock_guard<std::recursive_mutex> lock(m_queueLock);
        vkQueueWaitIdle(m_queue);
        return true;
    }

private:
    struct Texture {
        HostImage image;
        std::uint64_t version = 0;
    };
    struct Swap {
        HostImage twin;
        std::uint64_t copied = ~0ull;
    };

    std::uint32_t memoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
        VkPhysicalDeviceMemoryProperties p{};
        vkGetPhysicalDeviceMemoryProperties(m_pd, &p);
        for (std::uint32_t i = 0; i < p.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) {
                return i;
            }
        }
        return 0;
    }
    rt::HostImageInfo info(const HostImage& h) const {
        rt::HostImageInfo i;
        i.vkImage = u64(h.image);
        i.imageType = VK_IMAGE_TYPE_2D;
        i.format = kFormat;
        i.usage = h.usage;
        i.width = h.width;
        i.height = h.height;
        i.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        return i;
    }
    HostImage createImage(std::uint32_t w, std::uint32_t h, VkImageUsageFlags usage) {
        HostImage img;
        img.width = w;
        img.height = h;
        img.usage = usage;
        auto ci = vks<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = kFormat;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        vkCreateImage(m_device, &ci, nullptr, &img.image);
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, img.image, &req);
        auto mem = vks<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        mem.allocationSize = req.size;
        mem.memoryTypeIndex = memoryType(req.memoryTypeBits, 0);
        vkAllocateMemory(m_device, &mem, nullptr, &img.memory);
        vkBindImageMemory(m_device, img.image, img.memory, 0);
        img.initialized = false;
        begin();
        fullBarrier(&img); // UNDEFINED -> GENERAL, the host's default layout
        if (img.image == m_hud.image || (usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0) {
            clear(img.image, kHudColor);
        }
        endSubmit(false);
        return img;
    }
    void destroyImage(HostImage& img) {
        if (img.owned && img.image != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, img.image, nullptr);
            vkFreeMemory(m_device, img.memory, nullptr);
        }
        img = HostImage{};
    }

    void begin() {
        if (m_cmd != VK_NULL_HANDLE) {
            return;
        }
        auto a = vks<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        a.commandPool = m_pool;
        a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        a.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_device, &a, &m_cmd);
        auto b = vks<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m_cmd, &b);
    }
    /// Submits the current command stream: waits for the composite's release values, signals a pending
    /// acquire value. `wait`: blocks until done (and recycles every finished command buffer).
    void endSubmit(bool wait) {
        vkEndCommandBuffer(m_cmd);
        std::vector<VkSemaphoreSubmitInfo> waits;
        for (std::uint64_t v : m_waits) {
            auto w = vks<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
            w.semaphore = m_releaseSem;
            w.value = v;
            w.stageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT; // as DXVK's waitFence (== ALL_COMMANDS here)
            waits.push_back(w);
        }
        auto signal = vks<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
        signal.semaphore = m_acquireSem;
        signal.value = m_signal;
        signal.stageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT; // as DXVK's signalFence (== ALL_COMMANDS)
        auto cb = vks<VkCommandBufferSubmitInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
        cb.commandBuffer = m_cmd;
        auto s = vks<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
        s.waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size());
        s.pWaitSemaphoreInfos = waits.data();
        s.commandBufferInfoCount = 1;
        s.pCommandBufferInfos = &cb;
        s.signalSemaphoreInfoCount = m_signal ? 1u : 0u;
        s.pSignalSemaphoreInfos = &signal;
        auto fi = vks<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        VkFence fence = VK_NULL_HANDLE;
        vkCreateFence(m_device, &fi, nullptr, &fence);
        {
            std::lock_guard<std::recursive_mutex> lock(m_queueLock);
            vkQueueSubmit2(m_queue, 1, &s, fence);
        }
        m_inFlight.push_back({m_cmd, fence});
        m_cmd = VK_NULL_HANDLE;
        m_waits.clear();
        m_signal = 0;
        if (wait) {
            for (auto& f : m_inFlight) {
                vkWaitForFences(m_device, 1, &f.second, VK_TRUE, UINT64_MAX);
                vkFreeCommandBuffers(m_device, m_pool, 1, &f.first);
                m_fences.push_back(f.second);
            }
            m_inFlight.clear();
        }
    }
    /// Everything before -> everything after (the host is conservative; DXVK tracks per resource). With `init`,
    /// also UNDEFINED -> GENERAL for an image not yet initialised.
    void fullBarrier(HostImage* init = nullptr) {
        auto m = vks<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
        m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        m.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        m.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        auto ib = vks<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
        auto dep = vks<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &m;
        if (init && !init->initialized) {
            ib.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.image = init->image;
            ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &ib;
            init->initialized = true;
        }
        vkCmdPipelineBarrier2(m_cmd, &dep);
    }
    static VkClearColorValue color(std::uint32_t rgb) {
        VkClearColorValue c{};
        c.float32[0] = static_cast<float>((rgb >> 16) & 0xffu) / 255.f;
        c.float32[1] = static_cast<float>((rgb >> 8) & 0xffu) / 255.f;
        c.float32[2] = static_cast<float>(rgb & 0xffu) / 255.f;
        c.float32[3] = 1.f;
        return c;
    }
    void clear(VkImage image, std::uint32_t rgb) {
        const VkClearColorValue c = color(rgb);
        const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(m_cmd, image, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &r);
    }
    /// A rect "draw": the solid colour through a scratch copy of the HUD image's size is overkill; bands are
    /// drawn by clearing the texture-sized tile image and copying it (keeps the host transfer-only).
    void clearRect(HostImage& dst, std::uint32_t rgb, std::uint32_t x, std::uint32_t y, std::uint32_t w,
                   std::uint32_t h) {
        if (m_tile.image == VK_NULL_HANDLE || m_tile.width < w || m_tile.height < h) {
            m_tileGarbage.push_back(m_tile);
            m_tile = HostImage{};
            const std::uint32_t tw = std::max(w, 512u), th = std::max(h, 512u);
            // created in its own submission (the current stream stays open)
            VkCommandBuffer saved = m_cmd;
            m_cmd = VK_NULL_HANDLE;
            m_tile = createImage(tw, th, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                             VK_IMAGE_USAGE_SAMPLED_BIT);
            m_cmd = saved;
        }
        fullBarrier();
        clear(m_tile.image, rgb);
        fullBarrier();
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.dstOffset = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 0};
        region.extent = {w, h, 1};
        vkCmdCopyImage(m_cmd, m_tile.image, VK_IMAGE_LAYOUT_GENERAL, dst.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    }
    void copy(const HostImage& src, HostImage& dst, std::int32_t x, std::int32_t y) {
        if (!dst.initialized) {
            fullBarrier(&dst);
        }
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.dstOffset = {x, y, 0};
        region.extent = {std::min(src.width, dst.width - static_cast<std::uint32_t>(x)),
                         std::min(src.height, dst.height - static_cast<std::uint32_t>(y)), 1};
        vkCmdCopyImage(m_cmd, src.image, VK_IMAGE_LAYOUT_GENERAL, dst.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    }

public:
    void releaseScratch() {
        vkDeviceWaitIdle(m_device);
        destroyImage(m_tile);
        for (HostImage& t : m_tileGarbage) {
            destroyImage(t);
        }
        m_tileGarbage.clear();
    }

private:
    VkInstance m_instance;
    VkPhysicalDevice m_pd;
    VkDevice m_device;
    VkQueue m_queue;
    std::uint32_t m_family;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    std::vector<std::pair<VkCommandBuffer, VkFence>> m_inFlight;
    std::vector<VkFence> m_fences;
    VkSemaphore m_acquireSem = VK_NULL_HANDLE, m_releaseSem = VK_NULL_HANDLE;
    std::vector<std::uint64_t> m_waits;
    std::uint64_t m_signal = 0;
    std::recursive_mutex m_queueLock;
    VkBuffer m_readback = VK_NULL_HANDLE;
    VkDeviceMemory m_readbackMemory = VK_NULL_HANDLE;
    void* m_mapped = nullptr;
    HostImage m_backBuffer, m_hud, m_tile;
    std::vector<HostImage> m_tileGarbage;
    std::unordered_map<rt::ResourceId, Texture> m_textures;
    std::unordered_map<rt::ResourceId, Swap> m_swaps;
    std::unordered_map<rt::HostImageHandle, HostImage> m_imports;
    rt::ResourceId m_nextTexture = 1;
    rt::HostImageHandle m_nextImport = 1;
};

/// Real sampled views for the bindless registry.
class ViewFactory final : public rf::IImageViewFactory {
public:
    explicit ViewFactory(VkDevice device) : m_device(device) {}
    std::uint64_t createView(const rf::ExternalImageDesc& d) override {
        auto ci = vks<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        ci.image = rf::vkHandle<VkImage>(d.vkImage);
        ci.viewType = static_cast<VkImageViewType>(d.viewType);
        ci.format = static_cast<VkFormat>(d.vkFormat);
        ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, d.mipLevels, 0, d.arrayLayers};
        VkImageView v = VK_NULL_HANDLE;
        if (vkCreateImageView(m_device, &ci, nullptr, &v) != VK_SUCCESS) {
            return 0;
        }
        ++live;
        ++created;
        return rf::vkValue(v);
    }
    void destroyView(std::uint64_t view) override {
        vkDestroyImageView(m_device, rf::vkHandle<VkImageView>(view), nullptr);
        --live;
    }
    int live = 0;
    std::uint32_t created = 0;

private:
    VkDevice m_device;
};

struct Pixel {
    std::uint8_t b, g, r, a;
};
Pixel pixelAt(const std::vector<std::uint8_t>& img, std::uint32_t width, std::uint32_t x, std::uint32_t y) {
    const std::size_t o = (static_cast<std::size_t>(y) * width + x) * 4u;
    return Pixel{img[o], img[o + 1], img[o + 2], img[o + 3]};
}
bool pixelIs(const Pixel& p, std::uint32_t rgb) {
    return p.r == ((rgb >> 16) & 0xffu) && p.g == ((rgb >> 8) & 0xffu) && p.b == (rgb & 0xffu);
}

/// One run of `frames` frames; `orchestrator` null = the reference (tap off). Resizes at frame 2, updates the
/// texture at frame 3, destroys a second texture at frame 4.
std::vector<std::vector<std::uint8_t>> runFrames(SimHost& host, rf::FrameOrchestrator* orchestrator,
                                                 rf::BindlessImageRegistry* registry, int frames) {
    std::vector<std::vector<std::uint8_t>> out;
    host.resize(128, 96);
    const rt::ResourceId tex = host.createTexture(24, 16, 0xa05010u);
    const rt::ResourceId tex2 = host.createTexture(8, 8, 0x1080f0u);
    for (rt::ResourceId id : {tex, tex2}) {
        const rt::TextureDesc d = host.textureDesc(id);
        if (registry) {
            rf::ExternalImageDesc x;
            x.texture = id;
            x.vkImage = d.vkImage;
            x.vkFormat = kFormat;
            x.width = d.width;
            x.height = d.height;
            registry->registerImage(x, orchestrator ? orchestrator->retireSerial() : 1);
        }
        if (orchestrator && orchestrator->config().textureSwap) {
            check(orchestrator->swapTexture(d), "passthrough swap of a texture");
        }
    }
    auto destroy = [&](rt::ResourceId id) {
        if (orchestrator) {
            // The dispatcher's order: the host drops its swap, then the tap reports onImageDestroy.
            host.setTextureSwap(id, nullptr);
            orchestrator->onTextureDestroyed(id);
        }
        if (registry) {
            registry->release(id, orchestrator ? orchestrator->retireSerial() : 1);
        }
        host.destroyTexture(id);
    };
    bool tex2Alive = true;
    for (int f = 0; f < frames; ++f) {
        if (f == 2) {
            host.resize(160, 120);
        }
        if (f == 3) {
            host.updateTexture(tex, 0x3070b0u);
        }
        if (f == 4 && tex2Alive) {
            destroy(tex2);
            tex2Alive = false;
        }
        host.beginFrame(static_cast<std::uint32_t>(f));
        host.drawTextured(tex, 30, 20);
        if (tex2Alive) {
            host.drawTextured(tex2, 90, 50);
        }
        if (orchestrator) {
            const rf::InjectResult r = orchestrator->inject();
            check(r.injected, "frame " + std::to_string(f) + ": injected (" + r.error + ")");
            check(r.submit.passes == 1 && r.submit.imageBarriers > 0, "the frame graph planned barriers");
        }
        host.drawHud();
        out.push_back(host.present());
        if (orchestrator) {
            orchestrator->collect();
        }
    }
    destroy(tex);
    if (tex2Alive) {
        destroy(tex2);
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    bool validation = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-validation") == 0) {
            validation = false;
        }
    }
    if (validation) {
        if (!layerAvailable("VK_LAYER_KHRONOS_validation")) {
            std::printf("SKIP: VK_LAYER_KHRONOS_validation not installed (run with --no-validation)\n");
            return kSkip;
        }
        setEnv("VK_INSTANCE_LAYERS", "VK_LAYER_KHRONOS_validation");
        setEnv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT");
        setEnv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true");
    }
    rr::VulkanInstanceDesc idesc;
    idesc.appName = "rl_frame_vk";
    idesc.enableValidation = validation;
    auto instance = rr::VulkanInstance::create(idesc);
    if (!instance || !instance->isValid()) {
        std::printf("SKIP: no Vulkan instance\n");
        return kSkip;
    }
    const VkInstance vkInstance = static_cast<VkInstance>(instance->nativeHandle());
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkCreateDebugUtilsMessengerEXT"));
    auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkDestroyDebugUtilsMessengerEXT"));
    if (validation) {
        if (!createMessenger) {
            std::printf("FAIL: VK_EXT_debug_utils unavailable with validation\n");
            return 1;
        }
        auto mi = vks<VkDebugUtilsMessengerCreateInfoEXT>(VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = onMessage;
        createMessenger(vkInstance, &mi, nullptr, &messenger);
    }
    auto device = rr::VulkanDevice::create(*instance);
    if (!device || !device->isValid() || !device->info().timelineSemaphore) {
        std::printf("SKIP: no Vulkan device with timeline semaphores\n");
        return kSkip;
    }
    const VkDevice vkDevice = static_cast<VkDevice>(device->nativeHandle());
    std::printf("device: %s (%s)\n", device->info().deviceName.c_str(), validation ? "validation + sync validation"
                                                                                   : "no validation layer");

    int frames = 6;
    {
        SimHost host(vkInstance, static_cast<VkPhysicalDevice>(device->nativePhysicalDevice()), vkDevice,
                     static_cast<VkQueue>(device->queues().graphics), device->queues().graphicsFamily);

        // Reference: the tap off.
        const auto reference = runFrames(host, nullptr, nullptr, frames);

        // GPU bindless heap (WP-0.4) + real views.
        rr::BindlessDescriptors heap;
        heap.init(*device, rr::BindlessDesc{});
        rf::RendererBindlessHeap bindlessHeap(heap);
        ViewFactory views(vkDevice);
        rf::BindlessImageRegistry registry(bindlessHeap, &views);
        const std::uint32_t updatesBefore = heap.descriptorUpdateCount();

        // Passthrough composite + passthrough texture swap: bit-identical to the reference every frame.
        {
            rf::FrameConfig cfg;
            cfg.mode = rf::FrameMode::Passthrough;
            cfg.textureSwap = true;
            rf::FrameOrchestrator orch(cfg, &registry);
            check(orch.attach(&host), "attach: " + orch.lastError());
            const auto got = runFrames(host, &orch, &registry, frames);
            for (int f = 0; f < frames; ++f) {
                check(got[static_cast<std::size_t>(f)] == reference[static_cast<std::size_t>(f)],
                      "passthrough frame " + std::to_string(f) + " bit-identical to the tap-off reference");
            }
            const rf::OrchestratorStats& s = orch.stats();
            check(s.injections == static_cast<std::uint64_t>(frames), "one injection per frame");
            check(s.imageRebuilds == 1, "frame images rebuilt once on resize");
            check(s.swapsCreated == 2, "two passthrough swaps");
            check(orch.gpu().acquireCompleted() >= s.acquireValue - 1, "acquire timeline advanced");
            check(orch.gpu().waitRelease(s.releaseValue), "release timeline reached the last value");
            orch.collect();
            check(s.destroyed >= 3, "released images (old frame images, destroyed twin) destroyed after their acquire "
                                    "value (" + std::to_string(s.destroyed) + ")");
            // The registry: external (2 textures, one released), owned (twins + frame images).
            // 2 external textures + 2 twins + 2 frame images, and 2 frame images again after the resize.
            const rf::BindlessImageStats& b = registry.stats();
            check(b.registered == 8, "bindless registrations (" + std::to_string(b.registered) + " of 8)");
            check(b.external == 0 && b.released == 6, "every external texture and twin released (" +
                                                          std::to_string(b.released) + " of 6 before detach)");
            std::printf("bindless heap: %s backend, %u descriptor write(s), %u view(s) created\n",
                        rr::bindlessBackendName(heap.backend()), heap.descriptorUpdateCount() - updatesBefore,
                        views.created);
            if (heap.vulkanDescriptorsReady()) {
                check(heap.descriptorUpdateCount() > updatesBefore, "descriptors written for the registered images");
                check(views.created >= 8, "a view per registered image");
            }
            orch.detach();
            check(registry.stats().live == 0, "detach released FUSE's frame images from the registry");
            check(views.live == 0, "every view destroyed once the GPU was idle (" + std::to_string(views.live) + ")");
        }

        // Stale-handle semantics on the GPU heap: a released slot is invalid at once, reusable after collect.
        {
            rf::ExternalImageDesc x;
            x.texture = 4242;
            x.vkImage = 0x1000; // never read: no view factory for this one
            x.vkFormat = kFormat;
            rf::BindlessImageRegistry plain(bindlessHeap, nullptr);
            plain.registerImage(x, 1);
            const std::uint32_t handle = plain.shaderHandle(4242);
            check(handle != 0 && heap.validateShaderHandle(handle), "registered handle validates");
            plain.release(4242, 7);
            check(!heap.validateShaderHandle(handle), "released handle is stale at once");
            check(plain.collect(6) == 0 && plain.collect(7) == 1, "slot reclaimed only once serial 7 completed");
        }

        // Solid: FUSE's colour below the HUD.
        {
            rf::FrameConfig cfg;
            cfg.mode = rf::FrameMode::Solid;
            cfg.solidColor = 0x2050d0u;
            rf::FrameOrchestrator orch(cfg, nullptr);
            check(orch.attach(&host), "attach (solid): " + orch.lastError());
            const auto got = runFrames(host, &orch, nullptr, 3);
            const auto& img = got.back();
            const std::uint32_t w = host.width(), h = host.height();
            check(pixelIs(pixelAt(img, w, 2, 2), cfg.solidColor), "solid: background is FUSE's colour");
            check(pixelIs(pixelAt(img, w, 35, 25), cfg.solidColor), "solid: the textured draw before injection is covered");
            check(pixelIs(pixelAt(img, w, 12, h - 12), kHudColor), "solid: the HUD (after injection) stays on top");
            check(!pixelIs(pixelAt(reference[2], 160, 2, 2), cfg.solidColor), "solid: differs from the reference");
            orch.detach();
        }
        registry.releaseAll();
        check(views.live == 0, "every bindless view destroyed (" + std::to_string(views.live) + " live)");
        host.releaseScratch();
        heap.destroy(*device);
    }
    device->waitIdle();
    device.reset();
    if (messenger != VK_NULL_HANDLE) {
        destroyMessenger(vkInstance, messenger, nullptr);
    }
    instance.reset();
    check(g_validation == 0, std::to_string(g_validation) + " validation message(s)");
    if (g_failures) {
        std::printf("FAIL: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS: passthrough composite + texture swap bit-identical over %d frames (resize, texture update, "
                "texture destroy); solid composite below the HUD; bindless registration; %s\n",
                frames, validation ? "validation + synchronization validation clean" : "validation not run");
    return 0;
}
