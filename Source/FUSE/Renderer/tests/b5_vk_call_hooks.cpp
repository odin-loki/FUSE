// See b5_vk_call_hooks.hpp. Link this TU into exactly one test executable at a time.
#include "b5_vk_call_hooks.hpp"

#include <cstring>
#include <map>
#include <utility>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace b5hooks {
namespace {

CmdCounters g_counters;
std::vector<ObjectRecord> g_objects;
std::map<std::pair<std::uint32_t, std::uint64_t>, std::size_t> g_live; // (type, handle) -> g_objects
bool g_capturing = false;
std::uint32_t g_namesApplied = 0;
std::uint32_t g_currentPush = UINT32_MAX;

} // namespace

#if defined(FUSE_VULKAN_BACKEND)
namespace detail {

void* nextSymbol(const char* name) {
#if defined(_WIN32)
    // PE has no RTLD_NEXT: the hooks defined here win over the vulkan-1.lib import stubs at link
    // time, and the real entry points are the loader DLL's exports.
    static const HMODULE loader = ::LoadLibraryA("vulkan-1.dll");
    return loader != nullptr ? reinterpret_cast<void*>(::GetProcAddress(loader, name)) : nullptr;
#else
    void* fn = dlsym(RTLD_NEXT, name);
    return fn;
#endif
}

void recordCreate(std::uint32_t type, std::uint64_t handle, const char* createdBy) {
    if (handle == 0) {
        return;
    }
    const auto key = std::make_pair(type, handle);
    if (!g_capturing) {
        g_live.erase(key);
        return;
    }
    ObjectRecord record;
    record.type = type;
    record.handle = handle;
    record.createdBy = createdBy;
    g_objects.push_back(record);
    g_live[key] = g_objects.size() - 1u;
}

void recordDestroy(std::uint32_t type, std::uint64_t handle) {
    if (handle != 0) {
        g_live.erase(std::make_pair(type, handle));
    }
}

void recordName(std::uint32_t type, std::uint64_t handle, const char* name) {
    ++g_namesApplied;
    const auto it = g_live.find(std::make_pair(type, handle));
    if (it == g_live.end()) {
        return;
    }
    ObjectRecord& record = g_objects[it->second];
    record.named = name != nullptr && name[0] != '\0';
    record.name = name != nullptr ? name : "";
}

template <typename T>
std::uint64_t h64(T handle) {
    std::uint64_t value = 0;
    static_assert(sizeof(T) <= sizeof(value), "handle wider than 64 bits");
    std::memcpy(&value, &handle, sizeof(T));
    return value;
}

} // namespace detail
#endif

bool available() {
#if defined(FUSE_VULKAN_BACKEND)
    return true;
#else
    return false;
#endif
}

void resetCmdCounters() {
    g_counters = CmdCounters{};
    g_currentPush = UINT32_MAX;
}

const CmdCounters& cmdCounters() {
    return g_counters;
}

void beginObjectCapture() {
    g_objects.clear();
    g_live.clear();
    g_capturing = true;
}

void endObjectCapture() {
    g_capturing = false;
}

const std::vector<ObjectRecord>& capturedObjects() {
    return g_objects;
}

std::uint32_t namesApplied() {
    return g_namesApplied;
}

} // namespace b5hooks

#if defined(FUSE_VULKAN_BACKEND)

using b5hooks::detail::h64;
using b5hooks::detail::nextSymbol;
using b5hooks::detail::recordCreate;
using b5hooks::detail::recordDestroy;

#define B5_NEXT(fnName) \
    static const auto next_##fnName = reinterpret_cast<PFN_##fnName>(nextSymbol(#fnName))

// ---- Debug names -------------------------------------------------------------------------------

