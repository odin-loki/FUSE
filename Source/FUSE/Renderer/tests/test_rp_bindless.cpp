// WP-0.4 Vulkan exit gates for the bindless heap (Lavapipe, VK_LAYER_KHRONOS_validation with
// synchronization validation; every validation message fails the run).
//
//   --mode textures  One compute shader reaches every resource through 32-bit bindless handles:
//                    10,000 sampled textures (one texel each, distinct values), a sampler from the
//                    sampler heap, a storage-buffer handle table, a storage-buffer output, a buffer
//                    read only through the buffer-address table (BDA), a uniform buffer and an r32ui
//                    storage image. Readback equals the CPU expectation (including handles with the
//                    wrong type bits, which the shader rejects).
//   --mode churn     16 frames, 2 in flight, 1024 live textures; every frame retires 128 textures
//                    (retireSlot) and registers 128 new ones, rebuilding the handle table. Retired
//                    slots are reclaimed only after the frame fence (collectRetired), their images
//                    destroyed only then; no index still read by an in-flight frame is reused;
//                    stale handles are rejected; every frame's readback is exact.
//   --backend set | buffer | fallback
//                    set:      descriptor-indexing backend (UPDATE_AFTER_BIND set).
//                    buffer:   VK_EXT_descriptor_buffer (Auto preference; skip when unsupported).
//                    fallback: device created without optional features, descriptor buffer
//                              requested -> must fall back to the descriptor-set backend.
//
// Exit 77 = skip (stub build, no ICD / validation layer, shader not built, backend unsupported).
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

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

// Handle packing is shared with shaders/common/bindless.glsl: keep the two in sync.
static_assert(fuse::renderer::kBindlessHandleIndexMask == 0xFFFFFu, "bindless.glsl FUSE_HANDLE_INDEX_MASK");
static_assert(fuse::renderer::kBindlessHandleTypeShift == 20u, "bindless.glsl FUSE_HANDLE_TYPE_SHIFT");
static_assert(fuse::renderer::kBindlessHandleGenerationShift == 24u, "bindless.glsl FUSE_HANDLE_GENERATION_SHIFT");
static_assert(static_cast<unsigned>(fuse::renderer::BindlessResourceType::SampledImage) == 1u &&
                  static_cast<unsigned>(fuse::renderer::BindlessResourceType::StorageImage) == 2u &&
                  static_cast<unsigned>(fuse::renderer::BindlessResourceType::Sampler) == 3u &&
                  static_cast<unsigned>(fuse::renderer::BindlessResourceType::StorageBuffer) == 4u &&
                  static_cast<unsigned>(fuse::renderer::BindlessResourceType::UniformBuffer) == 5u,
              "bindless.glsl FUSE_HANDLE_TYPE_*");
static_assert(fuse::renderer::kBindlessBindingBufferAddressTable == 5u, "bindless.glsl address table binding");

#if !defined(FUSE_VULKAN_BACKEND)

int main() {
    std::printf("SKIP: stub build (no Vulkan backend)\n");
    return kSkip;
}

#else

