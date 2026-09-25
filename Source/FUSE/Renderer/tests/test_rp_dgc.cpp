// WP-9.3 device-generated commands: Lavapipe gates. CPU gates: test_rp_dgc_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): GpuScene (WP-1.1) delta upload, InstanceCuller phase 1
// (WP-1.3, frustum culling), then two DgcPipelineSelectors over the same culled draws:
//   "A" (DgcMode::Auto)          DGC when the device has it: one vkCmdExecuteGeneratedCommandsEXT
//   "B" (DgcMode::ForceFallback) the indirect-count path split by bucket: buckets x (bind + draw count)
// each into its own depth + ID target, plus the plain WP-1.3 indirect-count draw (one pipeline) into a
// third target. The bucket pipelines differ by a specialization constant and write (slot << 6) | bucket.
//
//   --mode parity    device from VulkanDevice::create (the local 1.3.275 headers cannot enable DGC, so
//                    A falls back and must report why); validation + sync validation, 0 messages.
//                    Per frame and kernel language: GPU sequences == CPU reference (dgc_generate_reference
//                    of the read-back culled args) word for word; GPU bucket args == reference per bucket;
//                    the CPU emulation of the indirect-commands layout over the GPU sequences == the
//                    indirect-count draw list of the GPU bucket args; A image == B image (bit exact) ==
//                    the WP-1.3 image with each slot's bucket; Slang == GLSL.
//   --mode dgc       device created here with VK_EXT_device_generated_commands + VK_KHR_maintenance5 and
//                    wrapped with VulkanDevice::adopt (feature chain handed to the selector): A runs real
//                    DGC on Lavapipe. Same checks, plus A records 2 commands vs 2 x buckets for B. The
//                    Khronos validation layer here is 1.3.275 and predates the extension, so this mode
//                    runs without it (--validate forces it on to list what the old layer reports).
//   --mode fallback  capability reporting: a standard device reports ExtensionNotEnabled / FeatureState-
//                    Unknown, an adopted DGC device without the chain reports FeatureStateUnknown, and
//                    ForceFallback reports ForcedFallback; the fallback renders the reference image.
//   --mode zero_alloc  steady-state frames: 0 operator-new calls in the selector calls (beginFrame,
//                    importInto, addGenerate, useDraws) and the dgc.* / draw callbacks.
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, no kernel built, backend unsupported).
#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/dgc/dgc_pipeline_selector.hpp>
#include <fuse/renderer/vk/dgc/dgc_reference.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

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
constexpr int kSkip = 77;
int g_failures = 0;

[[maybe_unused]] void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}
} // namespace

#if !defined(FUSE_VULKAN_BACKEND)

int main() {
    std::printf("SKIP: stub build (no Vulkan backend)\n");
    return kSkip;
}

#else

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::culling;
using namespace fuse::renderer::dgc;
using namespace fuse::renderer::gpu_scene;
using fuse::f32;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr usize kStagingBytes = 8u * 1024u * 1024u;
constexpr usize kReadbackBytes = 16u * 1024u * 1024u;
constexpr u32 kWidth = 256;
constexpr u32 kHeight = 192;
constexpr u32 kFormatR32Uint = 98u;
constexpr u32 kBuckets = 6;
constexpr u32 kMaterials = 12;
constexpr u32 kInstances = 3000;

u32 g_messages = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) ==
            0 ||
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) == 0) {
        return VK_FALSE;
    }
    ++g_messages;
    if (g_messages <= 20u) {
        std::fprintf(stderr, "VALIDATION MESSAGE: %s\n  %s\n",
                     data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName : "(no id)",
                     data != nullptr && data->pMessage != nullptr ? data->pMessage : "");
    }
    return VK_FALSE;
}

