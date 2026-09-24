// GPU-shader reference gate for the single-source spatial upscaler ports.
//
// The vendored GLSL passes (Engine/lib/fidelityfx/shaders/vk/{fsr1,cas}, Engine/lib/nvidia-nis/NIS/NIS_Main.glsl)
// are compiled with glslangValidator at build time (Renderer/cmake/upscale.cmake) and dispatched here on a Vulkan
// device (Lavapipe in CI) through a small compute harness that opens the loader at run time (no link dependency,
// so the gate runs in stub builds too). Each pass gets the same input image and the same constant-buffer bits as
// the CPU kernel (upscale_passes.hpp computes them like the SDK host helpers), and the outputs are compared:
//
//   pass            GLSL permutation                                    compared region / tolerance
//   EASU            ffx_fsr1_easu_pass, FFX_HALF=0, APPLY_RCAS=0         every pixel (gathers clamp)       abs 2e-3
//   RCAS            ffx_fsr1_rcas_pass, FFX_HALF=0 (FSR_RCAS_DENOISE)    interior (texelFetch OOB is UB)   abs 2e-3
//   CAS             ffx_cas_sharpen_pass, SHARPEN_ONLY=1, rgba16 unorm   interior, after unorm16 rounding  abs 2e-3
//   NIS NVScaler    NIS_Main.glsl, NIS_SCALER=1, fp32, SDR               every pixel                       abs 2e-3
//                   (power-of-two sources; non-power-of-two sources are bounded statistically because the
//                   shader's "exact" texel loads go through a linear sampler whose sub-texel quantization
//                   blends in up to 1/256 of a neighbour when 1 / width is inexact — see kSamplerBoundMean)
//
// plus mean-error bounds (the approximations the ports reproduce are bit tricks, so outputs agree to float
// rounding; GPU FMA contraction and sampler arithmetic account for the residual). Several sizes per pass, with
// partial workgroups and ratios 1.5x / 1.7x / 2x / 3x. Results are written to FUSE_UPSCALE_GPU_REPORT_PATH.
// Exit 77 (SKIP) when the shaders were not built, the headers / loader are missing or no device is found.

#include <fuse/core/init.hpp>
#include <fuse/renderer/upscale/upscale_passes.hpp>

#include "upscale_test_images.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(__has_include)
#if __has_include(<vulkan/vulkan.h>) && __has_include(<NIS_Config.h>)
#define FUSE_UPSCALE_HAVE_VK_HEADERS 1
#endif
#endif

#if defined(FUSE_UPSCALE_HAVE_VK_HEADERS) && defined(FUSE_UPSCALE_SHADERS_BUILT)
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#include <NIS_Config.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::usize;
using fuse::math::Vec4;
namespace up = fuse::renderer::upscale;
namespace ut = fuse::upscale_test;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// ---- Minimal Vulkan compute harness ------------------------------------------------------------------

/// Zero-initialised Vulkan struct with its sType set.
template <typename T>
T vk_struct(VkStructureType type) {
    T value{};
    value.sType = type;
    return value;
}

#define FUSE_VK_INSTANCE_FNS(X)                                                                                        \
    X(vkDestroyInstance)                                                                                               \
    X(vkEnumeratePhysicalDevices)                                                                                      \
    X(vkGetPhysicalDeviceProperties)                                                                                   \
    X(vkGetPhysicalDeviceFeatures)                                                                                     \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                        \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                             \
    X(vkGetPhysicalDeviceFormatProperties)                                                                             \
    X(vkCreateDevice)                                                                                                  \
    X(vkGetDeviceProcAddr)

#define FUSE_VK_DEVICE_FNS(X)                                                                                          \
    X(vkDestroyDevice)                                                                                                 \
    X(vkGetDeviceQueue)                                                                                                \
    X(vkCreateBuffer)                                                                                                  \
    X(vkDestroyBuffer)                                                                                                 \
    X(vkGetBufferMemoryRequirements)                                                                                   \
    X(vkAllocateMemory)                                                                                                \
    X(vkFreeMemory)                                                                                                    \
    X(vkBindBufferMemory)                                                                                              \
    X(vkMapMemory)                                                                                                     \
    X(vkUnmapMemory)                                                                                                   \
    X(vkCreateImage)                                                                                                   \
    X(vkDestroyImage)                                                                                                  \
    X(vkGetImageMemoryRequirements)                                                                                    \
    X(vkBindImageMemory)                                                                                               \
    X(vkCreateImageView)                                                                                               \
    X(vkDestroyImageView)                                                                                              \
    X(vkCreateSampler)                                                                                                 \
    X(vkDestroySampler)                                                                                                \
    X(vkCreateShaderModule)                                                                                            \
    X(vkDestroyShaderModule)                                                                                           \
    X(vkCreateDescriptorSetLayout)                                                                                     \
    X(vkDestroyDescriptorSetLayout)                                                                                    \
    X(vkCreatePipelineLayout)                                                                                          \
    X(vkDestroyPipelineLayout)                                                                                         \
    X(vkCreateComputePipelines)                                                                                        \
    X(vkDestroyPipeline)                                                                                               \
    X(vkCreateDescriptorPool)                                                                                          \
    X(vkDestroyDescriptorPool)                                                                                         \
    X(vkResetDescriptorPool)                                                                                           \
    X(vkAllocateDescriptorSets)                                                                                        \
    X(vkUpdateDescriptorSets)                                                                                          \
    X(vkCreateCommandPool)                                                                                             \
    X(vkDestroyCommandPool)                                                                                            \
    X(vkAllocateCommandBuffers)                                                                                        \
    X(vkFreeCommandBuffers)                                                                                            \
    X(vkBeginCommandBuffer)                                                                                            \
    X(vkEndCommandBuffer)                                                                                              \
    X(vkCmdPipelineBarrier)                                                                                            \
    X(vkCmdCopyBufferToImage)                                                                                          \
    X(vkCmdCopyImageToBuffer)                                                                                          \
    X(vkCmdBindPipeline)                                                                                               \
    X(vkCmdBindDescriptorSets)                                                                                         \
    X(vkCmdDispatch)                                                                                                   \
    X(vkQueueSubmit)                                                                                                   \
    X(vkQueueWaitIdle)                                                                                                 \
    X(vkDeviceWaitIdle)

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 width = 0;
    u32 height = 0;
    u32 texel_bytes = 16;
};

struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

struct Binding {
    u32 binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    const GpuImage* image = nullptr;
    const GpuBuffer* buffer = nullptr;
};

class VulkanCompute {
public:
    ~VulkanCompute() { shutdown(); }

    /// False (with `why`) when no loader / device is available: the gate skips.
    bool init(std::string& why) {
#if defined(_WIN32)
        m_lib = static_cast<void*>(LoadLibraryA("vulkan-1.dll"));
        if (m_lib != nullptr) {
            m_getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_lib), "vkGetInstanceProcAddr")));
        }
#else
        m_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (m_lib == nullptr) {
            m_lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (m_lib != nullptr) {
            m_getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(m_lib, "vkGetInstanceProcAddr"));
        }
#endif
        if (m_getInstanceProcAddr == nullptr) {
            why = "Vulkan loader not found";
            return false;
        }
        auto createInstance =
            reinterpret_cast<PFN_vkCreateInstance>(m_getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
        auto app = vk_struct<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        app.pApplicationName = "fuse_upscale_gpu_reference";
        app.apiVersion = VK_API_VERSION_1_1;
        auto ici = vk_struct<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        ici.pApplicationInfo = &app;
        if (createInstance == nullptr || createInstance(&ici, nullptr, &m_instance) != VK_SUCCESS) {
            why = "vkCreateInstance failed";
            return false;
        }
#define FUSE_LOAD_INSTANCE(fn)                                                                                         \
    fn = reinterpret_cast<PFN_##fn>(m_getInstanceProcAddr(m_instance, #fn));                                          \
    if (fn == nullptr) {                                                                                               \
        why = "missing " #fn;                                                                                          \
        return false;                                                                                                  \
    }
        FUSE_VK_INSTANCE_FNS(FUSE_LOAD_INSTANCE)
#undef FUSE_LOAD_INSTANCE

        u32 count = 0;
        vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        if (count == 0u || vkEnumeratePhysicalDevices(m_instance, &count, devices.data()) != VK_SUCCESS) {
            why = "no Vulkan physical device";
            return false;
        }
        for (VkPhysicalDevice pd : devices) {
            u32 qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qf(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
            for (u32 i = 0; i < qn; ++i) {
                if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0u) {
                    m_physical = pd;
                    m_queueFamily = i;
                    break;
                }
            }
            if (m_physical != VK_NULL_HANDLE) {
                break;
            }
        }
        if (m_physical == VK_NULL_HANDLE) {
            why = "no compute queue";
            return false;
        }
        vkGetPhysicalDeviceProperties(m_physical, &m_props);
        vkGetPhysicalDeviceMemoryProperties(m_physical, &m_memProps);
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(m_physical, &supported);
        if (!supported.shaderStorageImageExtendedFormats || !supported.shaderStorageImageWriteWithoutFormat) {
            why = "device lacks shaderStorageImageExtendedFormats / shaderStorageImageWriteWithoutFormat";
            return false;
        }
        VkPhysicalDeviceFeatures enabled{};
        enabled.shaderStorageImageExtendedFormats = VK_TRUE;
        enabled.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        const f32 priority = 1.f;
        auto qci = vk_struct<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        qci.queueFamilyIndex = m_queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        auto dci = vk_struct<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.pEnabledFeatures = &enabled;
        if (vkCreateDevice(m_physical, &dci, nullptr, &m_device) != VK_SUCCESS) {
            why = "vkCreateDevice failed";
            return false;
        }
#define FUSE_LOAD_DEVICE(fn)                                                                                           \
    fn = reinterpret_cast<PFN_##fn>(vkGetDeviceProcAddr(m_device, #fn));                                              \
    if (fn == nullptr) {                                                                                               \
        why = "missing " #fn;                                                                                          \
        return false;                                                                                                  \
    }
        FUSE_VK_DEVICE_FNS(FUSE_LOAD_DEVICE)
#undef FUSE_LOAD_DEVICE
        vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

        auto cpci = vk_struct<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = m_queueFamily;
        VkDescriptorPoolSize sizes[4] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64},
                                         {VK_DESCRIPTOR_TYPE_SAMPLER, 64},
                                         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64}};
        auto dpci = vk_struct<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        dpci.maxSets = 16;
        dpci.poolSizeCount = 4;
        dpci.pPoolSizes = sizes;
        auto sci = vk_struct<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.f;
        if (vkCreateCommandPool(m_device, &cpci, nullptr, &m_cmdPool) != VK_SUCCESS ||
            vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_descPool) != VK_SUCCESS ||
            vkCreateSampler(m_device, &sci, nullptr, &m_sampler) != VK_SUCCESS) {
            why = "command / descriptor pool or sampler creation failed";
            return false;
        }
        auto cbai = vk_struct<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        cbai.commandPool = m_cmdPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(m_device, &cbai, &m_cmd) != VK_SUCCESS) {
            why = "command buffer allocation failed";
            return false;
        }
        return true;
    }

    const char* device_name() const { return m_props.deviceName; }

    void shutdown() {
        if (m_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device);
            for (GpuImage& i : m_images) {
                vkDestroyImageView(m_device, i.view, nullptr);
                vkDestroyImage(m_device, i.image, nullptr);
                vkFreeMemory(m_device, i.memory, nullptr);
            }
            for (GpuBuffer& b : m_buffers) {
                vkDestroyBuffer(m_device, b.buffer, nullptr);
                vkFreeMemory(m_device, b.memory, nullptr);
            }
            m_images.clear();
            m_buffers.clear();
            if (m_sampler != VK_NULL_HANDLE) {
                vkDestroySampler(m_device, m_sampler, nullptr);
            }
            if (m_descPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
            }
            if (m_cmdPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(m_device, m_cmdPool, nullptr);
            }
            vkDestroyDevice(m_device, nullptr);
            m_device = VK_NULL_HANDLE;
        }
        if (m_instance != VK_NULL_HANDLE) {
            vkDestroyInstance(m_instance, nullptr);
            m_instance = VK_NULL_HANDLE;
        }
