#pragma once

#include <fuse/types.hpp>

namespace fuse::renderer {

/// Name a Vulkan object via `vkSetDebugUtilsObjectNameEXT` when `VK_EXT_debug_utils` is loaded.
/// `vkObjectType` is `VkObjectType` numeric (buffer=9, image=10, deviceMemory=8).
/// Returns false if device/name/handle is null, the proc is missing, or the backend is stub.
bool setDebugObjectName(void* vkDevice, u32 vkObjectType, u64 handle, const char* name);

/// Pointer-handle convenience over `setDebugObjectName` (opaque `void*` Vulkan handles).
bool nameVkObject(void* vkDevice, u32 vkObjectType, const void* handle, const char* name);

/// `VkObjectType` numeric values used by the renderer (kept here so headers stay vulkan.h-free).
namespace vk_object_type {
constexpr u32 kSemaphore = 5;
constexpr u32 kCommandBuffer = 6;
constexpr u32 kFence = 7;
constexpr u32 kDeviceMemory = 8;
constexpr u32 kBuffer = 9;
constexpr u32 kImage = 10;
constexpr u32 kQueryPool = 12;
constexpr u32 kImageView = 14;
constexpr u32 kShaderModule = 15;
constexpr u32 kPipelineCache = 16;
constexpr u32 kPipelineLayout = 17;
constexpr u32 kRenderPass = 18;
constexpr u32 kPipeline = 19;
constexpr u32 kDescriptorSetLayout = 20;
constexpr u32 kSampler = 21;
constexpr u32 kDescriptorPool = 22;
constexpr u32 kDescriptorSet = 23;
constexpr u32 kFramebuffer = 24;
constexpr u32 kCommandPool = 25;
} // namespace vk_object_type

} // namespace fuse::renderer