bool layerAvailable(const char* name) {
    u32 count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<char> readFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// --- math (column-major, Vulkan clip space, forward depth) ----------------------------------------
struct Mat4 {
    f32 m[16] = {};
};

Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            f32 s = 0.f;
            for (int k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}

Mat4 camera(f32 ex, f32 ey, f32 ez, f32 ax, f32 ay, f32 az) {
    const f32 fovY = 1.0f, aspect = static_cast<f32>(kWidth) / static_cast<f32>(kHeight), zn = 0.1f, zf = 200.f;
    const f32 f = 1.f / std::tan(fovY * 0.5f);
    Mat4 p{};
    p.m[0] = f / aspect;
    p.m[5] = -f;
    p.m[10] = zf / (zn - zf);
    p.m[11] = -1.f;
    p.m[14] = zn * zf / (zn - zf);
    f32 fw[3] = {ax - ex, ay - ey, az - ez};
    const f32 fl = std::sqrt(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
    for (f32& v : fw) {
        v /= fl;
    }
    const f32 up[3] = {0.f, 1.f, 0.f};
    f32 s[3] = {fw[1] * up[2] - fw[2] * up[1], fw[2] * up[0] - fw[0] * up[2], fw[0] * up[1] - fw[1] * up[0]};
    const f32 sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (f32& v : s) {
        v /= sl;
    }
    const f32 u[3] = {s[1] * fw[2] - s[2] * fw[1], s[2] * fw[0] - s[0] * fw[2], s[0] * fw[1] - s[1] * fw[0]};
    Mat4 v{};
    v.m[0] = s[0];
    v.m[4] = s[1];
    v.m[8] = s[2];
    v.m[1] = u[0];
    v.m[5] = u[1];
    v.m[9] = u[2];
    v.m[2] = -fw[0];
    v.m[6] = -fw[1];
    v.m[10] = -fw[2];
    v.m[12] = -(s[0] * ex + s[1] * ey + s[2] * ez);
    v.m[13] = -(u[0] * ex + u[1] * ey + u[2] * ez);
    v.m[14] = fw[0] * ex + fw[1] * ey + fw[2] * ez;
    v.m[15] = 1.f;
    return mul(p, v);
}

GpuTransform boxTransform(f32 x, f32 y, f32 z, f32 sx, f32 sy, f32 sz) {
    GpuTransform t{};
    t.rows[0][0] = sx;
    t.rows[1][1] = sy;
    t.rows[2][2] = sz;
    t.rows[0][3] = x;
    t.rows[1][3] = y;
    t.rows[2][3] = z;
    return t;
}

// --- Vulkan context -------------------------------------------------------------------------------
struct Target {
    Texture depth{};
    Texture id{};
    u32 depthLayout = 0;
    u32 idLayout = 0;
    u8 depthQueue = rg::kNoQueue;
    u8 idQueue = rg::kNoQueue;
};

/// Feature chain of a device created here (kept alive for adopt() and the selector).
struct FeatureChain {
    VkPhysicalDeviceFeatures2 core{};
    VkPhysicalDeviceVulkan11Features v11{};
    VkPhysicalDeviceVulkan12Features v12{};
    VkPhysicalDeviceVulkan13Features v13{};
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBuffer{};
    struct Dgc {
        VkStructureType sType;
        void* pNext;
        VkBool32 deviceGeneratedCommands;
        VkBool32 dynamicGeneratedPipelineLayout;
    } dgc{};
};
constexpr VkStructureType kSTypeDgcFeatures = static_cast<VkStructureType>(1000572000);

enum class DeviceKind : u8 { Standard, Dgc };

struct Context {
    std::unique_ptr<VulkanInstance> instance;
    std::unique_ptr<VulkanDevice> device;
    std::unique_ptr<GpuAllocator> allocator;
    std::unique_ptr<rg::Executor> executor;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkPipelineLayout rasterLayout = VK_NULL_HANDLE;
    FeatureChain chain{};
    bool dgcDevice = false;
    std::vector<const char*> extensions;
    BindlessDescriptors bindless;
    UploadQueue upload;
    Buffer staging{};
    Buffer readback{};
    Buffer vertices{};
    Buffer indices{};
    Target targets[3]{}; ///< A (Auto / DGC), B (fallback), R (WP-1.3 single pipeline)
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        upload.destroy();
        if (vkDevice != VK_NULL_HANDLE && rasterLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(vkDevice, rasterLayout, nullptr);
        }
        if (allocator != nullptr) {
            for (Target& t : targets) {
                allocator->destroyImage(t.depth);
                allocator->destroyImage(t.id);
            }
            allocator->destroyBuffer(readback);
            allocator->destroyBuffer(staging);
            allocator->destroyBuffer(vertices);
            allocator->destroyBuffer(indices);
        }
        if (device != nullptr) {
            bindless.collectRetired(~0ull);
            bindless.destroy(*device);
        }
        allocator.reset();
        device.reset();
        if (messenger != VK_NULL_HANDLE && destroyMessenger != nullptr && instance != nullptr) {
            destroyMessenger(static_cast<VkInstance>(instance->nativeHandle()), messenger, nullptr);
        }
        instance.reset();
    }
};

bool deviceHasExtension(VkPhysicalDevice pd, const char* name) {
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, props.data());
    for (const VkExtensionProperties& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

/// Creates a device with VK_EXT_device_generated_commands + VK_KHR_maintenance5 (+ descriptor buffer)
/// and every supported core feature, wraps it with VulkanDevice::adopt. Returns 77 when unavailable.
int createDgcDevice(Context& ctx, bool descriptorBuffer) {
    const VkInstance vkInstance = static_cast<VkInstance>(ctx.instance->nativeHandle());
    u32 count = 0;
    vkEnumeratePhysicalDevices(vkInstance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(vkInstance, &count, devices.data());
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    for (VkPhysicalDevice d : devices) {
        if (deviceHasExtension(d, "VK_EXT_device_generated_commands") && deviceHasExtension(d, "VK_KHR_maintenance5") &&
            (!descriptorBuffer || deviceHasExtension(d, "VK_EXT_descriptor_buffer"))) {
            pd = d;
            break;
        }
    }
    if (pd == VK_NULL_HANDLE) {
        std::printf("SKIP: no physical device with VK_EXT_device_generated_commands + VK_KHR_maintenance5%s\n",
                    descriptorBuffer ? " + VK_EXT_descriptor_buffer" : "");
        return kSkip;
    }
    FeatureChain& c = ctx.chain;
    c.core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    c.v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    c.v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    c.v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    c.m5.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR;
    c.descriptorBuffer.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
    c.dgc.sType = kSTypeDgcFeatures;
    c.core.pNext = &c.v11;
    c.v11.pNext = &c.v12;
    c.v12.pNext = &c.v13;
    c.v13.pNext = &c.m5;
    c.m5.pNext = &c.dgc;
    c.dgc.pNext = descriptorBuffer ? static_cast<void*>(&c.descriptorBuffer) : nullptr;
    vkGetPhysicalDeviceFeatures2(pd, &c.core);
    if (c.dgc.deviceGeneratedCommands != VK_TRUE || c.m5.maintenance5 != VK_TRUE) {
        std::printf("SKIP: deviceGeneratedCommands / maintenance5 feature not supported\n");
        return kSkip;
    }
    c.core.features.robustBufferAccess = VK_FALSE;
    c.dgc.dynamicGeneratedPipelineLayout = VK_FALSE;
    if (descriptorBuffer) {
        c.descriptorBuffer.descriptorBufferCaptureReplay = VK_FALSE;
        c.descriptorBuffer.descriptorBufferImageLayoutIgnored = VK_FALSE;
        c.descriptorBuffer.descriptorBufferPushDescriptors = VK_FALSE;
    }
    u32 families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &families, nullptr);
    std::vector<VkQueueFamilyProperties> fam(families);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &families, fam.data());
    u32 graphics = UINT32_MAX;
    for (u32 i = 0; i < families; ++i) {
        if ((fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
            graphics = i;
            break;
        }
    }
    if (graphics == UINT32_MAX) {
        std::printf("SKIP: no graphics queue\n");
        return kSkip;
    }
    ctx.extensions = {"VK_EXT_device_generated_commands", "VK_KHR_maintenance5"};
    if (descriptorBuffer) {
        ctx.extensions.push_back("VK_EXT_descriptor_buffer");
    }
    const f32 priority = 1.f;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = graphics;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &c.core;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = static_cast<u32>(ctx.extensions.size());
    info.ppEnabledExtensionNames = ctx.extensions.data();
    VkDevice dev = VK_NULL_HANDLE;
    if (vkCreateDevice(pd, &info, nullptr, &dev) != VK_SUCCESS) {
        std::fprintf(stderr, "FAIL: vkCreateDevice with VK_EXT_device_generated_commands\n");
        return 1;
    }
    VulkanDeviceAdoptDesc adopt{};
    adopt.instance = vkInstance;
    adopt.physicalDevice = pd;
    adopt.device = dev;
    adopt.enabledExtensions = ctx.extensions.data();
    adopt.enabledExtensionCount = static_cast<u32>(ctx.extensions.size());
    adopt.enabledFeatureChain = &c.core;
    adopt.graphicsFamily = graphics;
    adopt.takeOwnership = true;
    ctx.device = VulkanDevice::adopt(adopt);
    ctx.dgcDevice = true;
    return 0;
}

bool createTarget(Context& ctx, Target& t, const char* name) {
    TextureDesc depth{};
    depth.width = kWidth;
    depth.height = kHeight;
    depth.format = GpuFormat::D32Sfloat;
    depth.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::DepthStencilAttachment) |
                                          static_cast<u32>(ImageUsage::TransferSrc));
    depth.name = name;
    TextureDesc id{};
    id.width = kWidth;
    id.height = kHeight;
    id.format = static_cast<GpuFormat>(kFormatR32Uint);
    id.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::ColorAttachment) |
                                       static_cast<u32>(ImageUsage::TransferSrc));
    id.name = name;
    return ctx.allocator->createImage(depth, t.depth) && ctx.allocator->createImage(id, t.id);
}