#if defined(_WIN32)
        if (m_lib != nullptr) {
            FreeLibrary(static_cast<HMODULE>(m_lib));
        }
#else
        if (m_lib != nullptr) {
            dlclose(m_lib);
        }
#endif
        m_lib = nullptr;
    }

    /// Release every image / buffer created so far (between test cases).
    void release_resources() {
        vkDeviceWaitIdle(m_device);
        for (GpuImage& i : m_images) {
            vkDestroyImageView(m_device, i.view, nullptr);
            vkDestroyImage(m_device, i.image, nullptr);
            vkFreeMemory(m_device, i.memory, nullptr);
        }
        for (GpuBuffer& b : m_buffers) {
            vkDestroyBuffer(m_device, b.buffer, nullptr);
            vkFreeMemory(m_device, b.memory, nullptr);
        }
        m_images.clear();
        m_buffers.clear();
    }

    bool format_supports_storage(VkFormat format) const {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(m_physical, format, &props);
        return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0u &&
               (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0u;
    }

    const GpuImage* create_image(u32 width, u32 height, VkFormat format, u32 texel_bytes) {
        GpuImage img{};
        img.format = format;
        img.width = width;
        img.height = height;
        img.texel_bytes = texel_bytes;
        auto ici = vk_struct<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &ici, nullptr, &img.image) != VK_SUCCESS) {
            return nullptr;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, img.image, &req);
        img.memory = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (img.memory == VK_NULL_HANDLE || vkBindImageMemory(m_device, img.image, img.memory, 0) != VK_SUCCESS) {
            return nullptr;
        }
        auto vci = vk_struct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        vci.image = img.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(m_device, &vci, nullptr, &img.view) != VK_SUCCESS) {
            return nullptr;
        }
        m_images.push_back(img);
        // Transition to GENERAL once (sampled + storage + transfer all use GENERAL here).
        begin();
        barrier_image(img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        if (!submit()) {
            return nullptr;
        }
        return &m_images.back();
    }

    const GpuBuffer* create_buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
        GpuBuffer b{};
        b.size = size;
        auto bci = vk_struct<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        bci.size = size;
        bci.usage = usage;
        if (vkCreateBuffer(m_device, &bci, nullptr, &b.buffer) != VK_SUCCESS) {
            return nullptr;
        }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_device, b.buffer, &req);
        b.memory = allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (b.memory == VK_NULL_HANDLE || vkBindBufferMemory(m_device, b.buffer, b.memory, 0) != VK_SUCCESS) {
            return nullptr;
        }
        m_buffers.push_back(b);
        return &m_buffers.back();
    }

    bool write_buffer(const GpuBuffer& b, const void* data, usize bytes) {
        void* mapped = nullptr;
        if (bytes > b.size || vkMapMemory(m_device, b.memory, 0, b.size, 0, &mapped) != VK_SUCCESS) {
            return false;
        }
        std::memcpy(mapped, data, bytes);
        vkUnmapMemory(m_device, b.memory);
        return true;
    }

    bool read_buffer(const GpuBuffer& b, void* data, usize bytes) {
        void* mapped = nullptr;
        if (bytes > b.size || vkMapMemory(m_device, b.memory, 0, b.size, 0, &mapped) != VK_SUCCESS) {
            return false;
        }
        std::memcpy(data, mapped, bytes);
        vkUnmapMemory(m_device, b.memory);
        return true;
    }

    bool upload(const GpuImage& img, const void* texels) {
        const usize bytes = static_cast<usize>(img.width) * img.height * img.texel_bytes;
        const GpuBuffer* staging = create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if (staging == nullptr || !write_buffer(*staging, texels, bytes)) {
            return false;
        }
        begin();
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {img.width, img.height, 1};
        vkCmdCopyBufferToImage(m_cmd, staging->buffer, img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        memory_barrier();
        return submit();
    }

    bool download(const GpuImage& img, void* texels) {
        const usize bytes = static_cast<usize>(img.width) * img.height * img.texel_bytes;
        const GpuBuffer* staging = create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        if (staging == nullptr) {
            return false;
        }
        begin();
        memory_barrier();
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {img.width, img.height, 1};
        vkCmdCopyImageToBuffer(m_cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, staging->buffer, 1, &region);
        memory_barrier();
        return submit() && read_buffer(*staging, texels, bytes);
    }

    /// One dispatch of the SPIR-V compute shader at `spv_path` with `bindings` (set 0).
    bool dispatch(const std::string& spv_path, const std::vector<Binding>& bindings, u32 gx, u32 gy) {
        std::vector<u32> code;
        if (!read_spirv(spv_path, code)) {
            std::fprintf(stderr, "cannot read %s\n", spv_path.c_str());
            return false;
        }
        auto smci = vk_struct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        smci.codeSize = code.size() * sizeof(u32);
        smci.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &smci, nullptr, &module) != VK_SUCCESS) {
            return false;
        }
        std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
        for (const Binding& b : bindings) {
            VkDescriptorSetLayoutBinding lb{};
            lb.binding = b.binding;
            lb.descriptorType = b.type;
            lb.descriptorCount = 1;
            lb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            layoutBindings.push_back(lb);
        }
        auto dslci = vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        dslci.bindingCount = static_cast<u32>(layoutBindings.size());
        dslci.pBindings = layoutBindings.data();
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        bool ok = vkCreateDescriptorSetLayout(m_device, &dslci, nullptr, &dsl) == VK_SUCCESS;
        if (ok) {
            auto plci = vk_struct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
            plci.setLayoutCount = 1;
            plci.pSetLayouts = &dsl;
            ok = vkCreatePipelineLayout(m_device, &plci, nullptr, &layout) == VK_SUCCESS;
        }
        if (ok) {
            auto cpci = vk_struct<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpci.stage.module = module;
            cpci.stage.pName = "main";
            cpci.layout = layout;
            ok = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline) == VK_SUCCESS;
        }
        if (ok) {
            vkResetDescriptorPool(m_device, m_descPool, 0);
            auto dsai = vk_struct<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
            dsai.descriptorPool = m_descPool;
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts = &dsl;
            ok = vkAllocateDescriptorSets(m_device, &dsai, &set) == VK_SUCCESS;
        }
        if (ok) {
            std::vector<VkWriteDescriptorSet> writes(bindings.size());
            std::vector<VkDescriptorImageInfo> imageInfos(bindings.size());
            std::vector<VkDescriptorBufferInfo> bufferInfos(bindings.size());
            for (usize i = 0; i < bindings.size(); ++i) {
                const Binding& b = bindings[i];
                VkWriteDescriptorSet& w = writes[i];
                w = vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
                w.dstSet = set;
                w.dstBinding = b.binding;
                w.descriptorCount = 1;
                w.descriptorType = b.type;
                if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                    bufferInfos[i] = {b.buffer->buffer, 0, VK_WHOLE_SIZE};
                    w.pBufferInfo = &bufferInfos[i];
                } else if (b.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                    imageInfos[i] = {m_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
                    w.pImageInfo = &imageInfos[i];
                } else {
                    imageInfos[i] = {VK_NULL_HANDLE, b.image->view, VK_IMAGE_LAYOUT_GENERAL};
                    w.pImageInfo = &imageInfos[i];
                }
            }
            vkUpdateDescriptorSets(m_device, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
            begin();
            vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(m_cmd, gx, gy, 1);
            memory_barrier();
            ok = submit();
        }
        vkDeviceWaitIdle(m_device);
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, pipeline, nullptr);
        }
        if (layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device, layout, nullptr);
        }
        if (dsl != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device, dsl, nullptr);
        }
        vkDestroyShaderModule(m_device, module, nullptr);
        return ok;
    }

