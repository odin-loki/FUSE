// FUSE Relight RL-6.1: the developer overlay's compose pass on a real Vulkan device (Lavapipe), against its CPU
// reference (raster.hpp composeRegionCpu), with the Khronos validation layer + synchronization validation when available
// (native Linux; under Wine the host loader injects it: --external-validation, proved live by a negative control).
//
// OverlayGpu runs exactly as in the tap (prepare -> declare into an RG v2 graph -> record), the graph executed by the
// renderer's rg::Executor (every barrier planned from the declared accesses). The "back buffer" is the overlay's target
// image filled with random texels; per case the target is read back and compared with the CPU reference:
//   layer        random premultiplied layer over B8G8R8A8 and R8G8B8A8 targets, panels inside, clipped by the frame
//                edge and 1x1: bit-identical, and every texel outside the panel rectangle untouched;
//   debug views  albedo (R32G32B32A32_SFLOAT, rgb, nearest down-scale), normals (scale / bias 0.5), motion (rg0),
//                depth (D32_SFLOAT depth aspect, rrr, inverted): the whole frame shows the mapped buffer (<= 1 step per
//                channel: FMA contraction), the layer over it;
//   languages    Slang (primary) and the GLSL twin produce identical results;
//   zero_alloc   steady-state frames (prepare + graph declaration): no operator new;
//   validation   0 messages.
// Exit: 0 pass, 1 fail, 77 skip (no Vulkan device, or --validation required but no layer).
#include <fuse/relight/overlay/overlay_gpu.hpp>
#include <fuse/relight/overlay/raster.hpp>

#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include <fuse/relight/render/frame/vk_dispatch.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <vector>

// --- allocation counter (operator new) -----------------------------------------------------------
namespace {
thread_local bool t_count = false;
thread_local unsigned long long t_allocations = 0;
} // namespace

#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    if (t_count) {
        ++t_allocations;
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

namespace ov = fuse::relight::overlay;
namespace rf = fuse::relight::render::frame;
namespace rr = fuse::renderer;
namespace rg = fuse::renderer::rg;
namespace rt = fuse::relight::tap;

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

/// Negative control (VUID-VkSamplerCreateInfo-mipLodBias-01069) for a layer the process cannot enumerate.
int validationControl(VkPhysicalDevice pd, VkDevice device) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    const int before = g_validation;
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.mipLodBias = props.limits.maxSamplerLodBias + 64.f;
    ci.maxLod = 1.f;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device, &ci, nullptr, &sampler) == VK_SUCCESS) {
        vkDestroySampler(device, sampler, nullptr);
    }
    const int produced = g_validation - before;
    g_validation = before;
    return produced;
}