int setup(Context& ctx, DeviceKind kind, bool descriptorBuffer, bool validation) {
    if (validation) {
        if (!layerAvailable(kValidationLayer)) {
            std::printf("SKIP: %s not installed (the gate needs synchronization validation)\n", kValidationLayer);
            return kSkip;
        }
        setenv("VK_INSTANCE_LAYERS", kValidationLayer, 1);
        setenv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", 1);
        setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
        setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK", 1);
    } else {
        setenv("VK_INSTANCE_LAYERS", "", 1);
    }
    VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "fuse_rp_dgc";
    instanceDesc.enableValidation = validation;
    ctx.instance = VulkanInstance::create(instanceDesc);
    if (ctx.instance == nullptr || !ctx.instance->isValid()) {
        std::printf("SKIP: no Vulkan instance\n");
        return kSkip;
    }
    const VkInstance vkInstance = static_cast<VkInstance>(ctx.instance->nativeHandle());
    auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkCreateDebugUtilsMessengerEXT"));
    ctx.destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkDestroyDebugUtilsMessengerEXT"));
    if (createMessenger == nullptr && validation) {
        std::printf("SKIP: VK_EXT_debug_utils unavailable\n");
        return kSkip;
    }
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = onMessage;
    if (createMessenger != nullptr) {
        createMessenger(vkInstance, &info, nullptr, &ctx.messenger);
    }
    if (kind == DeviceKind::Dgc) {
        const int rc = createDgcDevice(ctx, descriptorBuffer);
        if (rc != 0) {
            return rc;
        }
    } else {
        ctx.device = VulkanDevice::create(*ctx.instance, VulkanDeviceDesc{});
    }
    if (ctx.device == nullptr || !ctx.device->isValid()) {
        std::printf("SKIP: no Vulkan device\n");
        return kSkip;
    }
    const RendererCaps& caps = ctx.device->info().caps;
    if (!caps.bufferDeviceAddress || !caps.drawIndirectCount || !caps.dynamicRendering) {
        std::printf("SKIP: device lacks bufferDeviceAddress / drawIndirectCount / dynamicRendering\n");
        return kSkip;
    }
    if (descriptorBuffer && !caps.descriptorBuffer) {
        std::printf("SKIP: device has no VK_EXT_descriptor_buffer\n");
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s (%s)\n", ctx.device->info().deviceName.c_str(),
                kind == DeviceKind::Dgc ? "created here with VK_EXT_device_generated_commands, adopted"
                                        : "VulkanDevice::create");
    BindlessDesc bdesc{};
    bdesc.backend = descriptorBuffer ? BindlessBackendPreference::DescriptorBuffer : BindlessBackendPreference::DescriptorSet;
    if (!ctx.bindless.init(*ctx.device, bdesc)) {
        std::fprintf(stderr, "FAIL: bindless init\n");
        return 1;
    }
    std::printf("bindless backend: %s\n", bindlessBackendName(ctx.bindless.backend()));
    BufferDesc stagingDesc{};
    stagingDesc.size = kStagingBytes;
    stagingDesc.usage = BufferUsage::TransferSrc;
    stagingDesc.memoryUsage = MemoryUsage::CpuToGpu;
    stagingDesc.name = "rp_dgc.staging";
    BufferDesc readbackDesc{};
    readbackDesc.size = kReadbackBytes;
    readbackDesc.usage = BufferUsage::TransferDst;
    readbackDesc.memoryUsage = MemoryUsage::GpuToCpu;
    readbackDesc.name = "rp_dgc.readback";
    const f32 cube[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                            {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
    const u32 cubeIndices[36] = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
                                 3, 2, 6, 3, 6, 7, 0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2};
    BufferDesc vbDesc{};
    vbDesc.size = sizeof(cube);
    vbDesc.usage = BufferUsage::Vertex;
    vbDesc.memoryUsage = MemoryUsage::CpuToGpu;
    vbDesc.name = "rp_dgc.cube_vertices";
    BufferDesc ibDesc{};
    ibDesc.size = sizeof(cubeIndices);
    ibDesc.usage = BufferUsage::Index;
    ibDesc.memoryUsage = MemoryUsage::CpuToGpu;
    ibDesc.name = "rp_dgc.cube_indices";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.allocator->createBuffer(readbackDesc, ctx.readback) || ctx.readback.mapped == nullptr ||
        !ctx.allocator->createBuffer(vbDesc, ctx.vertices) || ctx.vertices.mapped == nullptr ||
        !ctx.allocator->createBuffer(ibDesc, ctx.indices) || ctx.indices.mapped == nullptr) {
        std::fprintf(stderr, "FAIL: buffers\n");
        return 1;
    }
    std::memcpy(ctx.vertices.mapped, cube, sizeof(cube));
    std::memcpy(ctx.indices.mapped, cubeIndices, sizeof(cubeIndices));
    if (!ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: UploadQueue\n");
        return 1;
    }
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    const char* names[3] = {"rp_dgc.a", "rp_dgc.b", "rp_dgc.ref"};
    for (u32 i = 0; i < 3u; ++i) {
        if (!createTarget(ctx, ctx.targets[i], names[i])) {
            std::fprintf(stderr, "FAIL: render targets\n");
            return 1;
        }
    }
    VkDescriptorSetLayout setLayout = static_cast<VkDescriptorSetLayout>(ctx.bindless.layoutHandle());
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 80};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(ctx.vkDevice, &layoutInfo, nullptr, &ctx.rasterLayout) != VK_SUCCESS) {
        return 1;
    }
    return 0;
}

// --- bucket pipelines ----------------------------------------------------------------------------
struct Pipelines {
    std::vector<VkPipeline> buckets; ///< kBuckets, bucket = specialization constant 0
    VkDevice device = VK_NULL_HANDLE;
    ~Pipelines() {
        for (VkPipeline p : buckets) {
            vkDestroyPipeline(device, p, nullptr);
        }
    }
};