private:
    static bool read_spirv(const std::string& path, std::vector<u32>& out) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr) {
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size <= 0 || (size % 4) != 0) {
            std::fclose(f);
            return false;
        }
        out.resize(static_cast<usize>(size) / 4u);
        const usize read = std::fread(out.data(), 1, static_cast<usize>(size), f);
        std::fclose(f);
        return read == static_cast<usize>(size);
    }

    VkDeviceMemory allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags wanted) {
        for (u32 i = 0; i < m_memProps.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) != 0u &&
                (m_memProps.memoryTypes[i].propertyFlags & wanted) == wanted) {
                auto mai = vk_struct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = i;
                VkDeviceMemory memory = VK_NULL_HANDLE;
                if (vkAllocateMemory(m_device, &mai, nullptr, &memory) == VK_SUCCESS) {
                    return memory;
                }
            }
        }
        return VK_NULL_HANDLE;
    }

    void begin() {
        auto bi = vk_struct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m_cmd, &bi);
    }

    bool submit() {
        if (vkEndCommandBuffer(m_cmd) != VK_SUCCESS) {
            return false;
        }
        auto si = vk_struct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        si.commandBufferCount = 1;
        si.pCommandBuffers = &m_cmd;
        return vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(m_queue) == VK_SUCCESS;
    }

    void memory_barrier() {
        auto mb = vk_struct<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
    }

    void barrier_image(VkImage image, VkImageLayout from, VkImageLayout to) {
        auto ib = vk_struct<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        ib.srcAccessMask = 0;
        ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        ib.oldLayout = from;
        ib.newLayout = to;
        ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = image;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &ib);
    }

    void* m_lib = nullptr;
    PFN_vkGetInstanceProcAddr m_getInstanceProcAddr = nullptr;