namespace {

using namespace fuse::renderer;
using fuse::u32;
using fuse::u64;
using fuse::u8;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr u32 kBadHandle = 0xBAD0BAD0u;
constexpr u32 kStorageCount = 64;

// --- validation capture -----------------------------------------------------------------------

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

// --- context ------------------------------------------------------------------------------------

struct Context {
    std::unique_ptr<VulkanInstance> instance;
    std::unique_ptr<VulkanDevice> device;
    std::unique_ptr<GpuAllocator> allocator;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    BindlessDescriptors bindless;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
            if (pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(vkDevice, pipeline, nullptr);
            }
            if (pipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(vkDevice, pipelineLayout, nullptr);
            }
            if (commandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(vkDevice, commandPool, nullptr);
            }
        }
        if (device != nullptr) {
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

enum class BackendMode { Set, Buffer, Fallback };

int setupContext(Context& ctx, BackendMode mode) {
    if (!layerAvailable(kValidationLayer)) {
        std::printf("SKIP: %s not installed (the gate needs synchronization validation)\n", kValidationLayer);
        return kSkip;
    }
    setenv("VK_INSTANCE_LAYERS", kValidationLayer, 1);
    setenv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", 1);
    setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
    setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK", 1);

    VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "fuse_rp_bindless";
    instanceDesc.enableValidation = true;
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
    if (createMessenger == nullptr) {
        std::printf("SKIP: VK_EXT_debug_utils unavailable\n");
        return kSkip;
    }
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = onMessage;
    createMessenger(vkInstance, &info, nullptr, &ctx.messenger);

    VulkanDeviceDesc deviceDesc{};
    deviceDesc.enableOptionalFeatures = mode != BackendMode::Fallback;
    ctx.device = VulkanDevice::create(*ctx.instance, deviceDesc);
    if (ctx.device == nullptr || !ctx.device->isValid()) {
        std::printf("SKIP: no Vulkan device\n");
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.queue = static_cast<VkQueue>(ctx.device->queues().graphics);
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    const RendererCaps& caps = ctx.device->info().caps;
    std::printf("device: %s | %s\n", ctx.device->info().deviceName.c_str(), caps.summary().c_str());

    BindlessDesc desc{};
    switch (mode) {
    case BackendMode::Set:
        desc.backend = BindlessBackendPreference::DescriptorSet;
        break;
    case BackendMode::Buffer:
        if (!caps.descriptorBuffer) {
            std::printf("SKIP: device has no VK_EXT_descriptor_buffer\n");
            return kSkip;
        }
        desc.backend = BindlessBackendPreference::Auto; // Auto must pick the descriptor buffer
        break;
    case BackendMode::Fallback:
        desc.backend = BindlessBackendPreference::DescriptorBuffer;
        expect(!caps.descriptorBuffer, "optional features off: descriptor buffer not enabled");
        break;
    }
    if (!ctx.bindless.init(*ctx.device, desc)) {
        std::fprintf(stderr, "FAIL: bindless init created no GPU backend\n");
        return 1;
    }
    const BindlessBackend expected =
        mode == BackendMode::Buffer ? BindlessBackend::DescriptorBuffer : BindlessBackend::DescriptorSet;
    expect(ctx.bindless.backend() == expected, "bindless backend matches the request / fallback");
    const BindlessArraySizes& sizes = ctx.bindless.arraySizes();
    std::printf("bindless backend: %s | arrays: sampled %u storage-img %u samplers %u ssbo %u ubo %u | "
                "descriptor buffer %llu bytes | address table 0x%llx\n",
                bindlessBackendName(ctx.bindless.backend()), sizes.sampledImages, sizes.storageImages, sizes.samplers,
                sizes.storageBuffers, sizes.uniformBuffers,
                static_cast<unsigned long long>(ctx.bindless.descriptorBufferSize()),
                static_cast<unsigned long long>(ctx.bindless.bufferAddressTableAddress()));
    expect(ctx.bindless.bufferAddressTableAddress() != 0u, "buffer-address table created");
    if (expected == BindlessBackend::DescriptorBuffer) {
        expect(ctx.bindless.descriptorSetHandle() == nullptr && ctx.bindless.poolHandle() == nullptr,
               "descriptor-buffer backend has no pool / set");
        expect(ctx.bindless.pipelineCreateFlags() != 0u, "descriptor-buffer pipelines need the create flag");
    } else {
        expect(ctx.bindless.descriptorSetHandle() != nullptr, "descriptor-set backend has a set");
        expect(ctx.bindless.pipelineCreateFlags() == 0u, "descriptor-set pipelines need no create flag");
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = ctx.device->queues().graphicsFamily;
    if (vkCreateCommandPool(ctx.vkDevice, &poolInfo, nullptr, &ctx.commandPool) != VK_SUCCESS) {
        std::fprintf(stderr, "FAIL: command pool\n");
        return 1;
    }
    return 0;
}

int setupPipeline(Context& ctx) {
#if defined(FUSE_RP_BINDLESS_SHADER)
    std::ifstream file(FUSE_RP_BINDLESS_SHADER, std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (code.empty() || code.size() % 4u != 0u) {
        std::printf("SKIP: %s not readable\n", FUSE_RP_BINDLESS_SHADER);
        return kSkip;
    }
    VkDescriptorSetLayout setLayout = static_cast<VkDescriptorSetLayout>(ctx.bindless.layoutHandle());
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 * sizeof(u32)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(ctx.vkDevice, &layoutInfo, nullptr, &ctx.pipelineLayout) != VK_SUCCESS) {
        return 1;
    }
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = code.size();
    moduleInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(ctx.vkDevice, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
        return 1;
    }
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.flags = static_cast<VkPipelineCreateFlags>(ctx.bindless.pipelineCreateFlags());
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = ctx.pipelineLayout;
    const VkResult created = vkCreateComputePipelines(ctx.vkDevice, VK_NULL_HANDLE, 1, &info, nullptr, &ctx.pipeline);
    vkDestroyShaderModule(ctx.vkDevice, module, nullptr);
    return created == VK_SUCCESS ? 0 : 1;
#else
    (void)ctx;
    std::printf("SKIP: rp_bindless_test.comp not built (glslangValidator missing)\n");
    return kSkip;
#endif
}

// --- resources ----------------------------------------------------------------------------------

BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}
ImageUsage operator|(ImageUsage a, ImageUsage b) {
    return static_cast<ImageUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}

bool makeBuffer(Context& ctx, Buffer& out, u64 size, BufferUsage usage, MemoryUsage memory, const char* name) {
    BufferDesc desc{};
    desc.size = static_cast<fuse::usize>(size);
    desc.usage = usage;
    desc.memoryUsage = memory;
    desc.name = name;
    return ctx.allocator->createBuffer(desc, out) &&
           (memory == MemoryUsage::GpuOnly || out.mapped != nullptr);
}

/// Texel value of texture `id` (RGBA8, R in the low byte = packUnorm4x8 order).
u32 texelValue(u32 id, u32 salt) {
    const u32 r = id & 0xFFu;
    const u32 g = (id >> 8u) & 0xFFu;
    const u32 b = (id * 7u + 3u + salt) & 0xFFu;
    const u32 a = (0xA5u ^ (id & 3u) ^ (salt >> 3u)) & 0xFFu;
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}

u32 bdaWord(u32 i) { return i * 2654435761u; }

/// 1x1 host-visible (linear, PREINITIALIZED) sampled texture holding texelValue(id, salt).
bool makeTexture(Context& ctx, Texture& out, u32 id, u32 salt) {
    TextureDesc desc{};
    desc.width = 1;
    desc.height = 1;
    desc.format = GpuFormat::R8G8B8A8Unorm;
    desc.usage = ImageUsage::Sampled;
    desc.memoryUsage = MemoryUsage::CpuToGpu;
    if (!ctx.allocator->createImage(desc, out) || out.mapped == nullptr) {
        return false;
    }
    const u32 value = texelValue(id, salt);
    std::memcpy(out.mapped, &value, sizeof(value));
    return true;
}

VkImageMemoryBarrier2 hostToSampled(const Texture& texture) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = static_cast<VkImage>(texture.image);
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}

void pipelineBarrier(VkCommandBuffer cmd, const std::vector<VkImageMemoryBarrier2>& images,
                     const VkMemoryBarrier2* memory) {
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = memory != nullptr ? 1u : 0u;
    dep.pMemoryBarriers = memory;
    dep.imageMemoryBarrierCount = static_cast<u32>(images.size());
    dep.pImageMemoryBarriers = images.empty() ? nullptr : images.data();
    vkCmdPipelineBarrier2(cmd, &dep);
}

VkCommandBuffer allocateCommandBuffer(Context& ctx) {
    VkCommandBufferAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool = ctx.commandPool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(ctx.vkDevice, &info, &cmd);
    return cmd;
}

struct PushData {
    u32 handleTable = 0;
    u32 outputBuffer = 0;
    u32 bdaBuffer = 0;
    u32 samplerHandle = 0;
    u32 paramsHandle = 0;
    u32 imageHandle = 0;
    u32 count = 0;
    u32 storageCount = 0;
};

void recordDispatch(Context& ctx, VkCommandBuffer cmd, const PushData& push) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.pipeline);
    ctx.bindless.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.pipelineLayout, 0);
    vkCmdPushConstants(cmd, ctx.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (push.count + 63u) / 64u, 1, 1);
}

