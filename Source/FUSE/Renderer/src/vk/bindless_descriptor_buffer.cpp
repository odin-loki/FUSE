// WP-0.4 / WP-0.4b: VK_EXT_descriptor_buffer backend of the bindless heap, plus the host-visible
// buffer helper both backends use for the buffer-address table.
//
// The bindless set layout is created with VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT
// (bindless.cpp). One host-visible, persistently mapped buffer with RESOURCE + SAMPLER descriptor
// buffer usage holds the whole set; element i of binding b lives at
// vkGetDescriptorSetLayoutBindingOffsetEXT(b) + i * descriptorSize(type(b)). Descriptors are
// written with vkGetDescriptorEXT straight into the mapping (no vkUpdateDescriptorSets, no pool).
// Slot reuse is made safe by BindlessDescriptors::retireSlot/collectRetired: a slot's bytes are
// only rewritten once no submitted work can read them.

#include "bindless_internal.hpp"

#if defined(FUSE_VULKAN_BACKEND)

#include <fuse/renderer/vk/debug_utils.hpp>

#include <algorithm>
#include <cstring>
#include <string>

namespace fuse::renderer::bindless_detail {

namespace {

u32 findHostVisibleMemoryType(VkPhysicalDevice physicalDevice, u32 typeBits) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &props);
    const VkMemoryPropertyFlags required =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    // Prefer device-local host-visible memory (ReBAR / UMA): shaders read descriptors from it.
    for (int pass = 0; pass < 2; ++pass) {
        const VkMemoryPropertyFlags wanted =
            pass == 0 ? (required | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) : required;
        for (u32 i = 0; i < props.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) != 0u && (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
                return i;
            }
        }
    }
    return UINT32_MAX;
}

#if defined(VK_EXT_descriptor_buffer)
bool queryDescriptorBufferProperties(const VulkanDevice& device, VkPhysicalDeviceDescriptorBufferPropertiesEXT& out) {
    auto physicalDevice = static_cast<VkPhysicalDevice>(device.nativePhysicalDevice());
    if (physicalDevice == VK_NULL_HANDLE) {
        return false;
    }
    out = VkPhysicalDeviceDescriptorBufferPropertiesEXT{};
    out.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &out;
    vkGetPhysicalDeviceProperties2(physicalDevice, &props2);
    return true;
}
#endif

} // namespace

bool createHostVisibleBuffer(VkDevice device, VkPhysicalDevice physicalDevice, VkDeviceSize size,
                             VkBufferUsageFlags usage, const char* debugName, HostVisibleBuffer& out) {
    out = HostVisibleBuffer{};
    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || size == 0u) {
        return false;
    }
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &out.buffer) != VK_SUCCESS) {
        out.buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
    const u32 memoryType = findHostVisibleMemoryType(physicalDevice, requirements.memoryTypeBits);
    if (memoryType == UINT32_MAX) {
        destroyHostVisibleBuffer(device, out);
        return false;
    }

    VkMemoryAllocateFlagsInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &flagsInfo;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &out.memory) != VK_SUCCESS) {
        out.memory = VK_NULL_HANDLE;
        destroyHostVisibleBuffer(device, out);
        return false;
    }
    if (vkBindBufferMemory(device, out.buffer, out.memory, 0) != VK_SUCCESS ||
        vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) != VK_SUCCESS) {
        out.mapped = nullptr;
        destroyHostVisibleBuffer(device, out);
        return false;
    }
    std::memset(out.mapped, 0, static_cast<size_t>(size));

    VkBufferDeviceAddressInfo addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addressInfo.buffer = out.buffer;
    out.address = static_cast<u64>(vkGetBufferDeviceAddress(device, &addressInfo));
    if (debugName != nullptr) {
        nameVkObject(device, vk_object_type::kBuffer, static_cast<void*>(out.buffer), debugName);
        // fuse_b5_rhi_object_names: every vkAllocateMemory result carries a name too.
        const std::string memoryName = std::string(debugName) + ".memory";
        nameVkObject(device, vk_object_type::kDeviceMemory, static_cast<void*>(out.memory), memoryName.c_str());
    }
    return true;
}

void destroyHostVisibleBuffer(VkDevice device, HostVisibleBuffer& buffer) {
    if (device != VK_NULL_HANDLE) {
        if (buffer.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, buffer.buffer, nullptr);
        }
        if (buffer.memory != VK_NULL_HANDLE) {
            if (buffer.mapped != nullptr) {
                vkUnmapMemory(device, buffer.memory);
            }
            vkFreeMemory(device, buffer.memory, nullptr);
        }
    }
    buffer = HostVisibleBuffer{};
}

