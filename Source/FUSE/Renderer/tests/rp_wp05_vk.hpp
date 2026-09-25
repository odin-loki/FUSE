// WP-0.5 test helpers (Vulkan backend only): validation-layer context that counts every warning and
// error message, host-visible buffers and a one-shot compute dispatch with readback. Header-only;
// shared by test_rp_slang_twin.cpp and test_pipeline_cache.cpp.
#pragma once

#if defined(FUSE_VULKAN_BACKEND)

#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace rp_wp05 {

using fuse::u32;
using fuse::u8;

constexpr int kSkip = 77;
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

inline u32& validationMessageCount() {
    static u32 count = 0;
    return count;
}

inline VKAPI_ATTR VkBool32 VKAPI_CALL onValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                          VkDebugUtilsMessageTypeFlagsEXT type,
                                                          const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                          void*) {
    constexpr VkDebugUtilsMessageSeverityFlagsEXT kSeverities =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    constexpr VkDebugUtilsMessageTypeFlagsEXT kTypes =
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    if ((severity & kSeverities) == 0 || (type & kTypes) == 0) {
        return VK_FALSE;
    }
    ++validationMessageCount();
    std::fprintf(stderr, "VALIDATION MESSAGE: %s\n  %s\n",
                 data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName : "(no id)",
                 data != nullptr && data->pMessage != nullptr ? data->pMessage : "");
    return VK_FALSE;
}

inline bool layerAvailable(const char* name) {
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

inline void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

struct Context {
    std::unique_ptr<fuse::renderer::VulkanInstance> instance;
    std::unique_ptr<fuse::renderer::VulkanDevice> device;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkPhysicalDevice vkPhysical = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    u32 queueFamily = 0;

    Context() = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        device.reset();
        if (messenger != VK_NULL_HANDLE && destroyMessenger != nullptr && instance != nullptr) {
            destroyMessenger(static_cast<VkInstance>(instance->nativeHandle()), messenger, nullptr);
        }
        instance.reset();
    }
};

/// Validation (with synchronization validation) + device. Returns 0, kSkip (no layer / ICD) or 1.
inline int setupContext(Context& ctx, const char* appName) {
    if (!layerAvailable(kValidationLayer)) {
        std::printf("SKIP: %s not installed\n", kValidationLayer);
        return kSkip;
    }
    setEnv("VK_INSTANCE_LAYERS", kValidationLayer);
    setEnv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT");
    setEnv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true");
    setEnv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK");

    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = appName;
    instanceDesc.enableValidation = true;
    ctx.instance = fuse::renderer::VulkanInstance::create(instanceDesc);
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
        std::fprintf(stderr, "FAIL: VK_EXT_debug_utils unavailable with validation enabled\n");
        return 1;
    }
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = onValidationMessage;
    createMessenger(vkInstance, &info, nullptr, &ctx.messenger);

    ctx.device = fuse::renderer::VulkanDevice::create(*ctx.instance);
    if (ctx.device == nullptr || !ctx.device->isValid()) {
        std::printf("SKIP: no Vulkan device\n");
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.vkPhysical = static_cast<VkPhysicalDevice>(ctx.device->nativePhysicalDevice());
    ctx.queue = static_cast<VkQueue>(ctx.device->queues().compute);
    ctx.queueFamily = ctx.device->queues().computeFamily;
    if (ctx.queue == VK_NULL_HANDLE) {
        ctx.queue = static_cast<VkQueue>(ctx.device->queues().graphics);
        ctx.queueFamily = ctx.device->queues().graphicsFamily;
    }
    return 0;
}

/// Host-visible, host-coherent storage buffer.
struct HostBuffer {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;

    HostBuffer() = default;
    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;
    ~HostBuffer() {
        if (device == VK_NULL_HANDLE) {
            return;
        }
        if (mapped != nullptr) {
            vkUnmapMemory(device, memory);
        }
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, memory, nullptr);
    }

    bool create(const Context& ctx, VkDeviceSize bytes) {
        device = ctx.vkDevice;
        size = bytes;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties props{};
        vkGetPhysicalDeviceMemoryProperties(ctx.vkPhysical, &props);
        const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        u32 typeIndex = UINT32_MAX;
        for (u32 i = 0; i < props.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) != 0u &&
                (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
                typeIndex = i;
                break;
            }
        }
        if (typeIndex == UINT32_MAX) {
            return false;
        }
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = typeIndex;
        if (vkAllocateMemory(device, &alloc, nullptr, &memory) != VK_SUCCESS) {
            return false;
        }
        vkBindBufferMemory(device, buffer, memory, 0);
        return vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS;
    }
};

struct BufferBinding {
    u32 binding = 0;
    const HostBuffer* buffer = nullptr;
};

/// Binds `bindings` (storage buffers, set 0 of `setLayout`), pushes `push`, dispatches `groupsX`
/// workgroups and waits; a compute -> host barrier makes the writes visible for readback.
inline bool dispatchCompute(const Context& ctx, VkPipeline pipeline, VkPipelineLayout layout,
                            VkDescriptorSetLayout setLayout, const std::vector<BufferBinding>& bindings,
                            const void* push, u32 pushBytes, u32 groupsX) {
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<u32>(bindings.size())};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(ctx.vkDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        return false;
    }
    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = pool;
    setAlloc.descriptorSetCount = 1;
    setAlloc.pSetLayouts = &setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(ctx.vkDevice, &setAlloc, &set);
    std::vector<VkDescriptorBufferInfo> infos(bindings.size());
    std::vector<VkWriteDescriptorSet> writes(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        infos[i] = VkDescriptorBufferInfo{bindings[i].buffer->buffer, 0, VK_WHOLE_SIZE};
        writes[i] = VkWriteDescriptorSet{};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = bindings[i].binding;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(ctx.vkDevice, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);

    VkCommandPoolCreateInfo cmdPoolInfo{};
    cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmdPoolInfo.queueFamilyIndex = ctx.queueFamily;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    vkCreateCommandPool(ctx.vkDevice, &cmdPoolInfo, nullptr, &cmdPool);
    VkCommandBufferAllocateInfo cmdAlloc{};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = cmdPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(ctx.vkDevice, &cmdAlloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    if (push != nullptr && pushBytes > 0u) {
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes, push);
    }
    vkCmdDispatch(cmd, groupsX, 1, 1);
    VkMemoryBarrier2 toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &toHost;
    vkCmdPipelineBarrier2(cmd, &dependency);
    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(ctx.vkDevice, &fenceInfo, nullptr, &fence);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    bool ok = vkQueueSubmit(ctx.queue, 1, &submit, fence) == VK_SUCCESS;
    ok = ok && vkWaitForFences(ctx.vkDevice, 1, &fence, VK_TRUE, 60ull * 1000000000ull) == VK_SUCCESS;
    vkDestroyFence(ctx.vkDevice, fence, nullptr);
    vkDestroyCommandPool(ctx.vkDevice, cmdPool, nullptr);
    vkDestroyDescriptorPool(ctx.vkDevice, pool, nullptr);
    return ok;
}

} // namespace rp_wp05

#endif // FUSE_VULKAN_BACKEND