#define FUSE_DECLARE_FN(fn) PFN_##fn fn = nullptr;
    FUSE_VK_INSTANCE_FNS(FUSE_DECLARE_FN)
    FUSE_VK_DEVICE_FNS(FUSE_DECLARE_FN)
#undef FUSE_DECLARE_FN
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties m_props{};
    VkPhysicalDeviceMemoryProperties m_memProps{};
    u32 m_queueFamily = 0;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    // std::deque-like stability: reserve so pointers handed out stay valid.
    std::vector<GpuImage> m_images = [] {
        std::vector<GpuImage> v;
        v.reserve(256);
        return v;
    }();
    std::vector<GpuBuffer> m_buffers = [] {
        std::vector<GpuBuffer> v;
        v.reserve(256);
        return v;
    }();
};

// ---- Comparison ---------------------------------------------------------------------------------------

struct Comparison {
    std::string name;
    u32 src_w = 0;
    u32 src_h = 0;
    u32 dst_w = 0;
    u32 dst_h = 0;
    u64 compared = 0;
    u64 over_tolerance = 0;
    f64 max_abs = 0.0;
    f64 mean_abs = 0.0;
    bool strict = true; ///< false: sampler-precision-bound case (see check()).
};

/// RGB comparison of `gpu` against `cpu`; `border` pixels on each side are excluded.
Comparison compare(const std::string& name, u32 sw, u32 sh, const std::vector<Vec4>& cpu, const std::vector<Vec4>& gpu,
                   u32 w, u32 h, u32 border, f64 tol) {
    Comparison c{};
    c.name = name;
    c.src_w = sw;
    c.src_h = sh;
    c.dst_w = w;
    c.dst_h = h;
    f64 sum = 0.0;
    for (u32 y = border; y + border < h; ++y) {
        for (u32 x = border; x + border < w; ++x) {
            const Vec4& a = cpu[static_cast<usize>(y) * w + x];
            const Vec4& b = gpu[static_cast<usize>(y) * w + x];
            const f64 d[3] = {std::fabs(static_cast<f64>(a.x) - b.x), std::fabs(static_cast<f64>(a.y) - b.y),
                              std::fabs(static_cast<f64>(a.z) - b.z)};
            for (const f64 v : d) {
                const f64 e = std::isfinite(v) ? v : 1e30;
                c.max_abs = std::max(c.max_abs, e);
                sum += e;
                c.over_tolerance += e > tol ? 1u : 0u;
                ++c.compared;
            }
        }
    }
    c.mean_abs = c.compared > 0u ? sum / static_cast<f64>(c.compared) : 0.0;
    return c;
}

constexpr f64 kTolerance = 2e-3;
constexpr f64 kMeanTolerance = 1e-4;
/// NIS with non-power-of-two source sizes: the shader loads texels through a linear sampler at
/// (x + 0.5) * (1 / width); when 1 / width is not exact the GPU sampler's sub-texel quantization
/// (VkPhysicalDeviceLimits::subTexelPrecisionBits, 8 on Lavapipe) blends up to 1/256 of a neighbour into
/// the "exact" texel load, which the edge detector and the sharpening amplify. That is sampler hardware
/// precision, not the algorithm (power-of-two sources match to 1e-6), so those cases are bounded
/// statistically.
constexpr f64 kSamplerBoundMean = 2e-3;
constexpr f64 kSamplerBoundFraction = 0.08;