VkImageAspectFlags aspectOf(std::uint32_t format) {
    return format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

/// The harness's images and one-off transfers on the test device (what FrameGpu + the host do in the tap).
class TestDevice final : public ov::IOverlayDevice {
public:
    TestDevice(VkPhysicalDevice pd, VkDevice device, VkQueue queue, std::uint32_t family)
        : m_pd(pd), m_device(device), m_queue(queue) {
        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = family;
        vkCreateCommandPool(device, &pool, nullptr, &m_pool);
    }
    ~TestDevice() override {
        vkDeviceWaitIdle(m_device);
        vkDestroyCommandPool(m_device, m_pool, nullptr);
    }

    bool createImage(const rt::HostImageInfo& like, std::uint32_t usage, bool, rf::GpuImage& out) override {
        return create(like.format, like.width, like.height, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT, out);
    }
    void destroyImage(rf::GpuImage& image) override {
        vkDeviceWaitIdle(m_device);
        vkDestroyImage(m_device, rf::vkHandle<VkImage>(image.image.vkImage), nullptr);
        vkFreeMemory(m_device, rf::vkHandle<VkDeviceMemory>(image.memory), nullptr);
        image = rf::GpuImage{};
    }
    void waitSerial(std::uint64_t) override {} // every frame waits for the queue

    bool create(std::uint32_t format, std::uint32_t w, std::uint32_t h, std::uint32_t usage, rf::GpuImage& out) {
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = static_cast<VkFormat>(format);
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(m_device, &ci, nullptr, &image) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, image, &req);
        VkMemoryAllocateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mi.allocationSize = req.size;
        mi.memoryTypeIndex = memoryType(req.memoryTypeBits, 0);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        vkAllocateMemory(m_device, &mi, nullptr, &memory);
        vkBindImageMemory(m_device, image, memory, 0);
        out = rf::GpuImage{};
        out.image.vkImage = rf::vkValue(image);
        out.image.info.vkImage = out.image.vkImage;
        out.image.info.imageType = VK_IMAGE_TYPE_2D;
        out.image.info.format = format;
        out.image.info.width = w;
        out.image.info.height = h;
        out.image.info.usage = usage;
        out.image.info.aspects = aspectOf(format);
        out.memory = rf::vkValue(memory);
        VkCommandBuffer cmd = begin();
        barrier(cmd, image, aspectOf(format), VK_IMAGE_LAYOUT_UNDEFINED);
        end(cmd);
        return true;
    }

    /// Writes `bytes` into the image (GENERAL before and after).
    void upload(const rf::GpuImage& img, const void* bytes, std::size_t size) { transfer(img, bytes, nullptr, size); }
    std::vector<std::uint8_t> download(const rf::GpuImage& img, std::size_t size) {
        std::vector<std::uint8_t> out(size);
        transfer(img, nullptr, out.data(), size);
        return out;
    }

private:
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
    VkCommandBuffer begin() {
        VkCommandBufferAllocateInfo a{};
        a.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        a.commandPool = m_pool;
        a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        a.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(m_device, &a, &cmd);
        VkCommandBufferBeginInfo b{};
        b.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &b);
        return cmd;
    }
    void end(VkCommandBuffer cmd) {
        vkEndCommandBuffer(cmd);
        VkSubmitInfo s{};
        s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        s.commandBufferCount = 1;
        s.pCommandBuffers = &cmd;
        vkQueueSubmit(m_queue, 1, &s, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_queue);
        vkFreeCommandBuffers(m_device, m_pool, 1, &cmd);
    }
    /// Full barrier on `image` (old layout -> GENERAL) and on memory.
    static void barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout oldLayout) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = oldLayout;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {aspect, 0, 1, 0, 1};
        VkMemoryBarrier2 m{};
        m.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
        m.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
        m.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &m;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
    void transfer(const rf::GpuImage& img, const void* in, void* out, std::size_t size) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buffer = VK_NULL_HANDLE;
        vkCreateBuffer(m_device, &bi, nullptr, &buffer);
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_device, buffer, &req);
        VkMemoryAllocateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mi.allocationSize = req.size;
        mi.memoryTypeIndex =
            memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        vkAllocateMemory(m_device, &mi, nullptr, &memory);
        vkBindBufferMemory(m_device, buffer, memory, 0);
        void* mapped = nullptr;
        vkMapMemory(m_device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (in) {
            std::memcpy(mapped, in, size);
        }
        const VkImage image = rf::vkHandle<VkImage>(img.image.vkImage);
        const VkImageAspectFlags aspect = aspectOf(img.image.info.format);
        VkCommandBuffer cmd = begin();
        barrier(cmd, image, aspect, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {aspect, 0, 0, 1};
        region.imageExtent = {img.image.info.width, img.image.info.height, 1};
        if (in) {
            vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        } else {
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
        }
        barrier(cmd, image, aspect, VK_IMAGE_LAYOUT_GENERAL);
        end(cmd);
        if (out) {
            std::memcpy(out, mapped, size);
        }
        vkUnmapMemory(m_device, memory);
        vkDestroyBuffer(m_device, buffer, nullptr);
        vkFreeMemory(m_device, memory, nullptr);
    }

    VkPhysicalDevice m_pd;
    VkDevice m_device;
    VkQueue m_queue;
    VkCommandPool m_pool = VK_NULL_HANDLE;
};