VkShaderModule loadModule(Context& ctx, const char* path) {
    const std::vector<char> code = readFile(path);
    if (code.empty() || code.size() % 4u != 0u) {
        std::fprintf(stderr, "FAIL: %s not readable\n", path);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(ctx.vkDevice, &info, nullptr, &module);
    return module;
}

bool createPipelines(Context& ctx, Pipelines& out, const char* vsPath, const char* fsPath, u64 flags2) {
    out.device = ctx.vkDevice;
    VkShaderModule vs = loadModule(ctx, vsPath);
    VkShaderModule fs = loadModule(ctx, fsPath);
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        return false;
    }
    bool ok = true;
    for (u32 b = 0; b < kBuckets && ok; ++b) {
        const u32 bucket = b;
        const VkSpecializationMapEntry entry{0, 0, sizeof(u32)};
        VkSpecializationInfo spec{1, &entry, sizeof(u32), &bucket};
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";
        stages[1].pSpecializationInfo = &spec;
        VkVertexInputBindingDescription binding{0, 12, VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions = &attribute;
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rasterization{};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        const VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamics;
        const VkFormat colorFormat = VK_FORMAT_R32_UINT;
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat;
        rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        // DGC: VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT through the maintenance5 flags-2 struct
        // (which then replaces VkGraphicsPipelineCreateInfo::flags, so it carries the bindless flags too).
        VkPipelineCreateFlags2CreateInfoKHR flagsInfo{};
        flagsInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR;
        flagsInfo.pNext = &rendering;
        flagsInfo.flags = flags2 | static_cast<u64>(ctx.bindless.pipelineCreateFlags());
        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext = flags2 != 0u ? static_cast<const void*>(&flagsInfo) : static_cast<const void*>(&rendering);
        info.flags = static_cast<VkPipelineCreateFlags>(ctx.bindless.pipelineCreateFlags());
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &rasterization;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = ctx.rasterLayout;
        VkPipeline p = VK_NULL_HANDLE;
        ok = vkCreateGraphicsPipelines(ctx.vkDevice, VK_NULL_HANDLE, 1, &info, nullptr, &p) == VK_SUCCESS;
        out.buckets.push_back(p);
    }
    vkDestroyShaderModule(ctx.vkDevice, vs, nullptr);
    vkDestroyShaderModule(ctx.vkDevice, fs, nullptr);
    return ok;
}

struct Language {
    const char* name;
    DgcKernelLanguage kernel;
    CullKernelLanguage cull;
    const char* vs;
    const char* fs;
};

std::vector<Language> languages() {
    std::vector<Language> out;
#if defined(FUSE_RP_DGC_SLANG_VS) && defined(FUSE_RP_DGC_SLANG_FS)
    out.push_back({"slang", DgcKernelLanguage::Slang, CullKernelLanguage::Slang, FUSE_RP_DGC_SLANG_VS, FUSE_RP_DGC_SLANG_FS});
#endif
#if defined(FUSE_RP_DGC_GLSL_VS) && defined(FUSE_RP_DGC_GLSL_FS)
    out.push_back({"glsl", DgcKernelLanguage::Glsl, CullKernelLanguage::Glsl, FUSE_RP_DGC_GLSL_VS, FUSE_RP_DGC_GLSL_FS});
#endif
    return out;
}

// --- scene ---------------------------------------------------------------------------------------
struct SceneData {
    std::vector<InstanceHandle> handles;
    std::vector<u32> materialOfSlot;
    std::vector<u32> materialBuckets;
};

void buildScene(GpuScene& scene, SceneData& sd, u32 seed) {
    GpuMesh cube{};
    cube.boundsRadius = std::sqrt(3.f);
    cube.triangleCount = 12;
    scene.addMesh(cube);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    for (u32 m = 0; m < kMaterials; ++m) {
        // Two materials map outside the bucket range: they take the default bucket.
        sd.materialBuckets.push_back(m % 5u == 4u ? kBuckets + 3u : (m * 7u + 1u) % kBuckets);
    }
    for (u32 i = 0; i < kInstances; ++i) {
        InstanceDesc d{};
        d.mesh = 0;
        d.material = i % 97u == 5u ? kMaterials + 4u : static_cast<u32>(rng() % kMaterials); // some unknown rows
        const f32 s = 0.1f + 0.3f * (u(rng) * 0.5f + 0.5f);
        d.transform = boxTransform(u(rng) * 30.f, u(rng) * 14.f, -4.f - 70.f * (u(rng) * 0.5f + 0.5f), s, s * 1.3f, s);
        sd.handles.push_back(scene.addInstance(d));
        if (sd.materialOfSlot.size() <= sd.handles.back().slot) {
            sd.materialOfSlot.resize(sd.handles.back().slot + 1u, 0xFFFFFFFFu);
        }
        sd.materialOfSlot[sd.handles.back().slot] = d.material;
    }
}

// --- frame graph ---------------------------------------------------------------------------------
struct DrawRecord {
    Context* ctx = nullptr;
    const DgcPipelineSelector* selector = nullptr; ///< null: the WP-1.3 single-pipeline draw
    const InstanceCuller* culler = nullptr;
    VkPipeline single = VK_NULL_HANDLE;
    rg::TextureRef depth;
    rg::TextureRef id;
    struct {
        f32 viewProj[16];
        u32 scene;
        u32 pad[3];
    } push{};
};

void recordDraw(const rg::PassContext& pc, void* user) {
    const DrawRecord& d = *static_cast<const DrawRecord*>(user);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(pc.commandBuffer);
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = static_cast<VkImageView>(pc.imageView(d.id));
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color.uint32[0] = 0xFFFFFFFFu;
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = static_cast<VkImageView>(pc.imageView(d.depth));
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil.depth = 1.f;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = {kWidth, kHeight};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &rendering);
    // State the generated draws inherit: descriptors, push constants, viewport / scissor, buffers.
    if (d.selector == nullptr) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.single);
    }
    d.ctx->bindless.bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.ctx->rasterLayout, 0);
    const VkViewport viewport{0.f, 0.f, static_cast<f32>(kWidth), static_cast<f32>(kHeight), 0.f, 1.f};
    const VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    const VkBuffer vb = static_cast<VkBuffer>(d.ctx->vertices.handle);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(cmd, static_cast<VkBuffer>(d.ctx->indices.handle), 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cmd, d.ctx->rasterLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(d.push), &d.push);
    if (d.selector != nullptr) {
        d.selector->recordDraws(cmd);
    } else {
        d.culler->recordDraws(cmd, CullPhase::Phase1);
    }
    vkCmdEndRendering(cmd);
}

struct CopyRecord {
    bool image = false;
    bool depth = false;
    rg::TextureRef src;
    rg::BufferRef buffer;
    rg::BufferRef dst;
    u64 dstOffset = 0;
    u64 bytes = 0;
};

void recordCopy(const rg::PassContext& pc, void* user) {
    const CopyRecord& c = *static_cast<const CopyRecord*>(user);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(pc.commandBuffer);
    const VkBuffer dst = static_cast<VkBuffer>(pc.buffer(c.dst));
    if (!c.image) {
        const VkBufferCopy region{0, c.dstOffset, c.bytes};
        vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(pc.buffer(c.buffer)), dst, 1, &region);
        return;
    }
    VkBufferImageCopy region{};
    region.bufferOffset = c.dstOffset;
    region.imageSubresource = {c.depth ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT)
                                       : static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
                               0, 0, 1};
    region.imageExtent = {kWidth, kHeight, 1};
    vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(c.src)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
}

constexpr u64 kImageBytes = static_cast<u64>(kWidth) * kHeight * 4u;

struct Layout {
    u64 id[3] = {};
    u64 depth[3] = {};
    u64 cullArgs = 0, cullCounts = 0, seqA = 0, countsA = 0, bucketArgsB = 0, countsB = 0, end = 0;
};

struct FrameState {
    DrawRecord draws[3];
    CopyRecord copies[16];
    u32 copyCount = 0;
    Layout layout{};
};

rg::TextureRef importTarget(rg::Graph& graph, Texture& image, u32& layout, u8& queue, u32 format, const char* name) {
    rg::ImportedImage i{};
    i.image = image.image;
    i.view = image.view;
    i.format = format;
    i.width = kWidth;
    i.height = kHeight;
    i.initialLayout = layout;
    i.initialQueue = queue;
    i.layoutTracker = &layout;
    i.queueTracker = &queue;
    i.name = name;
    return graph.importImage(i);
}

struct Rig {
    Language lang{};
    InstanceCuller culler;
    DgcPipelineSelector a;
    DgcPipelineSelector b;
    Pipelines pipelines;
};