void check(const Comparison& c, std::vector<Comparison>& all) {
    char label[320];
    if (c.strict) {
        std::snprintf(label, sizeof(label),
                      "%s %ux%u->%ux%u CPU port == vendored GLSL on GPU (max |d| %.3g, mean %.3g, %llu/%llu over %.0e)",
                      c.name.c_str(), c.src_w, c.src_h, c.dst_w, c.dst_h, c.max_abs, c.mean_abs,
                      static_cast<unsigned long long>(c.over_tolerance), static_cast<unsigned long long>(c.compared),
                      kTolerance);
        expectTrue(c.compared > 0u && c.over_tolerance == 0u && c.mean_abs < kMeanTolerance, label);
    } else {
        const f64 fraction = c.compared > 0u ? static_cast<f64>(c.over_tolerance) / static_cast<f64>(c.compared) : 1.0;
        std::snprintf(label, sizeof(label),
                      "%s %ux%u->%ux%u (npot source, sampler-precision bound) mean |d| %.3g < %.0e, %.2f%% over %.0e < "
                      "%.0f%%",
                      c.name.c_str(), c.src_w, c.src_h, c.dst_w, c.dst_h, c.mean_abs, kSamplerBoundMean,
                      100.0 * fraction, kTolerance, 100.0 * kSamplerBoundFraction);
        expectTrue(c.compared > 0u && c.mean_abs < kSamplerBoundMean && fraction < kSamplerBoundFraction, label);
    }
    std::printf("%-6s %4ux%-4u -> %4ux%-4u  max %.3e  mean %.3e  over %llu / %llu%s\n", c.name.c_str(), c.src_w, c.src_h,
                c.dst_w, c.dst_h, c.max_abs, c.mean_abs, static_cast<unsigned long long>(c.over_tolerance),
                static_cast<unsigned long long>(c.compared), c.strict ? "" : "  (npot: sampler-precision bound)");
    all.push_back(c);
}

std::string spv(const char* name) { return std::string(FUSE_UPSCALE_SPV_DIR) + "/" + name + ".comp.spv"; }

/// Test input: the analytic scene with every 7th pixel replaced by noise (hits flat, edge, black and white paths).
std::vector<Vec4> make_input(u32 w, u32 h, u32 seed) {
    std::vector<Vec4> src = ut::render_scene(w, h, 3u);
    const std::vector<Vec4> noise = ut::noise_image(w, h, seed);
    for (usize i = 0; i < src.size(); i += 7u) {
        src[i] = noise[i];
    }
    return src;
}

struct Fsr1Cb {
    u32 con[5][4] = {};
};