VkMemoryBarrier2 computeToHost() {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    return barrier;
}

bool submitAndWait(Context& ctx, VkCommandBuffer cmd, VkFence fence) {
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (vkQueueSubmit(ctx.queue, 1, &submit, fence) != VK_SUCCESS) {
        return false;
    }
    return fence == VK_NULL_HANDLE ? vkQueueWaitIdle(ctx.queue) == VK_SUCCESS
                                   : vkWaitForFences(ctx.vkDevice, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
}

// --- mode: textures -------------------------------------------------------------------------------

int runTextures(Context& ctx) {
    constexpr u32 kTextures = 10000;
    constexpr u32 kSalt = 0x5EED1234u;
    BindlessDescriptors& bl = ctx.bindless;

    // Sampler heap: identical descs share one cached VkSampler / slot.
    SamplerDesc samplerDesc{};
    samplerDesc.name = "rp_bindless.nearest";
    const BindlessSlotHandle sampler = bl.acquireSampler(samplerDesc);
    const BindlessSlotHandle samplerAgain = bl.acquireSampler(samplerDesc);
    expect(sampler.isValid() && sampler == samplerAgain && bl.samplerCacheSize() == 1u,
           "sampler heap deduplicates identical sampler descs");
    SamplerDesc linearDesc{};
    linearDesc.minFilter = linearDesc.magFilter = 1;
    const BindlessSlotHandle linear = bl.acquireSampler(linearDesc);
    expect(linear.isValid() && linear != sampler && bl.samplerCacheSize() == 2u, "distinct sampler desc, new slot");

    std::vector<Texture> textures(kTextures);
    std::vector<BindlessSlotHandle> slots(kTextures);
    for (u32 i = 0; i < kTextures; ++i) {
        if (!makeTexture(ctx, textures[i], i, 0)) {
            std::fprintf(stderr, "FAIL: texture %u\n", i);
            return 1;
        }
        slots[i] = bl.registerTextureSlot(textures[i]);
        if (!slots[i].isValid()) {
            std::fprintf(stderr, "FAIL: bindless slot for texture %u\n", i);
            return 1;
        }
    }
    expect(bl.registeredTextureCount() == kTextures, "10k textures live in the heap");

    Buffer table{}, output{}, bda{}, params{}, imageReadback{};
    const u64 words = kTextures * sizeof(u32);
    bool ok = makeBuffer(ctx, table, words, BufferUsage::Storage, MemoryUsage::CpuToGpu, "rp_bindless.table") &&
              makeBuffer(ctx, output, words, BufferUsage::Storage, MemoryUsage::GpuToCpu, "rp_bindless.output") &&
              makeBuffer(ctx, bda, words, BufferUsage::Storage | BufferUsage::ShaderDeviceAddress,
                         MemoryUsage::CpuToGpu, "rp_bindless.bda") &&
              makeBuffer(ctx, params, 16, BufferUsage::Uniform, MemoryUsage::CpuToGpu, "rp_bindless.params") &&
              makeBuffer(ctx, imageReadback, kStorageCount * sizeof(u32), BufferUsage::TransferDst,
                         MemoryUsage::GpuToCpu, "rp_bindless.image_readback");
    Texture storageImage{};
    TextureDesc storageDesc{};
    storageDesc.width = kStorageCount;
    storageDesc.height = 1;
    storageDesc.format = static_cast<GpuFormat>(VK_FORMAT_R32_UINT);
    storageDesc.usage = ImageUsage::Storage | ImageUsage::TransferSrc;
    storageDesc.name = "rp_bindless.storage_image";
    ok = ok && ctx.allocator->createImage(storageDesc, storageImage);
    if (!ok || bda.deviceAddress == 0u) {
        std::fprintf(stderr, "FAIL: buffers / storage image\n");
        return 1;
    }

    const BindlessSlotHandle tableSlot = bl.registerBufferSlot(table);
    const BindlessSlotHandle outputSlot = bl.registerBufferSlot(output);
    const BindlessSlotHandle bdaSlot = bl.registerBufferSlot(bda);
    const BindlessSlotHandle paramsSlot = bl.registerBufferSlot(params, true);
    const BindlessSlotHandle imageSlot = bl.registerTextureSlot(storageImage, true);
    expect(bl.bufferAddressAt(bdaSlot.index) == bda.deviceAddress, "address table holds the BDA buffer address");
    expect(bl.bufferAddressAt(outputSlot.index) == output.deviceAddress, "address table holds every buffer address");

    // Handle table: texture handles, with every 997th entry replaced by a wrong-type (sampler) handle.
    std::vector<u32> handles(kTextures);
    std::vector<u32> expected(kTextures);
    auto* bdaWords = static_cast<u32*>(bda.mapped);
    for (u32 i = 0; i < kTextures; ++i) {
        handles[i] = bl.shaderHandle(slots[i]);
        bdaWords[i] = bdaWord(i);
        expected[i] = texelValue(i, 0) ^ bdaWord(i) ^ kSalt;
        if (i % 997u == 5u) {
            handles[i] = bl.shaderHandle(sampler);
            expected[i] = kBadHandle;
        }
    }
    expect(bindlessShaderHandleType(handles[0]) == BindlessResourceType::SampledImage &&
               bindlessShaderHandleIndex(handles[0]) == slots[0].index && bl.validateShaderHandle(handles[0]),
           "shader handle packs type + index and validates");
    expect(bl.slotHandleFromShaderHandle(handles[1]) == slots[1], "shader handle maps back to its slot");
    std::memcpy(table.mapped, handles.data(), words);
    const u32 paramsWords[4] = {kSalt, 0, 0, 0};
    std::memcpy(params.mapped, paramsWords, sizeof(paramsWords));

    VkCommandBuffer cmd = allocateCommandBuffer(ctx);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    std::vector<VkImageMemoryBarrier2> barriers;
    barriers.reserve(kTextures + 1u);
    for (const Texture& texture : textures) {
        barriers.push_back(hostToSampled(texture));
    }
    VkImageMemoryBarrier2 toGeneral{};
    toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    toGeneral.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    toGeneral.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toGeneral.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image = static_cast<VkImage>(storageImage.image);
    toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barriers.push_back(toGeneral);
    pipelineBarrier(cmd, barriers, nullptr);

    PushData push{};
    push.handleTable = bl.shaderHandle(tableSlot);
    push.outputBuffer = bl.shaderHandle(outputSlot);
    push.bdaBuffer = bl.shaderHandle(bdaSlot);
    push.samplerHandle = bl.shaderHandle(sampler);
    push.paramsHandle = bl.shaderHandle(paramsSlot);
    push.imageHandle = bl.shaderHandle(imageSlot);
    push.count = kTextures;
    push.storageCount = kStorageCount;
    expect(bindlessShaderHandleType(push.paramsHandle) == BindlessResourceType::UniformBuffer &&
               bindlessShaderHandleType(push.imageHandle) == BindlessResourceType::StorageImage,
           "uniform-buffer / storage-image handle types");
    recordDispatch(ctx, cmd, push);

    VkImageMemoryBarrier2 toCopy = toGeneral;
    toCopy.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toCopy.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toCopy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toCopy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    toCopy.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    pipelineBarrier(cmd, {toCopy}, nullptr);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kStorageCount, 1, 1};
    vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(storageImage.image), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           static_cast<VkBuffer>(imageReadback.handle), 1, &region);
    const VkMemoryBarrier2 toHost = computeToHost();
    pipelineBarrier(cmd, {}, &toHost);
    vkEndCommandBuffer(cmd);
    expect(submitAndWait(ctx, cmd, VK_NULL_HANDLE), "submit + wait");

    std::vector<u32> result(kTextures, 0u);
    ctx.allocator->readMapped(output, result.data(), words);
    u32 mismatches = 0;
    for (u32 i = 0; i < kTextures; ++i) {
        if (result[i] != expected[i]) {
            if (mismatches < 8u) {
                std::fprintf(stderr, "  out[%u] = 0x%08x, expected 0x%08x\n", i, result[i], expected[i]);
            }
            ++mismatches;
        }
    }
    std::vector<u32> imageWords(kStorageCount, 0u);
    ctx.allocator->readMapped(imageReadback, imageWords.data(), kStorageCount * sizeof(u32));
    const bool imageOk = std::equal(imageWords.begin(), imageWords.end(), expected.begin());
    std::printf("textures: %u sampled textures read through handles, %u mismatches; storage image %s\n", kTextures,
                mismatches, imageOk ? "matches" : "MISMATCH");
    expect(mismatches == 0u, "10k-texture + BDA readback equals the CPU expectation");
    expect(imageOk, "bindless storage-image writes read back");

    // Deferred release: handle stale at once, slot reserved until the serial completes.
    bl.setFrameSerial(1);
    expect(bl.retireSlot(slots[0]), "retire live texture slot");
    expect(!bl.validateSlot(slots[0]) && !bl.validateShaderHandle(handles[0]) && bl.shaderHandle(slots[0]) == 0u,
           "retired handle rejected immediately");
    expect(!bl.retireSlot(slots[0]), "double retire rejected");
    expect(bl.isSlotRetired(BindlessHeapKind::Texture, slots[0].index) && bl.retiredCount() == 1u,
           "slot reserved while retired");
    expect(bl.collectRetired(0) == 0u, "not reclaimed before its serial completes");
    expect(bl.collectRetired(1) == 1u && bl.retiredCount() == 0u, "reclaimed once the serial completes");

    bl.releaseSampler(samplerAgain);
    expect(bl.samplerCacheSize() == 2u && bl.validateSlot(sampler), "sampler kept while referenced");
    bl.releaseSampler(sampler);
    bl.releaseSampler(linear);
    expect(bl.samplerCacheSize() == 0u && bl.retiredCount(BindlessHeapKind::Sampler) == 2u,
           "last release retires the sampler slot");
    expect(bl.collectRetired(1) == 2u, "sampler slots reclaimed (VkSampler destroyed)");

    vkDeviceWaitIdle(ctx.vkDevice);
    vkFreeCommandBuffers(ctx.vkDevice, ctx.commandPool, 1, &cmd);
    for (u32 i = 0; i < kTextures; ++i) {
        if (i != 0u) {
            bl.freeSlot(slots[i]);
        }
        ctx.allocator->destroyImage(textures[i]);
    }
    for (BindlessSlotHandle slot : {tableSlot, outputSlot, bdaSlot, paramsSlot, imageSlot}) {
        bl.freeSlot(slot);
    }
    expect(bl.bufferAddressAt(bdaSlot.index) == 0u, "freed buffer slot clears its address-table entry");
    ctx.allocator->destroyImage(storageImage);
    for (Buffer* buffer : {&table, &output, &bda, &params, &imageReadback}) {
        ctx.allocator->destroyBuffer(*buffer);
    }
    return 0;
}