bool initRig(Context& ctx, Rig& rig, const Language& lang, const SceneData& sd, bool dgcChain) {
    rig.lang = lang;
    InstanceCullerDesc cdesc{};
    cdesc.device = ctx.device.get();
    cdesc.allocator = ctx.allocator.get();
    cdesc.bindless = &ctx.bindless;
    cdesc.instanceCapacity = kInstances + 64u;
    cdesc.language = lang.cull;
    if (!rig.culler.init(cdesc) || !rig.culler.setResolution(kWidth, kHeight)) {
        std::fprintf(stderr, "FAIL: culler init\n");
        return false;
    }
    DgcSelectorDesc desc{};
    desc.device = ctx.device.get();
    desc.allocator = ctx.allocator.get();
    desc.bindless = &ctx.bindless;
    desc.maxDraws = rig.culler.capacity();
    desc.bucketCount = kBuckets;
    desc.defaultBucket = 2u;
    desc.language = lang.kernel;
    desc.enabledFeatureChain = dgcChain ? &ctx.chain.core : nullptr;
    desc.writeBothStreams = true; // the gate compares both streams
    desc.name = "dgc.a";
    if (!rig.a.init(desc)) {
        std::fprintf(stderr, "FAIL: selector A init (%s)\n", rig.a.reasonText());
        return false;
    }
    desc.mode = DgcMode::ForceFallback;
    desc.name = "dgc.b";
    if (!rig.b.init(desc)) {
        std::fprintf(stderr, "FAIL: selector B init\n");
        return false;
    }
    if (!createPipelines(ctx, rig.pipelines, lang.vs, lang.fs, rig.a.pipelineCreateFlags2())) {
        std::fprintf(stderr, "FAIL: bucket pipelines\n");
        return false;
    }
    void* pipes[kBuckets];
    for (u32 i = 0; i < kBuckets; ++i) {
        pipes[i] = rig.pipelines.buckets[i];
    }
    return rig.a.setPipelines(pipes, kBuckets, ctx.rasterLayout) && rig.b.setPipelines(pipes, kBuckets, ctx.rasterLayout) &&
           rig.a.setMaterialBuckets(sd.materialBuckets.data(), kMaterials) &&
           rig.b.setMaterialBuckets(sd.materialBuckets.data(), kMaterials);
}

void buildFrame(Context& ctx, GpuScene& scene, Rig& rig, rg::Graph& graph, const Mat4& viewProj, bool readback,
                FrameState& fs) {
    graph.reset();
    fs.copyCount = 0;
    const GpuSceneGraphRefs sceneRefs = scene.importInto(graph);
    const CullGraphRefs cull = rig.culler.importInto(graph);
    const DgcGraphRefs refsA = rig.a.importInto(graph);
    const DgcGraphRefs refsB = rig.b.importInto(graph);
    rg::TextureRef depth[3];
    rg::TextureRef id[3];
    const char* names[3][2] = {{"a.depth", "a.id"}, {"b.depth", "b.id"}, {"ref.depth", "ref.id"}};
    for (u32 t = 0; t < 3u; ++t) {
        Target& target = ctx.targets[t];
        depth[t] = importTarget(graph, target.depth, target.depthLayout, target.depthQueue, VK_FORMAT_D32_SFLOAT,
                                names[t][0]);
        id[t] = importTarget(graph, target.id, target.idLayout, target.idQueue, kFormatR32Uint, names[t][1]);
    }
    const rg::BufferRef vb = graph.importBuffer(rg::ImportedBuffer{ctx.vertices.handle, ctx.vertices.desc.size,
                                                                   rg::kNoQueue, nullptr, "cube.vertices"});
    const rg::BufferRef ib = graph.importBuffer(rg::ImportedBuffer{ctx.indices.handle, ctx.indices.desc.size,
                                                                   rg::kNoQueue, nullptr, "cube.indices"});
    rig.culler.addPhase1(graph, cull, sceneRefs, scene.headerHandle());
    const DgcCullInput input = dgc_cull_input(rig.culler, cull, CullPhase::Phase1);
    rig.a.addGenerate(graph, refsA, input, sceneRefs, scene.headerAddress());
    rig.b.addGenerate(graph, refsB, input, sceneRefs, scene.headerAddress());
    const char* drawNames[3] = {"draw.dgc_a", "draw.dgc_b", "draw.ref"};
    for (u32 t = 0; t < 3u; ++t) {
        DrawRecord& d = fs.draws[t];
        d = DrawRecord{};
        d.ctx = &ctx;
        d.selector = t == 0u ? &rig.a : t == 1u ? &rig.b : nullptr;
        d.culler = &rig.culler;
        d.single = rig.pipelines.buckets[0];
        d.depth = depth[t];
        d.id = id[t];
        std::memcpy(d.push.viewProj, viewProj.m, sizeof(d.push.viewProj));
        d.push.scene = scene.headerHandle();
        rg::PassBuilder pass = graph.addPass(drawNames[t], &recordDraw, &d);
        if (t == 0u) {
            rig.a.useDraws(pass, refsA);
        } else if (t == 1u) {
            rig.b.useDraws(pass, refsB);
        } else {
            rig.culler.useDraws(pass, cull, CullPhase::Phase1);
        }
        GpuScene::useAll(pass, sceneRefs, rg::Access::StorageRead, rg::kStageVertex);
        pass.use(vb, rg::Access::VertexRead).use(ib, rg::Access::IndexRead);
        pass.use(depth[t], rg::Access::DepthAttachmentWrite).use(id[t], rg::Access::ColorAttachmentWrite);
    }
    if (!readback) {
        return;
    }
    Layout& l = fs.layout;
    u64 at = 0;
    for (u32 t = 0; t < 3u; ++t) {
        l.id[t] = at;
        at += kImageBytes;
        l.depth[t] = at;
        at += kImageBytes;
    }
    const u64 argsBytes = static_cast<u64>(rig.culler.capacity()) * sizeof(DrawIndexedIndirectCommand);
    l.cullArgs = at;
    at += argsBytes;
    l.cullCounts = at;
    at += kCountWords * 4u;
    l.seqA = at;
    at += rig.a.sequencesBytes();
    l.countsA = at;
    at += rig.a.countWords() * 4u;
    l.bucketArgsB = at;
    at += rig.b.bucketArgsBytes();
    l.countsB = at;
    at += rig.b.countWords() * 4u;
    l.end = at;
    const rg::BufferRef rb =
        graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, kReadbackBytes, rg::kNoQueue, nullptr, "rp_dgc.readback"});
    auto addCopy = [&](const char* name, bool image, bool isDepth, rg::TextureRef src, rg::BufferRef buffer, u64 dstOffset,
                       u64 bytes) {
        CopyRecord& c = fs.copies[fs.copyCount++];
        c = CopyRecord{image, isDepth, src, buffer, rb, dstOffset, bytes};
        rg::PassBuilder pass = graph.addPass(name, &recordCopy, &c);
        if (image) {
            pass.use(src, rg::Access::TransferSrc);
        } else {
            pass.use(buffer, rg::Access::TransferSrc, rg::BufferRange{0, bytes});
        }
        pass.use(rb, rg::Access::TransferDst, rg::BufferRange{dstOffset, image ? kImageBytes : bytes});
    };
    for (u32 t = 0; t < 3u; ++t) {
        addCopy("readback.id", true, false, id[t], {}, l.id[t], 0);
        addCopy("readback.depth", true, true, depth[t], {}, l.depth[t], 0);
    }
    addCopy("readback.cull_args", false, false, {}, cull.args, l.cullArgs, argsBytes);
    addCopy("readback.cull_counts", false, false, {}, cull.counts, l.cullCounts, kCountWords * 4u);
    addCopy("readback.seq_a", false, false, {}, refsA.sequences, l.seqA, rig.a.sequencesBytes());
    addCopy("readback.counts_a", false, false, {}, refsA.counts, l.countsA, rig.a.countWords() * 4u);
    addCopy("readback.bucket_args_b", false, false, {}, refsB.bucketArgs, l.bucketArgsB, rig.b.bucketArgsBytes());
    addCopy("readback.counts_b", false, false, {}, refsB.counts, l.countsB, rig.b.countWords() * 4u);
    graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
}