struct DebugSource {
    rf::GpuImage image;
    std::vector<float> texels; ///< RGBA per texel as the shader sees them
    std::uint32_t width = 0, height = 0;
};

struct Case {
    const char* name;
    std::uint32_t format;
    ov::Rect panel;
    int debug; ///< -1 none, else index into the debug sources
    rf::DebugImage mapping;
};

std::vector<ov::Color> randomLayer(std::mt19937& rng, std::size_t n) {
    std::vector<ov::Color> layer(n);
    for (ov::Color& c : layer) {
        const std::uint32_t a = rng() % 4 == 0 ? 0u : (rng() % 4 == 1 ? 255u : rng() & 0xffu);
        c = ov::rgba(rng() & 0xffu, rng() & 0xffu, rng() & 0xffu, a);
    }
    return layer;
}

/// One case: returns the downloaded target (for the language comparison).
std::vector<std::uint8_t> runCase(const Case& c, ov::OverlayGpu& gpu, TestDevice& dev, rg::Executor& executor,
                                  rg::Graph& graph, std::vector<DebugSource>& sources, const char* lang, std::uint32_t seed) {
    constexpr std::uint32_t kW = 64, kH = 48;
    std::mt19937 rng(seed);
    const std::vector<ov::Color> layer = randomLayer(rng, static_cast<std::size_t>(std::max(0, c.panel.w * c.panel.h)));
    rf::DebugImage debug = c.mapping;
    if (c.debug >= 0) {
        const DebugSource& s = sources[static_cast<std::size_t>(c.debug)];
        debug.vkImage = s.image.image.vkImage;
        debug.format = s.image.image.info.format;
        debug.width = s.width;
        debug.height = s.height;
        debug.aspect = aspectOf(debug.format);
        debug.layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    ov::OverlayFrameDesc d;
    d.frameW = kW;
    d.frameH = kH;
    d.frameFormat = c.format;
    d.panel = c.panel;
    d.layer = &layer;
    d.debug = c.debug >= 0 ? &debug : nullptr;
    d.serial = 1;
    const std::string where = std::string(lang) + " " + c.name;
    if (!gpu.prepare(d)) {
        check(false, where + ": prepare: " + gpu.lastError());
        return {};
    }
    std::vector<std::uint32_t> before(kW * kH);
    for (std::uint32_t& t : before) {
        t = rng();
    }
    dev.upload(gpu.target(), before.data(), before.size() * 4u);
    graph.reset();
    rg::ImportedImage out;
    out.image = reinterpret_cast<void*>(static_cast<std::uintptr_t>(gpu.target().image.vkImage));
    out.format = c.format;
    out.width = kW;
    out.height = kH;
    out.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    out.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    out.name = "test.target";
    const rg::TextureRef ref = graph.importImage(out);
    check(gpu.declare(graph, ref, gpu.target()), where + ": declare");
    const rg::ExecuteResult r = executor.execute(graph);
    check(r.ok && r.executedPasses == 3, where + ": graph executed (3 passes)");
    executor.waitIdle();
    gpu.submitted(1);
    const std::vector<std::uint8_t> got = dev.download(gpu.target(), before.size() * 4u);

    // CPU reference over the region.
    const ov::OverlayPush& p = gpu.push();
    std::vector<std::uint32_t> region(static_cast<std::size_t>(p.regionW) * p.regionH);
    for (std::uint32_t y = 0; y < p.regionH; ++y) {
        for (std::uint32_t x = 0; x < p.regionW; ++x) {
            region[y * p.regionW + x] = before[(p.regionY + y) * kW + p.regionX + x];
        }
    }
    ov::DebugMapping m;
    for (int i = 0; i < 4; ++i) {
        m.scale[i] = debug.scale[i];
        m.bias[i] = debug.bias[i];
    }
    m.swizzle = debug.swizzle;
    ov::composeRegionCpu(p, m, c.debug >= 0 ? sources[static_cast<std::size_t>(c.debug)].texels.data() : nullptr,
                         layer, region);
    std::vector<std::uint32_t> expected = before;
    for (std::uint32_t y = 0; y < p.regionH; ++y) {
        for (std::uint32_t x = 0; x < p.regionW; ++x) {
            expected[(p.regionY + y) * kW + p.regionX + x] = region[y * p.regionW + x];
        }
    }
    int outside = 0, exactMismatch = 0, farMismatch = 0;
    const ov::Rect regionRect{p.regionX, p.regionY, static_cast<std::int32_t>(p.regionW),
                              static_cast<std::int32_t>(p.regionH)};
    for (std::uint32_t i = 0; i < kW * kH; ++i) {
        std::uint32_t g = 0;
        std::memcpy(&g, got.data() + i * 4u, 4);
        const bool inRegion = regionRect.contains(static_cast<std::int32_t>(i % kW), static_cast<std::int32_t>(i / kW));
        if (!inRegion) {
            outside += g != before[i] ? 1 : 0;
            continue;
        }
        if (g != expected[i]) {
            ++exactMismatch;
            for (int ch = 0; ch < 4; ++ch) {
                const int a = static_cast<int>((g >> (8 * ch)) & 0xffu), b = static_cast<int>((expected[i] >> (8 * ch)) & 0xffu);
                if (std::abs(a - b) > (c.debug >= 0 && ch < 3 ? 1 : 0)) {
                    ++farMismatch;
                    if (farMismatch <= 3) {
                        std::printf("  %s texel %u ch %d: gpu %d cpu %d\n", where.c_str(), i, ch, a, b);
                    }
                }
            }
        }
    }
    check(outside == 0, where + ": " + std::to_string(outside) + " texel(s) outside the region changed");
    if (c.debug < 0) {
        check(exactMismatch == 0, where + ": " + std::to_string(exactMismatch) + " texel(s) differ from the CPU reference");
    } else {
        check(farMismatch == 0, where + ": " + std::to_string(farMismatch) + " channel(s) off by more than one step (" +
                                    std::to_string(exactMismatch) + " texel(s) not exact)");
    }
    const bool fullFrame = c.debug >= 0;
    check(fullFrame ? (p.regionW == kW && p.regionH == kH) : true, where + ": a debug view covers the frame");
    return got;
}

} // namespace

