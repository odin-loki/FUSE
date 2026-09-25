#pragma once

// WP-0.4 internal: helpers shared by bindless.cpp (registry, descriptor-set backend) and
// bindless_descriptor_buffer.cpp (VK_EXT_descriptor_buffer backend). Vulkan builds only.

#if defined(FUSE_VULKAN_BACKEND)

#include <fuse/renderer/vk/bindless.hpp>

#include <vulkan/vulkan.h>

namespace fuse::renderer::bindless_detail {

struct HostVisibleBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    u64 address = 0;
};

/// Host-visible, host-coherent, persistently mapped buffer with SHADER_DEVICE_ADDRESS usage
/// (device-local preferred). Zero-filled. Raw vkAllocateMemory: stays out of VMA statistics.
bool createHostVisibleBuffer(VkDevice device, VkPhysicalDevice physicalDevice, VkDeviceSize size,
                             VkBufferUsageFlags usage, const char* debugName, HostVisibleBuffer& out);
void destroyHostVisibleBuffer(VkDevice device, HostVisibleBuffer& buffer);

/// True when the logical device has VK_EXT_descriptor_buffer enabled (RendererCaps) and the entry
/// points resolve.
bool descriptorBufferAvailable(const VulkanDevice& device);

/// Core (non update-after-bind) descriptor limits: they bind a layout created without
/// UPDATE_AFTER_BIND_POOL, which is every descriptor-buffer layout.
VulkanDescriptorLimits descriptorBufferLimits(const VulkanDevice& device);

/// Allocates and maps the descriptor buffer for `layout` (created with DESCRIPTOR_BUFFER_BIT),
/// records binding offsets, per-binding descriptor sizes and entry points into `state`.
bool createDescriptorBuffer(const VulkanDevice& device, VkDescriptorSetLayout layout, BindlessGpuHeapState& state);
void destroyDescriptorBuffer(VkDevice device, BindlessGpuHeapState& state);

/// Writes one descriptor (vkGetDescriptorEXT) at element `arrayIndex` of `binding`.
bool writeDescriptorBufferEntry(VkDevice device, const BindlessGpuHeapState& state, u32 binding, u32 arrayIndex,
                                const VkDescriptorGetInfoEXT& info);

/// vkCmdBindDescriptorBuffersEXT (buffer 0) + vkCmdSetDescriptorBufferOffsetsEXT(set, offset 0).
void bindDescriptorBuffer(const BindlessGpuHeapState& state, VkCommandBuffer cmd, VkPipelineBindPoint bindPoint,
                          VkPipelineLayout layout, u32 setIndex);

} // namespace fuse::renderer::bindless_detail

#endif