bool descriptorBufferAvailable(const VulkanDevice& device) {
#if defined(VK_EXT_descriptor_buffer)
    if (!device.isValid() || !device.info().caps.descriptorBuffer || !device.info().caps.bufferDeviceAddress) {
        return false;
    }
    auto vkDevice = static_cast<VkDevice>(device.nativeHandle());
    return vkGetDeviceProcAddr(vkDevice, "vkGetDescriptorEXT") != nullptr &&
           vkGetDeviceProcAddr(vkDevice, "vkGetDescriptorSetLayoutSizeEXT") != nullptr &&
           vkGetDeviceProcAddr(vkDevice, "vkGetDescriptorSetLayoutBindingOffsetEXT") != nullptr &&
           vkGetDeviceProcAddr(vkDevice, "vkCmdBindDescriptorBuffersEXT") != nullptr &&
           vkGetDeviceProcAddr(vkDevice, "vkCmdSetDescriptorBufferOffsetsEXT") != nullptr;
#else
    (void)device;
    return false;
#endif
}

VulkanDescriptorLimits descriptorBufferLimits(const VulkanDevice& device) {
    VulkanDescriptorLimits limits{};
    auto physicalDevice = static_cast<VkPhysicalDevice>(device.nativePhysicalDevice());
    if (physicalDevice == VK_NULL_HANDLE) {
        return limits;
    }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    const VkPhysicalDeviceLimits& l = props.limits;
    limits.sampledImages = std::min(l.maxPerStageDescriptorSampledImages, l.maxDescriptorSetSampledImages);
    limits.storageImages = std::min(l.maxPerStageDescriptorStorageImages, l.maxDescriptorSetStorageImages);
    // One storage buffer of the per-stage budget belongs to the address table (binding 5).
    limits.storageBuffers = std::min(l.maxPerStageDescriptorStorageBuffers, l.maxDescriptorSetStorageBuffers);
    limits.uniformBuffers = std::min(l.maxPerStageDescriptorUniformBuffers, l.maxDescriptorSetUniformBuffers);
    limits.samplers = std::min(l.maxPerStageDescriptorSamplers, l.maxDescriptorSetSamplers);
    limits.perStageResources = l.maxPerStageResources;
    limits.allPools = 0; // no pool on this backend
    return limits;
}