int main(int argc, char** argv) {
    bool validation = true;
    bool external = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-validation") == 0) {
            validation = false;
        } else if (std::strcmp(argv[i], "--external-validation") == 0) {
            external = true;
        }
    }
    if (validation && !external) {
        if (!layerAvailable("VK_LAYER_KHRONOS_validation")) {
            std::printf("SKIP: VK_LAYER_KHRONOS_validation not installed (run with --no-validation)\n");
            return kSkip;
        }
        setEnv("VK_INSTANCE_LAYERS", "VK_LAYER_KHRONOS_validation");
        setEnv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT");
        setEnv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true");
    }
    rr::VulkanInstanceDesc idesc;
    idesc.appName = "rl_overlay_vk";
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
    if (validation || external) {
        if (!createMessenger) {
            std::printf("FAIL: VK_EXT_debug_utils unavailable with validation\n");
            return 1;
        }
        VkDebugUtilsMessengerCreateInfoEXT mi{};
        mi.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = onMessage;
        createMessenger(vkInstance, &mi, nullptr, &messenger);
    }
    auto device = rr::VulkanDevice::create(*instance);
    if (!device || !device->isValid()) {
        std::printf("SKIP: no Vulkan device\n");
        return kSkip;
    }
    const VkDevice vkDevice = static_cast<VkDevice>(device->nativeHandle());
    const VkPhysicalDevice vkPd = static_cast<VkPhysicalDevice>(device->nativePhysicalDevice());
    if (external) {
        const int control = validationControl(vkPd, vkDevice);
        if (control == 0) {
            std::printf("SKIP: no validation layer reached this process (negative control silent)\n");
            return kSkip;
        }
        std::printf("external validation layer live (negative control: %d message(s), not counted)\n", control);
        validation = true;
    }
    std::size_t caseCount = 0;
    std::printf("device: %s (%s)\n", device->info().deviceName.c_str(),
                validation ? "validation + sync validation" : "no validation layer");
    {
        TestDevice dev(vkPd, vkDevice, static_cast<VkQueue>(device->queues().graphics), device->queues().graphicsFamily);
        auto executor = rg::Executor::create(*device);
        if (!executor) {
            std::printf("FAIL: rg::Executor\n");
            return 1;
        }
        // Debug sources: an RGBA float buffer (albedo / normals / motion) and a depth buffer.
        std::vector<DebugSource> sources(2);
        {
            DebugSource& s = sources[0];
            s.width = 32;
            s.height = 24;
            std::mt19937 rng(7);
            std::uniform_real_distribution<float> u(-0.5f, 1.5f);
            s.texels.resize(s.width * s.height * 4u);
            for (float& t : s.texels) {
                t = u(rng);
            }
            dev.create(VK_FORMAT_R32G32B32A32_SFLOAT, s.width, s.height,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       s.image);
            dev.upload(s.image, s.texels.data(), s.texels.size() * 4u);
        }
        {
            DebugSource& s = sources[1];
            s.width = 40;
            s.height = 30;
            std::vector<float> depth(s.width * s.height);
            for (std::uint32_t i = 0; i < depth.size(); ++i) {
                depth[i] = static_cast<float>((i * 37u) % 1000u) / 999.f;
            }
            s.texels.resize(depth.size() * 4u);
            for (std::size_t i = 0; i < depth.size(); ++i) {
                s.texels[i * 4] = depth[i];
                s.texels[i * 4 + 1] = 0.f;
                s.texels[i * 4 + 2] = 0.f;
                s.texels[i * 4 + 3] = 1.f;
            }
            dev.create(VK_FORMAT_D32_SFLOAT, s.width, s.height,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       s.image);
            dev.upload(s.image, depth.data(), depth.size() * 4u);
        }
        rf::DebugImage plain;
        rf::DebugImage normals;
        for (int i = 0; i < 4; ++i) {
            normals.scale[i] = 0.5f;
            normals.bias[i] = 0.5f;
        }
        rf::DebugImage motion;
        motion.swizzle = ov::kOverlaySwizzleRg0;
        motion.scale[0] = motion.scale[1] = 4.f;
        motion.bias[0] = motion.bias[1] = 0.5f;
        rf::DebugImage depthMap;
        depthMap.swizzle = ov::kOverlaySwizzleRrr;
        depthMap.scale[0] = -1.f;
        depthMap.bias[0] = 1.f;
        const Case cases[] = {
            {"layer_bgra", VK_FORMAT_B8G8R8A8_UNORM, ov::Rect{5, 7, 40, 30}, -1, plain},
            {"layer_rgba_srgb", VK_FORMAT_R8G8B8A8_SRGB, ov::Rect{0, 0, 64, 48}, -1, plain},
            {"layer_edge", VK_FORMAT_B8G8R8A8_UNORM, ov::Rect{50, 40, 30, 20}, -1, plain},
            {"layer_1x1", VK_FORMAT_R8G8B8A8_UNORM, ov::Rect{63, 47, 1, 1}, -1, plain},
            {"debug_albedo", VK_FORMAT_B8G8R8A8_UNORM, ov::Rect{4, 4, 30, 20}, 0, plain},
            {"debug_normals", VK_FORMAT_R8G8B8A8_UNORM, ov::Rect{10, 10, 12, 12}, 0, normals},
            {"debug_motion", VK_FORMAT_B8G8R8A8_SRGB, ov::Rect{4, 4, 30, 20}, 0, motion},
            {"debug_depth", VK_FORMAT_B8G8R8A8_UNORM, ov::Rect{4, 4, 30, 20}, 1, depthMap},
        };
        caseCount = sizeof(cases) / sizeof(cases[0]);
        std::vector<std::vector<std::uint8_t>> results[2];
        const ov::OverlayGpu::Language langs[2] = {ov::OverlayGpu::Language::Slang, ov::OverlayGpu::Language::Glsl};
        rg::Graph graph;
        for (int l = 0; l < 2; ++l) {
            const char* name = l == 0 ? "slang" : "glsl";
            if (!ov::OverlayGpu::languageAvailable(langs[l])) {
                std::printf("%s: not compiled in this tree, skipped\n", name);
                continue;
            }
            ov::OverlayGpu gpu;
            if (!gpu.init(rf::vkValue(vkDevice), rf::vkValue(vkPd), dev, langs[l])) {
                check(false, std::string(name) + ": init: " + gpu.lastError());
                continue;
            }
            std::uint32_t seed = 100;
            for (const Case& c : cases) {
                results[l].push_back(runCase(c, gpu, dev, *executor, graph, sources, name, seed++));
            }
            // Zero steady-state allocations: prepare + graph declaration of an unchanged frame.
            std::vector<ov::Color> layer(40u * 30u, ov::rgba(10, 20, 30, 128));
            ov::OverlayFrameDesc d;
            d.frameW = 64;
            d.frameH = 48;
            d.frameFormat = VK_FORMAT_B8G8R8A8_UNORM;
            d.panel = ov::Rect{5, 7, 40, 30};
            d.layer = &layer;
            for (int i = 0; i < 8; ++i) {
                gpu.prepare(d);
                graph.reset();
                rg::ImportedImage out;
                out.image = reinterpret_cast<void*>(static_cast<std::uintptr_t>(gpu.target().image.vkImage));
                out.format = d.frameFormat;
                out.width = 64;
                out.height = 48;
                gpu.declare(graph, graph.importImage(out), gpu.target());
            }
            t_allocations = 0;
            t_count = true;
            for (int i = 0; i < 64; ++i) {
                gpu.prepare(d);
                graph.reset();
                rg::ImportedImage out;
                out.image = reinterpret_cast<void*>(static_cast<std::uintptr_t>(gpu.target().image.vkImage));
                out.format = d.frameFormat;
                out.width = 64;
                out.height = 48;
                gpu.declare(graph, graph.importImage(out), gpu.target());
            }
            t_count = false;
            check(t_allocations == 0,
                  std::string(name) + ": steady-state prepare + declare: " + std::to_string(t_allocations) +
                      " operator new call(s)");
            gpu.shutdown();
        }
        if (!results[0].empty() && !results[1].empty()) {
            for (std::size_t i = 0; i < results[0].size() && i < results[1].size(); ++i) {
                check(results[0][i] == results[1][i],
                      std::string("Slang and GLSL agree on ") + cases[i].name);
            }
        }
        executor->waitIdle();
        for (DebugSource& s : sources) {
            dev.destroyImage(s.image);
        }
    }
    if (messenger != VK_NULL_HANDLE && destroyMessenger) {
        destroyMessenger(vkInstance, messenger, nullptr);
    }
    check(g_validation == 0, std::to_string(g_validation) + " validation message(s)");
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS: rl_overlay_vk: %zu case(s) x languages GPU == CPU reference (layer bit-exact, debug views <= 1 "
                "step), outside the region untouched, zero steady-state allocations, %s\n",
                caseCount, validation ? "0 validation messages" : "validation not run");
    return 0;
}