// --- mode: churn ----------------------------------------------------------------------------------

struct LiveTexture {
    Texture texture{};
    BindlessSlotHandle slot{};
    u32 id = 0;
};

struct PendingDestroy {
    Texture texture{};
    u64 serial = 0;
};

u32 nextRandom(u32& state) {
    state ^= state << 13u;
    state ^= state >> 17u;
    state ^= state << 5u;
    return state;
}

int runChurn(Context& ctx) {
    constexpr u32 kLive = 1024;
    constexpr u32 kChurn = 128;
    constexpr u32 kFrames = 16;
    constexpr u32 kInFlight = 2;
    constexpr u32 kSalt = 0xC0FFEEu;
    BindlessDescriptors& bl = ctx.bindless;

    SamplerDesc samplerDesc{};
    const BindlessSlotHandle sampler = bl.acquireSampler(samplerDesc);
    Buffer bda{}, params{};
    const u64 words = kLive * sizeof(u32);
    if (!makeBuffer(ctx, bda, words, BufferUsage::Storage | BufferUsage::ShaderDeviceAddress, MemoryUsage::CpuToGpu,
                    "rp_bindless.churn.bda") ||
        !makeBuffer(ctx, params, 16, BufferUsage::Uniform, MemoryUsage::CpuToGpu, "rp_bindless.churn.params")) {
        std::fprintf(stderr, "FAIL: churn buffers\n");
        return 1;
    }
    for (u32 i = 0; i < kLive; ++i) {
        static_cast<u32*>(bda.mapped)[i] = bdaWord(i);
    }
    const u32 paramsWords[4] = {kSalt, 0, 0, 0};
    std::memcpy(params.mapped, paramsWords, sizeof(paramsWords));
    const BindlessSlotHandle bdaSlot = bl.registerBufferSlot(bda);
    const BindlessSlotHandle paramsSlot = bl.registerBufferSlot(params, true);

    struct FrameSlot {
        Buffer table{}, output{};
        BindlessSlotHandle tableSlot{}, outputSlot{};
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool submitted = false;
        u64 serial = 0;
        std::vector<u32> expected;
        std::vector<u32> indices; // texture slot indices this frame's dispatch reads
    };
    FrameSlot frames[kInFlight];
    for (u32 s = 0; s < kInFlight; ++s) {
        FrameSlot& f = frames[s];
        if (!makeBuffer(ctx, f.table, words, BufferUsage::Storage, MemoryUsage::CpuToGpu, "rp_bindless.churn.table") ||
            !makeBuffer(ctx, f.output, words, BufferUsage::Storage, MemoryUsage::GpuToCpu, "rp_bindless.churn.out")) {
            return 1;
        }
        f.tableSlot = bl.registerBufferSlot(f.table);
        f.outputSlot = bl.registerBufferSlot(f.output);
        f.cmd = allocateCommandBuffer(ctx);
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(ctx.vkDevice, &fenceInfo, nullptr, &f.fence);
    }

    std::vector<LiveTexture> live(kLive);
    std::vector<Texture> fresh; // created this frame: need the PREINITIALIZED -> read-only transition
    u32 nextId = 0;
    for (LiveTexture& entry : live) {
        entry.id = nextId++;
        if (!makeTexture(ctx, entry.texture, entry.id, kSalt) ||
            !(entry.slot = bl.registerTextureSlot(entry.texture)).isValid()) {
            return 1;
        }
        fresh.push_back(entry.texture);
    }
    const u32 initialHighWater = bl.heapCapacity(BindlessHeapKind::Texture);

    std::vector<PendingDestroy> pendingDestroy;
    std::vector<BindlessSlotHandle> staleHandles;
    u32 rng = 0x1234567u;
    u32 reusedIndices = 0;
    u32 inFlightReuseViolations = 0;
    u32 frameMismatches = 0;
    u32 verifiedFrames = 0;

    auto verify = [&](FrameSlot& f) {
        std::vector<u32> result(kLive, 0u);
        ctx.allocator->readMapped(f.output, result.data(), words);
        u32 bad = 0;
        for (u32 i = 0; i < kLive; ++i) {
            bad += result[i] != f.expected[i] ? 1u : 0u;
        }
        if (bad != 0u) {
            std::fprintf(stderr, "  frame serial %llu: %u mismatches\n", static_cast<unsigned long long>(f.serial), bad);
        }
        frameMismatches += bad;
        ++verifiedFrames;
    };

    for (u32 frame = 1; frame <= kFrames; ++frame) {
        FrameSlot& f = frames[frame % kInFlight];
        if (f.submitted) {
            vkWaitForFences(ctx.vkDevice, 1, &f.fence, VK_TRUE, UINT64_MAX);
            vkResetFences(ctx.vkDevice, 1, &f.fence);
            f.submitted = false;
            verify(f);
            // Every frame up to f.serial has completed (in-order single queue).
            bl.collectRetired(f.serial);
            auto done = std::stable_partition(pendingDestroy.begin(), pendingDestroy.end(),
                                              [&](const PendingDestroy& p) { return p.serial > f.serial; });
            for (auto it = done; it != pendingDestroy.end(); ++it) {
                ctx.allocator->destroyImage(it->texture);
            }
            pendingDestroy.erase(done, pendingDestroy.end());
        }
        bl.setFrameSerial(frame);

        // Indices still read by the other in-flight frame must not be handed out again.
        const FrameSlot& other = frames[(frame + 1u) % kInFlight];
        if (frame > 1u) {
            for (u32 c = 0; c < kChurn; ++c) {
                LiveTexture& victim = live[nextRandom(rng) % kLive];
                expect(bl.retireSlot(victim.slot), "retire live slot");
                staleHandles.push_back(victim.slot);
                pendingDestroy.push_back(PendingDestroy{victim.texture, frame});
                victim.id = nextId++;
                if (!makeTexture(ctx, victim.texture, victim.id, kSalt)) {
                    return 1;
                }
                victim.slot = bl.registerTextureSlot(victim.texture);
                if (!victim.slot.isValid()) {
                    std::fprintf(stderr, "FAIL: heap exhausted at frame %u\n", frame);
                    return 1;
                }
                fresh.push_back(victim.texture);
                if (victim.slot.generation > 1u) {
                    ++reusedIndices;
                }
                if (other.submitted &&
                    std::find(other.indices.begin(), other.indices.end(), victim.slot.index) != other.indices.end()) {
                    ++inFlightReuseViolations;
                }
            }
        }

        f.serial = frame;
        f.expected.assign(kLive, 0u);
        f.indices.assign(kLive, 0u);
        std::vector<u32> handles(kLive);
        for (u32 i = 0; i < kLive; ++i) {
            handles[i] = bl.shaderHandle(live[i].slot);
            f.indices[i] = live[i].slot.index;
            f.expected[i] = texelValue(live[i].id, kSalt) ^ bdaWord(i) ^ kSalt;
        }
        std::memcpy(f.table.mapped, handles.data(), words);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(f.cmd, 0);
        vkBeginCommandBuffer(f.cmd, &begin);
        std::vector<VkImageMemoryBarrier2> barriers;
        for (const Texture& texture : fresh) {
            barriers.push_back(hostToSampled(texture));
        }
        fresh.clear();
        pipelineBarrier(f.cmd, barriers, nullptr);
        PushData push{};
        push.handleTable = bl.shaderHandle(f.tableSlot);
        push.outputBuffer = bl.shaderHandle(f.outputSlot);
        push.bdaBuffer = bl.shaderHandle(bdaSlot);
        push.samplerHandle = bl.shaderHandle(sampler);
        push.paramsHandle = bl.shaderHandle(paramsSlot);
        push.count = kLive;
        push.storageCount = 0;
        recordDispatch(ctx, f.cmd, push);
        const VkMemoryBarrier2 toHost = computeToHost();
        pipelineBarrier(f.cmd, {}, &toHost);
        vkEndCommandBuffer(f.cmd);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &f.cmd;
        expect(vkQueueSubmit(ctx.queue, 1, &submit, f.fence) == VK_SUCCESS, "churn submit");
        f.submitted = true;
    }
    for (FrameSlot& f : frames) {
        if (f.submitted) {
            vkWaitForFences(ctx.vkDevice, 1, &f.fence, VK_TRUE, UINT64_MAX);
            f.submitted = false;
            verify(f);
        }
    }
    bl.collectRetired(kFrames);
    for (PendingDestroy& p : pendingDestroy) {
        ctx.allocator->destroyImage(p.texture);
    }
    pendingDestroy.clear();

    u32 staleAccepted = 0;
    for (BindlessSlotHandle stale : staleHandles) {
        staleAccepted += (bl.validateSlot(stale) || bl.shaderHandle(stale) != 0u) ? 1u : 0u;
    }
    const u32 highWater = bl.heapCapacity(BindlessHeapKind::Texture);
    std::printf("churn: %u frames x %u retire/register over %u live; %u reused indices, heap high-water %u "
                "(initial %u), %u in-flight reuse violations, %u stale handles accepted, %u mismatches "
                "(%u frames verified)\n",
                kFrames, kChurn, kLive, reusedIndices, highWater, initialHighWater, inFlightReuseViolations,
                staleAccepted, frameMismatches, verifiedFrames);
    expect(verifiedFrames == kFrames, "every frame verified");
    expect(frameMismatches == 0u, "churned frames read back exactly");
    expect(inFlightReuseViolations == 0u, "no slot still read by an in-flight frame is reused");
    expect(staleAccepted == 0u, "stale handles rejected after churn");
    expect(reusedIndices > 0u, "retired slots are reclaimed and reused after their fence");
    // Reclaim after kInFlight frames bounds the heap: live + retire window of kInFlight+1 frames.
    expect(highWater <= kLive + (kInFlight + 1u) * kChurn, "heap high-water bounded by the retire window");
    expect(bl.retiredCount() == 0u && bl.registeredTextureCount() == kLive, "all retired slots reclaimed");

    vkDeviceWaitIdle(ctx.vkDevice);
    for (LiveTexture& entry : live) {
        bl.freeSlot(entry.slot);
        ctx.allocator->destroyImage(entry.texture);
    }
    for (FrameSlot& f : frames) {
        bl.freeSlot(f.tableSlot);
        bl.freeSlot(f.outputSlot);
        ctx.allocator->destroyBuffer(f.table);
        ctx.allocator->destroyBuffer(f.output);
        vkFreeCommandBuffers(ctx.vkDevice, ctx.commandPool, 1, &f.cmd);
        vkDestroyFence(ctx.vkDevice, f.fence, nullptr);
    }
    bl.freeSlot(bdaSlot);
    bl.freeSlot(paramsSlot);
    bl.releaseSampler(sampler);
    bl.collectRetired(UINT64_MAX);
    ctx.allocator->destroyBuffer(bda);
    ctx.allocator->destroyBuffer(params);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "textures";
    std::string backend = "set";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0) {
            mode = argv[++i];
        } else if (std::strcmp(argv[i], "--backend") == 0) {
            backend = argv[++i];
        }
    }
    BackendMode backendMode = BackendMode::Set;
    if (backend == "buffer") {
        backendMode = BackendMode::Buffer;
    } else if (backend == "fallback") {
        backendMode = BackendMode::Fallback;
    } else if (backend != "set") {
        std::fprintf(stderr, "unknown --backend %s\n", backend.c_str());
        return 2;
    }

    int rc = 0;
    {
        Context ctx;
        rc = setupContext(ctx, backendMode);
        if (rc == 0) {
            rc = setupPipeline(ctx);
        }
        if (rc == 0) {
            if (mode == "textures") {
                rc = runTextures(ctx);
            } else if (mode == "churn") {
                rc = runChurn(ctx);
            } else {
                std::fprintf(stderr, "unknown --mode %s\n", mode.c_str());
                rc = 2;
            }
        }
    }
    if (rc == kSkip) {
        return kSkip;
    }
    std::printf("validation messages: %u\n", g_messages);
    expect(g_messages == 0u, "zero validation messages (incl. synchronization validation)");
    if (rc != 0 || g_failures != 0) {
        std::fprintf(stderr, "fuse_rp_bindless (%s, %s): FAILED (rc %d, %d check failure(s))\n", mode.c_str(),
                     backend.c_str(), rc, g_failures);
        return 1;
    }
    std::printf("fuse_rp_bindless (%s, %s): all checks passed\n", mode.c_str(), backend.c_str());
    return 0;
}

#endif