namespace {

PFN_vkSetDebugUtilsObjectNameEXT g_realSetName = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL hookSetDebugUtilsObjectNameEXT(VkDevice device,
                                                              const VkDebugUtilsObjectNameInfoEXT* info) {
    if (info != nullptr) {
        b5hooks::detail::recordName(static_cast<std::uint32_t>(info->objectType), info->objectHandle,
                                    info->pObjectName);
    }
    return g_realSetName != nullptr ? g_realSetName(device, info) : VK_SUCCESS;
}

} // namespace

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    B5_NEXT(vkGetDeviceProcAddr);
    PFN_vkVoidFunction fn = next_vkGetDeviceProcAddr(device, pName);
    if (fn != nullptr && pName != nullptr && std::strcmp(pName, "vkSetDebugUtilsObjectNameEXT") == 0) {
        g_realSetName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(fn);
        return reinterpret_cast<PFN_vkVoidFunction>(&hookSetDebugUtilsObjectNameEXT);
    }
    return fn;
}

// ---- Object creation / destruction ------------------------------------------------------------

#define B5_CREATE_DESTROY(Type, TypeEnum, CreateFn, DestroyFn, CreateInfoT)                               \
    VKAPI_ATTR VkResult VKAPI_CALL CreateFn(VkDevice device, const CreateInfoT* pCreateInfo,              \
                                            const VkAllocationCallbacks* pAllocator, Type* pOut) {        \
        B5_NEXT(CreateFn);                                                                                 \
        const VkResult result = next_##CreateFn(device, pCreateInfo, pAllocator, pOut);                    \
        if (result == VK_SUCCESS && pOut != nullptr) {                                                     \
            recordCreate(TypeEnum, h64(*pOut), #CreateFn);                                                 \
        }                                                                                                  \
        return result;                                                                                     \
    }                                                                                                      \
    VKAPI_ATTR void VKAPI_CALL DestroyFn(VkDevice device, Type object, const VkAllocationCallbacks* pAllocator) { \
        B5_NEXT(DestroyFn);                                                                                \
        recordDestroy(TypeEnum, h64(object));                                                              \
        next_##DestroyFn(device, object, pAllocator);                                                      \
    }

B5_CREATE_DESTROY(VkBuffer, VK_OBJECT_TYPE_BUFFER, vkCreateBuffer, vkDestroyBuffer, VkBufferCreateInfo)
B5_CREATE_DESTROY(VkImage, VK_OBJECT_TYPE_IMAGE, vkCreateImage, vkDestroyImage, VkImageCreateInfo)
B5_CREATE_DESTROY(VkImageView, VK_OBJECT_TYPE_IMAGE_VIEW, vkCreateImageView, vkDestroyImageView,
                  VkImageViewCreateInfo)
B5_CREATE_DESTROY(VkSampler, VK_OBJECT_TYPE_SAMPLER, vkCreateSampler, vkDestroySampler, VkSamplerCreateInfo)
B5_CREATE_DESTROY(VkShaderModule, VK_OBJECT_TYPE_SHADER_MODULE, vkCreateShaderModule, vkDestroyShaderModule,
                  VkShaderModuleCreateInfo)
B5_CREATE_DESTROY(VkPipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT, vkCreatePipelineLayout,
                  vkDestroyPipelineLayout, VkPipelineLayoutCreateInfo)
B5_CREATE_DESTROY(VkRenderPass, VK_OBJECT_TYPE_RENDER_PASS, vkCreateRenderPass, vkDestroyRenderPass,
                  VkRenderPassCreateInfo)
B5_CREATE_DESTROY(VkFramebuffer, VK_OBJECT_TYPE_FRAMEBUFFER, vkCreateFramebuffer, vkDestroyFramebuffer,
                  VkFramebufferCreateInfo)
B5_CREATE_DESTROY(VkDescriptorSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, vkCreateDescriptorSetLayout,
                  vkDestroyDescriptorSetLayout, VkDescriptorSetLayoutCreateInfo)
B5_CREATE_DESTROY(VkDescriptorPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL, vkCreateDescriptorPool,
                  vkDestroyDescriptorPool, VkDescriptorPoolCreateInfo)
B5_CREATE_DESTROY(VkCommandPool, VK_OBJECT_TYPE_COMMAND_POOL, vkCreateCommandPool, vkDestroyCommandPool,
                  VkCommandPoolCreateInfo)
B5_CREATE_DESTROY(VkFence, VK_OBJECT_TYPE_FENCE, vkCreateFence, vkDestroyFence, VkFenceCreateInfo)
B5_CREATE_DESTROY(VkSemaphore, VK_OBJECT_TYPE_SEMAPHORE, vkCreateSemaphore, vkDestroySemaphore,
                  VkSemaphoreCreateInfo)
B5_CREATE_DESTROY(VkQueryPool, VK_OBJECT_TYPE_QUERY_POOL, vkCreateQueryPool, vkDestroyQueryPool,
                  VkQueryPoolCreateInfo)
B5_CREATE_DESTROY(VkPipelineCache, VK_OBJECT_TYPE_PIPELINE_CACHE, vkCreatePipelineCache,
                  vkDestroyPipelineCache, VkPipelineCacheCreateInfo)

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo,
                                                const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory) {
    B5_NEXT(vkAllocateMemory);
    const VkResult result = next_vkAllocateMemory(device, pAllocateInfo, pAllocator, pMemory);
    if (result == VK_SUCCESS && pMemory != nullptr) {
        recordCreate(VK_OBJECT_TYPE_DEVICE_MEMORY, h64(*pMemory), "vkAllocateMemory");
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory,
                                        const VkAllocationCallbacks* pAllocator) {
    B5_NEXT(vkFreeMemory);
    recordDestroy(VK_OBJECT_TYPE_DEVICE_MEMORY, h64(memory));
    next_vkFreeMemory(device, memory, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                         const VkGraphicsPipelineCreateInfo* pCreateInfos,
                                                         const VkAllocationCallbacks* pAllocator,
                                                         VkPipeline* pPipelines) {
    B5_NEXT(vkCreateGraphicsPipelines);
    const VkResult result = next_vkCreateGraphicsPipelines(device, cache, count, pCreateInfos, pAllocator, pPipelines);
    if (result == VK_SUCCESS && pPipelines != nullptr) {
        for (uint32_t i = 0; i < count; ++i) {
            recordCreate(VK_OBJECT_TYPE_PIPELINE, h64(pPipelines[i]), "vkCreateGraphicsPipelines");
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                        const VkComputePipelineCreateInfo* pCreateInfos,
                                                        const VkAllocationCallbacks* pAllocator,
                                                        VkPipeline* pPipelines) {
    B5_NEXT(vkCreateComputePipelines);
    const VkResult result = next_vkCreateComputePipelines(device, cache, count, pCreateInfos, pAllocator, pPipelines);
    if (result == VK_SUCCESS && pPipelines != nullptr) {
        for (uint32_t i = 0; i < count; ++i) {
            recordCreate(VK_OBJECT_TYPE_PIPELINE, h64(pPipelines[i]), "vkCreateComputePipelines");
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice device, VkPipeline pipeline,
                                             const VkAllocationCallbacks* pAllocator) {
    B5_NEXT(vkDestroyPipeline);
    recordDestroy(VK_OBJECT_TYPE_PIPELINE, h64(pipeline));
    next_vkDestroyPipeline(device, pipeline, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* pInfo,
                                                        VkCommandBuffer* pBuffers) {
    B5_NEXT(vkAllocateCommandBuffers);
    const VkResult result = next_vkAllocateCommandBuffers(device, pInfo, pBuffers);
    if (result == VK_SUCCESS && pInfo != nullptr && pBuffers != nullptr) {
        for (uint32_t i = 0; i < pInfo->commandBufferCount; ++i) {
            recordCreate(VK_OBJECT_TYPE_COMMAND_BUFFER, h64(pBuffers[i]), "vkAllocateCommandBuffers");
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count,
                                                const VkCommandBuffer* pBuffers) {
    B5_NEXT(vkFreeCommandBuffers);
    for (uint32_t i = 0; pBuffers != nullptr && i < count; ++i) {
        recordDestroy(VK_OBJECT_TYPE_COMMAND_BUFFER, h64(pBuffers[i]));
    }
    next_vkFreeCommandBuffers(device, pool, count, pBuffers);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* pInfo,
                                                        VkDescriptorSet* pSets) {
    B5_NEXT(vkAllocateDescriptorSets);
    const VkResult result = next_vkAllocateDescriptorSets(device, pInfo, pSets);
    if (result == VK_SUCCESS && pInfo != nullptr && pSets != nullptr) {
        for (uint32_t i = 0; i < pInfo->descriptorSetCount; ++i) {
            recordCreate(VK_OBJECT_TYPE_DESCRIPTOR_SET, h64(pSets[i]), "vkAllocateDescriptorSets");
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice device, VkDescriptorPool pool, uint32_t count,
                                                    const VkDescriptorSet* pSets) {
    B5_NEXT(vkFreeDescriptorSets);
    for (uint32_t i = 0; pSets != nullptr && i < count; ++i) {
        recordDestroy(VK_OBJECT_TYPE_DESCRIPTOR_SET, h64(pSets[i]));
    }
    return next_vkFreeDescriptorSets(device, pool, count, pSets);
}

// ---- Command recording ------------------------------------------------------------------------

VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint,
                                             VkPipeline pipeline) {
    B5_NEXT(vkCmdBindPipeline);
    if (bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        ++b5hooks::g_counters.bindPipelineGraphics;
        b5hooks::g_counters.graphicsPipelines.push_back(h64(pipeline));
    } else if (bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
        ++b5hooks::g_counters.bindPipelineCompute;
    }
    next_vkCmdBindPipeline(cmd, bindPoint, pipeline);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer cmd, uint32_t firstBinding, uint32_t bindingCount,
                                                  const VkBuffer* pBuffers, const VkDeviceSize* pOffsets) {
    B5_NEXT(vkCmdBindVertexBuffers);
    ++b5hooks::g_counters.bindVertexBuffers;
    next_vkCmdBindVertexBuffers(cmd, firstBinding, bindingCount, pBuffers, pOffsets);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset,
                                                VkIndexType indexType) {
    B5_NEXT(vkCmdBindIndexBuffer);
    ++b5hooks::g_counters.bindIndexBuffer;
    next_vkCmdBindIndexBuffer(cmd, buffer, offset, indexType);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint,
                                                   VkPipelineLayout layout, uint32_t firstSet, uint32_t setCount,
                                                   const VkDescriptorSet* pSets, uint32_t dynamicCount,
                                                   const uint32_t* pDynamicOffsets) {
    B5_NEXT(vkCmdBindDescriptorSets);
    ++b5hooks::g_counters.bindDescriptorSets;
    next_vkCmdBindDescriptorSets(cmd, bindPoint, layout, firstSet, setCount, pSets, dynamicCount, pDynamicOffsets);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout layout,
                                              VkShaderStageFlags stages, uint32_t offset, uint32_t size,
                                              const void* pValues) {
    B5_NEXT(vkCmdPushConstants);
    ++b5hooks::g_counters.pushConstants;
    std::uint32_t first = 0;
    if (pValues != nullptr && size >= sizeof(first)) {
        std::memcpy(&first, pValues, sizeof(first));
    }
    b5hooks::g_counters.pushFirstWords.push_back(first);
    if (offset == 0) {
        b5hooks::g_currentPush = first;
    }
    next_vkCmdPushConstants(cmd, layout, stages, offset, size, pValues);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer cmd, uint32_t indexCount, uint32_t instanceCount,
                                            uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    B5_NEXT(vkCmdDrawIndexed);
    ++b5hooks::g_counters.drawIndexed;
    b5hooks::g_counters.drawIndexedMaterial.push_back(b5hooks::g_currentPush);
    next_vkCmdDrawIndexed(cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer cmd, uint32_t vertexCount, uint32_t instanceCount,
                                     uint32_t firstVertex, uint32_t firstInstance) {
    B5_NEXT(vkCmdDraw);
    ++b5hooks::g_counters.draw;
    next_vkCmdDraw(cmd, vertexCount, instanceCount, firstVertex, firstInstance);
}

} // extern "C"

#endif // FUSE_VULKAN_BACKEND