void beginSceneFrame(Context& ctx, GpuScene& scene) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    scene.beginFrame(ctx.serial);
}

bool beginSelectors(Context& ctx, GpuScene& scene, Rig& rig, const Mat4& viewProj) {
    CullFrameDesc frame{};
    std::memcpy(frame.viewProj, viewProj.m, sizeof(frame.viewProj));
    frame.instanceCount = scene.instanceHighWater();
    frame.frustum = true;
    frame.occlusion = false;
    return rig.culler.beginFrame(ctx.serial, frame) && rig.a.beginFrame(ctx.serial) && rig.b.beginFrame(ctx.serial);
}

GpuSceneDesc gpuDesc(Context& ctx) {
    GpuSceneDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.bindless = &ctx.bindless;
    d.instanceCapacity = kInstances + 64u;
    return d;
}

Mat4 frameCamera(u32 frame) {
    const f32 t = static_cast<f32>(frame);
    return camera(-4.f + 1.3f * t, 1.f - 0.4f * t, 8.f - 2.f * t, 0.5f * t, 0.f, -40.f);
}

const u8* rbAt(Context& ctx, u64 offset) { return static_cast<const u8*>(ctx.readback.mapped) + offset; }

struct FrameCheck {
    u32 draws = 0;
    u32 seqCount = 0;
    std::vector<u32> sequenceWords;
    u32 pixelsCovered = 0;
    u32 bucketHistogram[kBuckets] = {};
};

/// Every comparison of one frame (see the file comment).
void checkFrame(Context& ctx, Rig& rig, const SceneData& sd, const FrameState& fs, FrameCheck& out) {
    const Layout& l = fs.layout;
    const u32 n = reinterpret_cast<const u32*>(rbAt(ctx, l.cullCounts))[kCountPhase1Draws];
    out.draws = n;
    const DgcDrawIndexed* args = reinterpret_cast<const DgcDrawIndexed*>(rbAt(ctx, l.cullArgs));
    DgcReferenceInput in{};
    in.draws = args;
    in.drawCount = n;
    in.maxDraws = rig.a.maxDraws();
    in.instanceMaterials = sd.materialOfSlot.data();
    in.instanceCount = static_cast<u32>(sd.materialOfSlot.size());
    in.materialBuckets = sd.materialBuckets.data();
    in.materialCount = kMaterials;
    in.bucketCount = kBuckets;
    in.defaultBucket = 2u;
    DgcReferenceOutput ref;
    dgc_generate_reference(in, ref);
    // (1) GPU sequences == CPU reference, word for word.
    const u32* countsA = reinterpret_cast<const u32*>(rbAt(ctx, l.countsA));
    out.seqCount = countsA[kDgcCountSequences];
    expect(out.seqCount == ref.sequences.size(), "GPU sequence count == reference");
    const DgcSequence* seq = reinterpret_cast<const DgcSequence*>(rbAt(ctx, l.seqA));
    const bool seqEqual =
        out.seqCount == ref.sequences.size() && std::memcmp(seq, ref.sequences.data(), ref.sequences.size() * sizeof(DgcSequence)) == 0;
    expect(seqEqual, "GPU DGC sequences == CPU reference (word for word)");
    // Cross-language comparison: the culler compacts its args with atomics, so the draw (and hence
    // sequence) order differs between runs; compare the sorted sequences.
    std::vector<DgcSequence> sorted(seq, seq + out.seqCount);
    std::sort(sorted.begin(), sorted.end(), [](const DgcSequence& x, const DgcSequence& y) {
        return std::memcmp(&x, &y, sizeof(DgcSequence)) < 0;
    });
    out.sequenceWords.assign(reinterpret_cast<const u32*>(sorted.data()),
                             reinterpret_cast<const u32*>(sorted.data() + sorted.size()));
    // (2) GPU bucket args (fallback) == reference per bucket (order inside a bucket is atomic).
    const u32* countsB = reinterpret_cast<const u32*>(rbAt(ctx, l.countsB));
    std::vector<std::vector<DgcDrawIndexed>> gpuBuckets(kBuckets);
    u32 gpuBucketCounts[kBuckets] = {};
    for (u32 b = 0; b < kBuckets; ++b) {
        gpuBucketCounts[b] = countsB[kDgcCountBucketBase + b];
        expect(gpuBucketCounts[b] == ref.bucketArgs[b].size(), "GPU bucket count == reference");
        const DgcDrawIndexed* bargs =
            reinterpret_cast<const DgcDrawIndexed*>(rbAt(ctx, l.bucketArgsB)) + static_cast<u64>(b) * rig.b.maxDraws();
        gpuBuckets[b].assign(bargs, bargs + std::min(gpuBucketCounts[b], rig.b.maxDraws()));
        out.bucketHistogram[b] = gpuBucketCounts[b];
    }
    // (3) CPU emulation of the indirect-commands layout over the GPU sequences == the indirect-count
    //     path's draw list (GPU bucket args) and == the reference.
    std::vector<DgcCommand> emulated;
    expect(dgc_emulate_layout(kDgcLayoutTokens, kDgcLayoutTokenCount, kDgcSequenceStride, seq, rig.a.sequencesBytes(),
                              out.seqCount, rig.a.maxDraws(), 0u, emulated),
           "emulate the GPU stream");
    std::vector<DgcCommand> indirectCount;
    dgc_indirect_count_commands(gpuBuckets, gpuBucketCounts, indirectCount);
    expect(dgc_same_draws(emulated, indirectCount), "emulated DGC command stream == indirect-count draw list");
    std::vector<u32> refCounts;
    for (const auto& b : ref.bucketArgs) {
        refCounts.push_back(static_cast<u32>(b.size()));
    }
    std::vector<DgcCommand> refStream;
    dgc_indirect_count_commands(ref.bucketArgs, refCounts.data(), refStream);
    expect(dgc_same_draws(indirectCount, refStream), "GPU indirect-count draw list == CPU reference");
    // (4) Images: A == B bit exact; both == the WP-1.3 image with each slot's bucket.
    const u32* idA = reinterpret_cast<const u32*>(rbAt(ctx, l.id[0]));
    const u32* idB = reinterpret_cast<const u32*>(rbAt(ctx, l.id[1]));
    const u32* idR = reinterpret_cast<const u32*>(rbAt(ctx, l.id[2]));
    expect(std::memcmp(idA, idB, kImageBytes) == 0, "ID image: A (DGC / Auto) == B (indirect-count fallback), bit exact");
    expect(std::memcmp(rbAt(ctx, l.depth[0]), rbAt(ctx, l.depth[1]), kImageBytes) == 0, "depth: A == B, bit exact");
    expect(std::memcmp(rbAt(ctx, l.depth[0]), rbAt(ctx, l.depth[2]), kImageBytes) == 0,
           "depth: A == WP-1.3 indirect-count draw, bit exact");
    u32 mismatches = 0;
    for (u32 p = 0; p < kWidth * kHeight; ++p) {
        if (idR[p] == 0xFFFFFFFFu) {
            mismatches += idA[p] != 0xFFFFFFFFu ? 1u : 0u;
            continue;
        }
        ++out.pixelsCovered;
        const u32 slot = idR[p] >> 6u;
        const u32 material = slot < sd.materialOfSlot.size() ? sd.materialOfSlot[slot] : 0xFFFFFFFFu;
        const u32 bucket = dgc_select_bucket(material, sd.materialBuckets.data(), kMaterials, kBuckets, 2u);
        mismatches += idA[p] != ((slot << 6u) | bucket) ? 1u : 0u;
    }
    if (mismatches != 0u) {
        std::fprintf(stderr, "FAIL: %u pixels differ from the WP-1.3 image with bucket colours\n", mismatches);
        ++g_failures;
    }
}