bool createDescriptorBuffer(const VulkanDevice& device, VkDescriptorSetLayout layout, BindlessGpuHeapState& state) {
#if defined(VK_EXT_descriptor_buffer)
    if (!descriptorBufferAvailable(device) || layout == VK_NULL_HANDLE) {
        return false;
    }
    auto vkDevice = static_cast<VkDevice>(device.nativeHandle());
    auto physicalDevice = static_cast<VkPhysicalDevice>(device.nativePhysicalDevice());
    auto getLayoutSize = reinterpret_cast<PFN_vkGetDescriptorSetLayoutSizeEXT>(
        vkGetDeviceProcAddr(vkDevice, "vkGetDescriptorSetLayoutSizeEXT"));
    auto getBindingOffset = reinterpret_cast<PFN_vkGetDescriptorSetLayoutBindingOffsetEXT>(
        vkGetDeviceProcAddr(vkDevice, "vkGetDescriptorSetLayoutBindingOffsetEXT"));

    VkPhysicalDeviceDescriptorBufferPropertiesEXT props{};
    if (!queryDescriptorBufferProperties(device, props)) {
        return false;
    }

    VkDeviceSize layoutSize = 0;
    getLayoutSize(vkDevice, layout, &layoutSize);
    // One buffer carries both resource and sampler descriptors, so it must fit both ranges.
    const VkDeviceSize maxRange = std::min(props.maxResourceDescriptorBufferRange, props.maxSamplerDescriptorBufferRange);
    const VkDeviceSize addressSpace =
        std::min(props.samplerDescriptorBufferAddressSpaceSize, props.resourceDescriptorBufferAddressSpaceSize);
    if (layoutSize == 0u || layoutSize > maxRange || layoutSize > addressSpace ||
        props.maxDescriptorBufferBindings < 1u || props.maxSamplerDescriptorBufferBindings < 1u ||
        props.maxResourceDescriptorBufferBindings < 1u) {
        return false;
    }

    // robustBufferAccess is not enabled by VulkanDevice, so the non-robust buffer sizes apply.
    state.descriptorSizes[kBindlessBindingStorageImages] = static_cast<u32>(props.storageImageDescriptorSize);
    state.descriptorSizes[kBindlessBindingSampledImages] = static_cast<u32>(props.sampledImageDescriptorSize);
    state.descriptorSizes[kBindlessBindingSamplers] = static_cast<u32>(props.samplerDescriptorSize);
    state.descriptorSizes[kBindlessBindingStorageBuffers] = static_cast<u32>(props.storageBufferDescriptorSize);
    state.descriptorSizes[kBindlessBindingUniformBuffers] = static_cast<u32>(props.uniformBufferDescriptorSize);
    state.descriptorSizes[kBindlessBindingBufferAddressTable] = static_cast<u32>(props.storageBufferDescriptorSize);
    for (u32 binding = 0; binding < kBindlessBindingCount; ++binding) {
        VkDeviceSize offset = 0;
        getBindingOffset(vkDevice, layout, binding, &offset);
        state.bindingOffsets[binding] = static_cast<u64>(offset);
    }

    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT;
    HostVisibleBuffer buffer{};
    if (!createHostVisibleBuffer(vkDevice, physicalDevice, layoutSize, usage, "fuse.bindless.descriptor_buffer",
                                 buffer)) {
        return false;
    }
    if (props.descriptorBufferOffsetAlignment > 0u && (buffer.address % props.descriptorBufferOffsetAlignment) != 0u) {
        destroyHostVisibleBuffer(vkDevice, buffer);
        return false;
    }
    state.descriptorBuffer = buffer.buffer;
    state.descriptorMemory = buffer.memory;
    state.descriptorMapped = static_cast<u8*>(buffer.mapped);
    state.descriptorBufferAddress = buffer.address;
    state.descriptorBufferSize = static_cast<u64>(layoutSize);
    state.descriptorBufferUsage = static_cast<u32>(usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    state.fnGetDescriptor = reinterpret_cast<void*>(vkGetDeviceProcAddr(vkDevice, "vkGetDescriptorEXT"));
    state.fnBindDescriptorBuffers = reinterpret_cast<void*>(vkGetDeviceProcAddr(vkDevice, "vkCmdBindDescriptorBuffersEXT"));
    state.fnSetDescriptorBufferOffsets =
        reinterpret_cast<void*>(vkGetDeviceProcAddr(vkDevice, "vkCmdSetDescriptorBufferOffsetsEXT"));
    return true;
#else
    (void)device;
    (void)layout;
    (void)state;
    return false;
#endif
}

void destroyDescriptorBuffer(VkDevice device, BindlessGpuHeapState& state) {
    HostVisibleBuffer buffer{};
    buffer.buffer = static_cast<VkBuffer>(state.descriptorBuffer);
    buffer.memory = static_cast<VkDeviceMemory>(state.descriptorMemory);
    buffer.mapped = state.descriptorMapped;
    destroyHostVisibleBuffer(device, buffer);
    state.descriptorBuffer = nullptr;
    state.descriptorMemory = nullptr;
    state.descriptorMapped = nullptr;
    state.descriptorBufferAddress = 0;
    state.descriptorBufferSize = 0;
    state.descriptorBufferUsage = 0;
    std::fill(std::begin(state.bindingOffsets), std::end(state.bindingOffsets), u64{0});
    std::fill(std::begin(state.descriptorSizes), std::end(state.descriptorSizes), 0u);
    state.fnGetDescriptor = nullptr;
    state.fnBindDescriptorBuffers = nullptr;
    state.fnSetDescriptorBufferOffsets = nullptr;
}

bool writeDescriptorBufferEntry(VkDevice device, const BindlessGpuHeapState& state, u32 binding, u32 arrayIndex,
                                const VkDescriptorGetInfoEXT& info) {
#if defined(VK_EXT_descriptor_buffer)
    if (state.descriptorMapped == nullptr || state.fnGetDescriptor == nullptr || binding >= kBindlessBindingCount) {
        return false;
    }
    const u64 size = state.descriptorSizes[binding];
    const u64 offset = state.bindingOffsets[binding] + static_cast<u64>(arrayIndex) * size;
    if (size == 0u || offset + size > state.descriptorBufferSize) {
        return false;
    }
    auto getDescriptor = reinterpret_cast<PFN_vkGetDescriptorEXT>(state.fnGetDescriptor);
    getDescriptor(device, &info, static_cast<size_t>(size), state.descriptorMapped + offset);
    return true;
#else
    (void)device;
    (void)state;
    (void)binding;
    (void)arrayIndex;
    (void)info;
    return false;
#endif
}

void bindDescriptorBuffer(const BindlessGpuHeapState& state, VkCommandBuffer cmd, VkPipelineBindPoint bindPoint,
                          VkPipelineLayout layout, u32 setIndex) {
#if defined(VK_EXT_descriptor_buffer)
    if (cmd == VK_NULL_HANDLE || state.fnBindDescriptorBuffers == nullptr || state.fnSetDescriptorBufferOffsets == nullptr) {
        return;
    }
    VkDescriptorBufferBindingInfoEXT binding{};
    binding.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT;
    binding.address = state.descriptorBufferAddress;
    binding.usage = static_cast<VkBufferUsageFlags>(state.descriptorBufferUsage);
    reinterpret_cast<PFN_vkCmdBindDescriptorBuffersEXT>(state.fnBindDescriptorBuffers)(cmd, 1, &binding);
    const u32 bufferIndex = 0;
    const VkDeviceSize offset = 0;
    reinterpret_cast<PFN_vkCmdSetDescriptorBufferOffsetsEXT>(state.fnSetDescriptorBufferOffsets)(
        cmd, bindPoint, layout, setIndex, 1, &bufferIndex, &offset);
#else
    (void)state;
    (void)cmd;
    (void)bindPoint;
    (void)layout;
    (void)setIndex;
#endif
}

} // namespace fuse::renderer::bindless_detail

#endif