void run_easu_case(VulkanCompute& vk, u32 sw, u32 sh, u32 dw, u32 dh, std::vector<Comparison>& all) {
    const std::vector<Vec4> src = make_input(sw, sh, sw * 131u + dw);
    std::vector<Vec4> cpu(static_cast<usize>(dw) * dh);
    expectTrue(up::run_easu({src.data(), sw, sh}, {cpu.data(), dw, dh}, {fuse::kernel::Backend::CpuReference}),
               "cpu easu");
    const up::kernels::EasuConstants k = up::easu_constants(sw, sh, dw, dh);
    Fsr1Cb cb{};
    std::memcpy(cb.con[0], k.con0, 16);
    std::memcpy(cb.con[1], k.con1, 16);
    std::memcpy(cb.con[2], k.con2, 16);
    std::memcpy(cb.con[3], k.con3, 16);
    const GpuImage* in = vk.create_image(sw, sh, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    const GpuImage* internal = vk.create_image(dw, dh, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    const GpuImage* out = vk.create_image(dw, dh, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    const GpuBuffer* ubo = vk.create_buffer(sizeof(cb), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    std::vector<Vec4> gpu(cpu.size());
    const bool ok = in && internal && out && ubo && vk.upload(*in, src.data()) && vk.write_buffer(*ubo, &cb, sizeof(cb)) &&
                    vk.dispatch(spv("fsr1_easu"),
                                {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, in, nullptr},
                                 {1000, VK_DESCRIPTOR_TYPE_SAMPLER, nullptr, nullptr},
                                 {2000, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, internal, nullptr},
                                 {2001, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, out, nullptr},
                                 {3000, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, ubo}},
                                (dw + 15u) / 16u, (dh + 15u) / 16u) &&
                    vk.download(*out, gpu.data());
    expectTrue(ok, "gpu easu dispatch");
    if (ok) {
        check(compare("easu", sw, sh, cpu, gpu, dw, dh, 0u, kTolerance), all);
    }
    vk.release_resources();
}

void run_rcas_case(VulkanCompute& vk, u32 w, u32 h, f32 stops, std::vector<Comparison>& all) {
    // RCAS runs on EASU output in the SDK pipeline; feed it an EASU-upscaled image.
    const std::vector<Vec4> low = make_input((w * 2u) / 3u, (h * 2u) / 3u, w + 7u);
    std::vector<Vec4> src(static_cast<usize>(w) * h);
    expectTrue(up::run_easu({low.data(), (w * 2u) / 3u, (h * 2u) / 3u}, {src.data(), w, h}), "rcas input");
    std::vector<Vec4> cpu(src.size());
    expectTrue(up::run_rcas({src.data(), w, h}, {cpu.data(), w, h}, stops, {fuse::kernel::Backend::CpuReference}),
               "cpu rcas");
    const up::kernels::RcasConstants k = up::rcas_constants(stops);
    Fsr1Cb cb{};
    std::memcpy(cb.con[0], k.con, 16);
    const GpuImage* in = vk.create_image(w, h, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    const GpuImage* out = vk.create_image(w, h, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    const GpuBuffer* ubo = vk.create_buffer(sizeof(cb), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    std::vector<Vec4> gpu(cpu.size());
    const bool ok = in && out && ubo && vk.upload(*in, src.data()) && vk.write_buffer(*ubo, &cb, sizeof(cb)) &&
                    vk.dispatch(spv("fsr1_rcas"),
                                {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, in, nullptr},
                                 {1000, VK_DESCRIPTOR_TYPE_SAMPLER, nullptr, nullptr},
                                 {2000, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, out, nullptr},
                                 {3000, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, ubo}},
                                (w + 15u) / 16u, (h + 15u) / 16u) &&
                    vk.download(*out, gpu.data());
    expectTrue(ok, "gpu rcas dispatch");
    if (ok) {
        check(compare("rcas", w, h, cpu, gpu, w, h, 1u, kTolerance), all);
    }
    vk.release_resources();
}

void run_cas_case(VulkanCompute& vk, u32 w, u32 h, f32 sharpness, std::vector<Comparison>& all) {
    const std::vector<Vec4> src = make_input(w, h, w * 17u + h);
    std::vector<Vec4> cpu(src.size());
    expectTrue(up::run_cas({src.data(), w, h}, {cpu.data(), w, h}, sharpness, {fuse::kernel::Backend::CpuReference}),
               "cpu cas");
    // The SDK's CAS output UAV is rgba16 (unorm): round the CPU result the same way before comparing.
    for (Vec4& c : cpu) {
        c.x = std::nearbyint(std::clamp(c.x, 0.f, 1.f) * 65535.f) / 65535.f;
        c.y = std::nearbyint(std::clamp(c.y, 0.f, 1.f) * 65535.f) / 65535.f;
        c.z = std::nearbyint(std::clamp(c.z, 0.f, 1.f) * 65535.f) / 65535.f;
    }
    const up::kernels::CasConstants k = up::cas_constants(sharpness, w, h, w, h);
    u32 cb[2][4] = {};
    std::memcpy(cb[0], k.const0, 16);
    std::memcpy(cb[1], k.const1, 16);
    const GpuImage* in = vk.create_image(w, h, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    const GpuImage* out = vk.create_image(w, h, VK_FORMAT_R16G16B16A16_UNORM, 8);
    const GpuBuffer* ubo = vk.create_buffer(sizeof(cb), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    std::vector<fuse::u16> raw(static_cast<usize>(w) * h * 4u);
    const bool ok = in && out && ubo && vk.upload(*in, src.data()) && vk.write_buffer(*ubo, cb, sizeof(cb)) &&
                    vk.dispatch(spv("cas_sharpen"),
                                {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, in, nullptr},
                                 {1000, VK_DESCRIPTOR_TYPE_SAMPLER, nullptr, nullptr},
                                 {2000, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, out, nullptr},
                                 {3000, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, ubo}},
                                (w + 15u) / 16u, (h + 15u) / 16u) &&
                    vk.download(*out, raw.data());
    expectTrue(ok, "gpu cas dispatch");
    if (ok) {
        std::vector<Vec4> gpu(cpu.size());
        for (usize i = 0; i < gpu.size(); ++i) {
            gpu[i] = Vec4(raw[i * 4u] / 65535.f, raw[i * 4u + 1u] / 65535.f, raw[i * 4u + 2u] / 65535.f, 1.f);
        }
        check(compare("cas", w, h, cpu, gpu, w, h, 1u, kTolerance), all);
    }
    vk.release_resources();
}

void run_nis_case(VulkanCompute& vk, u32 sw, u32 sh, u32 dw, u32 dh, f32 sharpness, std::vector<Comparison>& all) {
    const auto pow2 = [](u32 v) { return v != 0u && (v & (v - 1u)) == 0u; };
    const std::vector<Vec4> src = make_input(sw, sh, sw * 3u + dh);
    std::vector<Vec4> cpu(static_cast<usize>(dw) * dh);
    expectTrue(up::run_nis({src.data(), sw, sh}, {cpu.data(), dw, dh}, sharpness, {fuse::kernel::Backend::CpuReference}),
               "cpu nis");
    NISConfig config{};
    expectTrue(NVScalerUpdateConfig(config, sharpness, 0, 0, sw, sh, sw, sh, 0, 0, dw, dh, dw, dh, NISHDRMode::None),
               "NVScalerUpdateConfig");
    const GpuImage* in = vk.create_image(sw, sh, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    const GpuImage* out = vk.create_image(dw, dh, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    const GpuImage* coefScale = vk.create_image(2, 64, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    const GpuImage* coefUsm = vk.create_image(2, 64, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    const GpuBuffer* ubo = vk.create_buffer(sizeof(NISConfig), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    std::vector<Vec4> gpu(cpu.size());
    // NIS_Main.glsl defaults: NIS_BLOCK_WIDTH 32, NIS_BLOCK_HEIGHT 24, NIS_THREAD_GROUP_SIZE 256.
    const bool ok = in && out && coefScale && coefUsm && ubo && vk.upload(*in, src.data()) &&
                    vk.upload(*coefScale, up::nis_coef_scale().data()) && vk.upload(*coefUsm, up::nis_coef_usm().data()) &&
                    vk.write_buffer(*ubo, &config, sizeof(config)) &&
                    vk.dispatch(spv("nis_scaler"),
                                {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, ubo},
                                 {1, VK_DESCRIPTOR_TYPE_SAMPLER, nullptr, nullptr},
                                 {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, in, nullptr},
                                 {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, out, nullptr},
                                 {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, coefScale, nullptr},
                                 {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, coefUsm, nullptr}},
                                (dw + 31u) / 32u, (dh + 23u) / 24u) &&
                    vk.download(*out, gpu.data());
    expectTrue(ok, "gpu nis dispatch");
    if (ok) {
        Comparison c = compare("nis", sw, sh, cpu, gpu, dw, dh, 0u, kTolerance);
        c.strict = pow2(sw) && pow2(sh);
        check(c, all);
    }
    vk.release_resources();
}

void write_report(const VulkanCompute& vk, const std::vector<Comparison>& all) {
#ifdef FUSE_UPSCALE_GPU_REPORT_PATH
    if (FILE* f = std::fopen(FUSE_UPSCALE_GPU_REPORT_PATH, "wb")) {
        std::fprintf(f, "{\n  \"device\": \"%s\",\n  \"tolerance_abs\": %g,\n  \"tolerance_mean\": %g,\n  \"cases\": [\n",
                     vk.device_name(), kTolerance, kMeanTolerance);
        for (usize i = 0; i < all.size(); ++i) {
            const Comparison& c = all[i];
            std::fprintf(f,
                         "    {\"pass\": \"%s\", \"src\": [%u, %u], \"dst\": [%u, %u], \"compared\": %llu, "
                         "\"over_tolerance\": %llu, \"max_abs\": %.6g, \"mean_abs\": %.6g, \"strict\": %s}%s\n",
                         c.name.c_str(), c.src_w, c.src_h, c.dst_w, c.dst_h, static_cast<unsigned long long>(c.compared),
                         static_cast<unsigned long long>(c.over_tolerance), c.max_abs, c.mean_abs,
                         c.strict ? "true" : "false", i + 1u < all.size() ? "," : "");
        }
        std::fprintf(f, "  ]\n}\n");
        std::fclose(f);
        std::printf("report: %s\n", FUSE_UPSCALE_GPU_REPORT_PATH);
    }
#else
    (void)vk;
    (void)all;
#endif
}

} // namespace

int main() {
    fuse::core::initialize();
    VulkanCompute vk;
    std::string why;
    if (!vk.init(why)) {
        std::printf("SKIP: %s\n", why.c_str());
        fuse::core::shutdown();
        return 77;
    }
    std::printf("device: %s\n", vk.device_name());
    if (!vk.format_supports_storage(VK_FORMAT_R32G32B32A32_SFLOAT) ||
        !vk.format_supports_storage(VK_FORMAT_R16G16B16A16_UNORM)) {
        std::printf("SKIP: rgba32f / rgba16 storage + linear filtering unsupported\n");
        fuse::core::shutdown();
        return 77;
    }
    std::vector<Comparison> all;
    // EASU: 1.5x, 1.7x, 2x, 3x and a non-multiple-of-16 partial-workgroup case.
    run_easu_case(vk, 128, 96, 192, 144, all);
    run_easu_case(vk, 113, 64, 192, 108, all);
    run_easu_case(vk, 96, 72, 192, 144, all);
    run_easu_case(vk, 64, 48, 192, 144, all);
    run_easu_case(vk, 61, 43, 97, 71, all);
    // RCAS: several sharpness levels.
    run_rcas_case(vk, 97, 71, 0.f, all);
    run_rcas_case(vk, 192, 144, 0.2f, all);
    run_rcas_case(vk, 150, 100, 1.6f, all);
    // CAS: sharpness range.
    run_cas_case(vk, 97, 71, 0.f, all);
    run_cas_case(vk, 160, 120, 0.5f, all);
    run_cas_case(vk, 131, 77, 1.f, all);
    // NIS, power-of-two sources (exact texel loads on the GPU): 1x, 1.5x, 1.7x, 2x, partial 32x24 blocks.
    run_nis_case(vk, 128, 64, 128, 64, 0.5f, all);
    run_nis_case(vk, 128, 64, 192, 96, 0.5f, all);
    run_nis_case(vk, 128, 64, 218, 109, 0.2f, all);
    run_nis_case(vk, 64, 64, 128, 128, 0.8f, all);
    run_nis_case(vk, 64, 32, 97, 61, 0.5f, all);
    // NIS, non-power-of-two sources: sampler-precision bound (see kSamplerBoundMean).
    run_nis_case(vk, 96, 72, 192, 144, 0.5f, all);
    run_nis_case(vk, 61, 43, 110, 80, 0.5f, all);
    write_report(vk, all);
    vk.shutdown();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_upscale_gpu_reference: all checks passed\n");
    return 0;
}

#else // no Vulkan headers / shaders

int main() {
    std::printf("SKIP: Vulkan headers, NIS_Config.h or the upscaler SPIR-V were not available at build time\n");
    return 77;
}

#endif