/// Runs the frames for every language; `expectDgc` = selector A must have DGC active.
int runFrames(Context& ctx, bool expectDgc, bool dgcChain, u32 frames) {
    const std::vector<Language> langs = languages();
    if (langs.empty()) {
        std::printf("SKIP: test raster shaders not built\n");
        return kSkip;
    }
    std::vector<std::vector<std::vector<u32>>> perLanguage;
    for (const Language& lang : langs) {
        GpuScene scene;
        if (!scene.init(gpuDesc(ctx)) || !scene.gpuEnabled()) {
            std::fprintf(stderr, "FAIL: GpuScene GPU init\n");
            return 1;
        }
        SceneData sd;
        Rig rig;
        if (!initRig(ctx, rig, lang, sd, dgcChain)) {
            return 1;
        }
        std::printf("  language %s: generate kernel %s, selector A: %s (%s)\n", lang.name, rig.a.kernelLanguage(),
                    rig.a.dgcActive() ? "DGC" : "fallback", rig.a.reasonText());
        if (std::strcmp(rig.a.kernelLanguage(), lang.name) != 0) {
            std::printf("  (kernel %s not built, skipped)\n", lang.name);
            continue;
        }
        expect(rig.a.dgcActive() == expectDgc, expectDgc ? "selector A runs DGC" : "selector A falls back");
        expect(!rig.b.dgcActive() && rig.b.reason() == DgcReason::ForcedFallback, "selector B: forced fallback");
        rg::Graph graph;
        FrameState fs;
        std::vector<std::vector<u32>> seqs;
        std::printf("  %5s %6s %6s %8s %9s %s\n", "frame", "draws", "seqs", "pixels", "cmds A/B", "bucket histogram");
        for (u32 frame = 0; frame < frames; ++frame) {
            beginSceneFrame(ctx, scene);
            if (frame == 0) {
                buildScene(scene, sd, 4242u);
                rig.a.setMaterialBuckets(sd.materialBuckets.data(), kMaterials);
                rig.b.setMaterialBuckets(sd.materialBuckets.data(), kMaterials);
            }
            const GpuSceneCommitStats stats = scene.commit();
            ctx.upload.flush();
            const Mat4 vp = frameCamera(frame);
            if (!beginSelectors(ctx, scene, rig, vp)) {
                std::fprintf(stderr, "FAIL: beginFrame\n");
                return 1;
            }
            buildFrame(ctx, scene, rig, graph, vp, true, fs);
            const rg::ExecuteResult result = ctx.executor->execute(graph);
            const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
            rig.culler.collectRetired(ctx.serial);
            scene.collectRetired(ctx.serial);
            expect(stats.ok && result.ok && waited, "frame executed");
            FrameCheck fc;
            checkFrame(ctx, rig, sd, fs, fc);
            seqs.push_back(fc.sequenceWords);
            const u32 cmdA = rig.a.stats().recordedCommands;
            const u32 cmdB = rig.b.stats().recordedCommands;
            expect(cmdA == dgc_recorded_commands(rig.a.dgcActive(), kBuckets), "A command count matches the model");
            expect(cmdB == 2u * kBuckets, "B records 2 x buckets commands");
            if (expectDgc) {
                expect(cmdA < cmdB, "DGC records fewer CPU commands than the indirect-count path");
            }
            expect(fc.draws > 100u && fc.draws < kInstances, "frustum culling leaves a partial set");
            std::printf("  %5u %6u %6u %8u %4u/%-4u", frame, fc.draws, fc.seqCount, fc.pixelsCovered, cmdA, cmdB);
            for (u32 b = 0; b < kBuckets; ++b) {
                std::printf(" %u", fc.bucketHistogram[b]);
            }
            std::printf("\n");
        }
        std::printf("  A: %u DGC executes, preprocess %llu B; B: %u indirect-count draws\n", rig.a.stats().executes,
                    static_cast<unsigned long long>(rig.a.stats().preprocessBytes), rig.b.stats().indirectCountDraws);
        perLanguage.push_back(seqs);
        ctx.device->waitIdle();
        rig.a.destroy();
        rig.b.destroy();
        rig.culler.destroy();
        scene.destroy();
    }
    if (perLanguage.size() == 2u) {
        expect(perLanguage[0] == perLanguage[1], "Slang == GLSL: identical sequence sets on every frame");
        std::printf("  Slang == GLSL sequence sets on %zu/%zu frames\n", perLanguage[0].size(), perLanguage[0].size());
    }
    return 0;
}

int runFallbackReasons(bool validation) {
    int rc = 0;
    // 1. Standard device: the local headers cannot enable DGC.
    {
        Context ctx;
        const int s = setup(ctx, DeviceKind::Standard, false, validation);
        if (s != 0) {
            return s;
        }
        DgcPipelineSelector sel;
        DgcSelectorDesc desc{};
        desc.device = ctx.device.get();
        desc.allocator = ctx.allocator.get();
        desc.bindless = &ctx.bindless;
        desc.bucketCount = kBuckets;
        expect(sel.init(desc), "fallback selector init");
        expect(!sel.dgcCapable() && sel.pipelineCreateFlags2() == 0u, "standard device: no DGC");
        expect(sel.reason() == DgcReason::ExtensionNotEnabled || sel.reason() == DgcReason::FeatureStateUnknown ||
                   sel.reason() == DgcReason::Maintenance5NotEnabled,
               "standard device reason");
        std::printf("  standard device: '%s'\n", sel.reasonText());
        desc.mode = DgcMode::ForceFallback;
        DgcPipelineSelector forced;
        expect(forced.init(desc) && forced.reason() == DgcReason::ForcedFallback, "ForceFallback reason");
        std::printf("  ForceFallback: '%s'\n", forced.reasonText());
        rc |= runFrames(ctx, false, false, 2u);
    }
    // 2. Adopted DGC device without the feature chain: the feature state cannot be read back.
    {
        Context ctx;
        const int s = setup(ctx, DeviceKind::Dgc, false, false);
        if (s == 0) {
            DgcPipelineSelector sel;
            DgcSelectorDesc desc{};
            desc.device = ctx.device.get();
            desc.allocator = ctx.allocator.get();
            desc.bindless = &ctx.bindless;
            desc.bucketCount = kBuckets;
            expect(sel.init(desc) && sel.reason() == DgcReason::FeatureStateUnknown, "no chain -> FeatureStateUnknown");
            std::printf("  DGC device, no feature chain: '%s'\n", sel.reasonText());
            desc.enabledFeatureChain = &ctx.chain.core;
            DgcPipelineSelector with;
            expect(with.init(desc) && with.dgcCapable(), "with the chain -> capable");
            std::printf("  DGC device, feature chain: '%s'\n", with.reasonText());
            desc.bucketCount = 65u;
            DgcPipelineSelector tooMany;
            expect(!tooMany.init(desc) && tooMany.reason() == DgcReason::BadBucketCount, "65 buckets rejected");
        } else if (s != kSkip) {
            rc |= s;
        }
    }
    return rc;
}

// --- zero_alloc -----------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && (std::strncmp(name, "dgc.", 4) == 0 || std::strncmp(name, "draw.dgc", 8) == 0)) {
        t_inPass = true;
        t_count = true;
    }
}

void hookEnd(const rg::PassContext&, const char*, void*) {
    if (t_inPass) {
        t_inPass = false;
        t_count = false;
    }
}

int runZeroAlloc(Context& ctx, bool dgcChain) {
    const std::vector<Language> langs = languages();
    if (langs.empty()) {
        std::printf("SKIP: test raster shaders not built\n");
        return kSkip;
    }
    GpuScene scene;
    SceneData sd;
    Rig rig;
    Language lang = langs.front();
    lang.kernel = DgcKernelLanguage::Auto;
    lang.cull = CullKernelLanguage::Auto;
    if (!scene.init(gpuDesc(ctx)) || !initRig(ctx, rig, lang, sd, dgcChain)) {
        std::fprintf(stderr, "FAIL: init\n");
        return 1;
    }
    rg::Graph graph;
    FrameState fs;
    constexpr u32 kWarmup = 8;
    constexpr u32 kFrames = 40;
    unsigned long long selectorCalls = 0, callbacks = 0;
    ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    for (u32 frame = 0; frame < kFrames; ++frame) {
        const bool measure = frame >= kWarmup;
        beginSceneFrame(ctx, scene);
        if (frame == 0) {
            buildScene(scene, sd, 7u);
            rig.a.setMaterialBuckets(sd.materialBuckets.data(), kMaterials);
            rig.b.setMaterialBuckets(sd.materialBuckets.data(), kMaterials);
        }
        scene.commit();
        ctx.upload.flush();
        const Mat4 vp = frameCamera(frame % 4u);
        CullFrameDesc cf{};
        std::memcpy(cf.viewProj, vp.m, sizeof(cf.viewProj));
        cf.instanceCount = scene.instanceHighWater();
        cf.occlusion = false;
        rig.culler.beginFrame(ctx.serial, cf);
        // Selector calls: beginFrame + importInto + addGenerate + useDraws (inside buildFrame; the
        // graph's own storage is warm after the first frames).
        t_allocations = 0;
        t_count = measure;
        rig.a.beginFrame(ctx.serial);
        rig.b.beginFrame(ctx.serial);
        buildFrame(ctx, scene, rig, graph, vp, false, fs);
        t_count = false;
        const unsigned long long build = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = measure ? t_allocations : 0u;
        t_count = false;
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        rig.culler.collectRetired(ctx.serial);
        scene.collectRetired(ctx.serial);
        expect(result.ok && waited, "frame ok");
        if (measure) {
            selectorCalls += build;
            callbacks += inCallbacks;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    std::printf("zero_alloc (validation off: the layer allocates through operator new), selector A %s:\n"
                "  %u steady-state frames: graph build incl. selector calls %llu, dgc.* + draw.dgc_* callbacks %llu "
                "operator-new calls\n",
                rig.a.dgcActive() ? "DGC" : "fallback", kFrames - kWarmup, selectorCalls, callbacks);
    expect(selectorCalls == 0u, "selector calls + graph build make no steady-state heap allocations");
    expect(callbacks == 0u, "dgc.* / draw callbacks make no steady-state heap allocations");
    ctx.device->waitIdle();
    rig.a.destroy();
    rig.b.destroy();
    rig.culler.destroy();
    scene.destroy();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "parity";
    std::string backend = "set";
    bool forceValidate = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if (std::strcmp(argv[i], "--validate") == 0) {
            forceValidate = true;
        }
    }
    const bool buffer = backend == "buffer";
    int rc = 0;
    if (mode == "parity") {
        Context ctx;
        const int s = setup(ctx, DeviceKind::Standard, buffer, true);
        if (s != 0) {
            return s;
        }
        rc = runFrames(ctx, false, false, 4u);
    } else if (mode == "dgc") {
        // The 1.3.275 validation layer predates VK_EXT_device_generated_commands (it cannot parse its
        // structs / flags): unvalidated unless --validate.
        Context ctx;
        const int s = setup(ctx, DeviceKind::Dgc, buffer, forceValidate);
        if (s != 0) {
            return s;
        }
        rc = runFrames(ctx, true, true, 4u);
    } else if (mode == "fallback") {
        rc = runFallbackReasons(true);
    } else if (mode == "zero_alloc") {
        {
            Context ctx; // validated pass over the same frames first (fallback path)
            const int s = setup(ctx, DeviceKind::Standard, buffer, true);
            if (s != 0) {
                return s;
            }
            rc = runFrames(ctx, false, false, 2u);
        }
        const u32 validatedMessages = g_messages;
        for (DeviceKind kind : {DeviceKind::Standard, DeviceKind::Dgc}) {
            Context ctx;
            const int s = setup(ctx, kind, buffer, false);
            if (s == kSkip) {
                continue;
            }
            if (s != 0) {
                return s;
            }
            rc |= runZeroAlloc(ctx, kind == DeviceKind::Dgc);
        }
        g_messages = validatedMessages;
    } else {
        std::fprintf(stderr, "unknown --mode %s\n", mode.c_str());
        return 2;
    }
    if (rc == kSkip) {
        return kSkip;
    }
    std::printf("validation messages: %u\n", g_messages);
    if (rc != 0 || g_failures != 0 || g_messages != 0u) {
        std::fprintf(stderr, "FAIL: %d failure(s), %u validation message(s)\n", g_failures + rc, g_messages);
        return 1;
    }
    std::printf("PASS %s (%s)\n", mode.c_str(), backend.c_str());
    return 0;
}

#endif
