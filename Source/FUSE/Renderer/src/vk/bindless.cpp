#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/debug_utils.hpp>

#include <algorithm>
#include <cstring>

#if defined(FUSE_VULKAN_BACKEND)
#include "bindless_internal.hpp"

#include <vulkan/vulkan.h>

#include <array>
#endif

namespace fuse::renderer {

namespace {

#if defined(FUSE_VULKAN_BACKEND)
VkDescriptorSetLayoutBinding makeBinding(u32 binding, VkDescriptorType type, u32 count, VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding layoutBinding{};
    layoutBinding.binding = binding;
    layoutBinding.descriptorType = type;
    layoutBinding.descriptorCount = count;
    layoutBinding.stageFlags = stages;
    return layoutBinding;
}

/// Stage mask of the bindless bindings. The descriptor-set backend keeps VK_SHADER_STAGE_ALL (the
/// B2 contract). The descriptor-buffer backend lists only real stages the device can run:
/// Lavapipe (Mesa 25.2) walks every bit of a descriptor-buffer set layout's stage mask in
/// vkCmdSetDescriptorBufferOffsetsEXT and maps it to a shader stage index
/// (vk_to_mesa_shader_stage = ffs - 1). The undefined high bits of VK_SHADER_STAGE_ALL
/// (0x7FFFFFFF) index past its per-stage constant-buffer table, corrupting driver state: shaders
/// then read the set from a wrong base and crash or write through garbage descriptors.
VkShaderStageFlags bindlessStageFlags(const VulkanDevice& device, BindlessBackend backend) {
    if (backend != BindlessBackend::DescriptorBuffer) {
        return VK_SHADER_STAGE_ALL;
    }
    const RendererCaps& caps = device.info().caps;
    VkShaderStageFlags stages = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
#if defined(VK_EXT_mesh_shader)
    if (caps.taskShader) {
        stages |= VK_SHADER_STAGE_TASK_BIT_EXT;
    }
    if (caps.meshShader) {
        stages |= VK_SHADER_STAGE_MESH_BIT_EXT;
    }
#endif
#if defined(VK_KHR_ray_tracing_pipeline)
    if (caps.rayTracingPipeline) {
        stages |= VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                  VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                  VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR;
    }
#endif
    (void)caps;
    return stages;
}

/// DescriptorSet backend: UPDATE_AFTER_BIND pool + layout + one set. DescriptorBuffer backend:
/// layout only (DESCRIPTOR_BUFFER_BIT, no update-after-bind flags, no pool / set).
bool createVulkanBindlessDescriptors(const VulkanDevice& device, const BindlessArraySizes& sizes,
                                     BindlessBackend backend, void*& outPool, void*& outLayout, void*& outSet) {
    if (!device.isValid() || backend == BindlessBackend::None) {
        return false;
    }

    auto vkDevice = static_cast<VkDevice>(device.nativeHandle());
    const bool descriptorBuffer = backend == BindlessBackend::DescriptorBuffer;

    const VkShaderStageFlags stages = bindlessStageFlags(device, backend);
    std::array<VkDescriptorSetLayoutBinding, kBindlessBindingCount> bindings = {
        makeBinding(kBindlessBindingStorageImages, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizes.storageImages, stages),
        makeBinding(kBindlessBindingSampledImages, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sizes.sampledImages, stages),
        makeBinding(kBindlessBindingSamplers, VK_DESCRIPTOR_TYPE_SAMPLER, sizes.samplers, stages),
        makeBinding(kBindlessBindingStorageBuffers, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sizes.storageBuffers, stages),
        makeBinding(kBindlessBindingUniformBuffers, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sizes.uniformBuffers, stages),
        makeBinding(kBindlessBindingBufferAddressTable, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, stages),
    };

    // UPDATE_AFTER_BIND is only legal per descriptor type the device enabled (VUID-03005/03007...),
    // and never on a descriptor-buffer layout.
    const VulkanDeviceInfo& features = device.info();
    auto flagsFor = [descriptorBuffer](bool updateAfterBind) -> VkDescriptorBindingFlags {
        return VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
               (updateAfterBind && !descriptorBuffer
                    ? VkDescriptorBindingFlags{VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT}
                    : VkDescriptorBindingFlags{0});
    };
    std::array<VkDescriptorBindingFlags, kBindlessBindingCount> bindingFlags = {
        flagsFor(features.storageImageUpdateAfterBind),
        flagsFor(features.sampledImageUpdateAfterBind),
        flagsFor(features.sampledImageUpdateAfterBind),
        flagsFor(features.storageBufferUpdateAfterBind),
        flagsFor(features.uniformBufferUpdateAfterBind),
        flagsFor(features.storageBufferUpdateAfterBind),
    };

    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
    bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    bindingFlagsInfo.bindingCount = static_cast<u32>(bindingFlags.size());
    bindingFlagsInfo.pBindingFlags = bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
#if defined(VK_EXT_descriptor_buffer)
    layoutInfo.flags = descriptorBuffer ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT
                                        : VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
#else
    if (descriptorBuffer) {
        return false;
    }
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
#endif
    layoutInfo.bindingCount = static_cast<u32>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    layoutInfo.pNext = &bindingFlagsInfo;

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(vkDevice, &layoutInfo, nullptr, &layout) != VK_SUCCESS) {
        return false;
    }
    nameVkObject(vkDevice, vk_object_type::kDescriptorSetLayout, static_cast<void*>(layout), "fuse.bindless.layout");

    if (descriptorBuffer) {
        outPool = nullptr;
        outLayout = layout;
        outSet = nullptr;
        return true;
    }

    std::array<VkDescriptorPoolSize, 5> poolSizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizes.storageImages},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sizes.sampledImages},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, sizes.samplers},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sizes.storageBuffers + 1u},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sizes.uniformBuffers},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<u32>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(vkDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(vkDevice, layout, nullptr);
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vkDevice, &allocInfo, &set) != VK_SUCCESS) {
        vkDestroyDescriptorPool(vkDevice, pool, nullptr);
        vkDestroyDescriptorSetLayout(vkDevice, layout, nullptr);
        return false;
    }

    outPool = pool;
    outLayout = layout;
    outSet = set;
    nameVkObject(vkDevice, vk_object_type::kDescriptorPool, static_cast<void*>(pool), "fuse.bindless.pool");
    nameVkObject(vkDevice, vk_object_type::kDescriptorSet, static_cast<void*>(set), "fuse.bindless.set");
    return true;
}

void destroyVulkanBindlessDescriptors(const VulkanDevice& device, void*& pool, void*& layout, void*& set) {
    if (!device.isValid()) {
        pool = nullptr;
        layout = nullptr;
        set = nullptr;
        return;
    }

    auto vkDevice = static_cast<VkDevice>(device.nativeHandle());
    if (pool != nullptr) {
        vkDestroyDescriptorPool(vkDevice, static_cast<VkDescriptorPool>(pool), nullptr);
    }
    if (layout != nullptr) {
        vkDestroyDescriptorSetLayout(vkDevice, static_cast<VkDescriptorSetLayout>(layout), nullptr);
    }
    pool = nullptr;
    layout = nullptr;
    set = nullptr;
}
#endif

template <typename SlotVec>
u32 countLiveSlots(const SlotVec& slots) {
    u32 live = 0;
    for (const auto& slot : slots) {
        if (slot.occupied) {
            ++live;
        }
    }
    return live;
}

} // namespace

BindlessArraySizes computeBindlessArraySizes(const VulkanDescriptorLimits& limits) {
    auto fit = [](u32 limit, u32 budget) -> u32 {
        if (limit == 0u) {
            return std::min(kBindlessGpuArrayCapacity, budget);
        }
        const u32 reserve = kBindlessReservedPerStageDescriptors;
        const u32 usable = limit > 2u * reserve ? limit - reserve : std::max(1u, limit / 2u);
        return std::min(usable, budget);
    };

    BindlessArraySizes sizes{};
    sizes.sampledImages = fit(limits.sampledImages, kBindlessSampledImageBudget);
    sizes.storageImages = std::min(fit(limits.storageImages, kBindlessStorageImageBudget), sizes.sampledImages);
    sizes.storageBuffers = fit(limits.storageBuffers, kBindlessStorageBufferBudget);
    sizes.uniformBuffers = std::min(fit(limits.uniformBuffers, kBindlessUniformBufferBudget), sizes.storageBuffers);
    sizes.samplers = limits.samplers == 0u ? kMaxSamplers : std::min(fit(limits.samplers, kMaxSamplers), kMaxSamplers);

    // Every binding is visible to all stages, so the four resource arrays together count against
    // maxPerStageUpdateAfterBindResources (samplers do not); samplers join the all-pools total.
    auto scaleTo = [&sizes](u64 available, bool includeSamplers) {
        u64 total = static_cast<u64>(sizes.sampledImages) + sizes.storageImages + sizes.storageBuffers +
                    sizes.uniformBuffers + (includeSamplers ? sizes.samplers : 0u);
        if (available == 0u || total <= available) {
            return;
        }
        auto scale = [&](u32 value) {
            return static_cast<u32>(std::max<u64>(1u, static_cast<u64>(value) * available / total));
        };
        sizes.sampledImages = scale(sizes.sampledImages);
        sizes.storageImages = std::min(scale(sizes.storageImages), sizes.sampledImages);
        sizes.storageBuffers = scale(sizes.storageBuffers);
        sizes.uniformBuffers = std::min(scale(sizes.uniformBuffers), sizes.storageBuffers);
        if (includeSamplers) {
            sizes.samplers = scale(sizes.samplers);
        }
    };
    if (limits.perStageResources > 0u) {
        const u64 reserve = 4u * kBindlessReservedPerStageDescriptors;
        scaleTo(limits.perStageResources > 2u * reserve ? limits.perStageResources - reserve
                                                        : std::max<u64>(4u, limits.perStageResources / 2u),
                false);
    }
    scaleTo(limits.allPools, true);
    return sizes;
}

BindlessBindingIndex bindlessTextureBinding(u32 slotIndex, bool storage) {
    return {storage ? kBindlessBindingStorageImages : kBindlessBindingSampledImages, slotIndex};
}

BindlessBindingIndex bindlessBufferBinding(u32 slotIndex, bool uniform) {
    return {uniform ? kBindlessBindingUniformBuffers : kBindlessBindingStorageBuffers, slotIndex};
}

BindlessBindingIndex bindlessSamplerBinding(u32 slotIndex) {
    return {kBindlessBindingSamplers, slotIndex};
}

u32 bindlessHeapMaxCapacity(BindlessHeapKind kind) {
    switch (kind) {
    case BindlessHeapKind::Texture:
        return kMaxTextures;
    case BindlessHeapKind::Buffer:
        return kMaxBuffers;
    case BindlessHeapKind::Sampler:
        return kMaxSamplers;
    }
    return 0;
}

u32 clampHeapCapacity(BindlessHeapKind kind, u32 requested) {
    const u32 maxCap = bindlessHeapMaxCapacity(kind);
    if (requested > maxCap) {
        return maxCap;
    }
    return requested;
}

bool bindlessHeapIsEmpty(BindlessHeapKind kind, u32 heapCapacity) {
    (void)kind;
    return heapCapacity == 0u;
}

bool bindlessSlotIndexOutOfRange(BindlessHeapKind kind, u32 index, u32 heapCapacity) {
    (void)kind;
    if (bindlessHeapIsEmpty(kind, heapCapacity)) {
        return true;
    }
    return index >= heapCapacity;
}

bool bindlessHeapAtCapacity(u32 slotCount, u32 freeCount, u32 maxCapacity) {
    return slotCount >= maxCapacity && freeCount == 0u;
}

bool bindlessSlotGenerationMatches(BindlessSlotHandle handle, u32 liveGeneration, bool occupied) {
    return handle.isValid() && occupied && handle.generation == liveGeneration;
}

bool bindlessNativeHandleReady(void* nativeHandle) {
    return nativeHandle != nullptr && reinterpret_cast<uintptr_t>(nativeHandle) > 0x10000u;
}

BindlessSlotPreflight preflightBindlessSlotHandle(BindlessSlotHandle handle, u32 heapCapacity, u32 slotGeneration,
                                                  bool occupied, bool initialized) {
    BindlessSlotPreflight result{};
    result.initialized = initialized;
    result.handle_valid = handle.isValid();
    result.heap_empty = bindlessHeapIsEmpty(handle.kind, heapCapacity);
    result.index_in_range =
        !result.heap_empty && !bindlessSlotIndexOutOfRange(handle.kind, handle.index, heapCapacity);
    if (result.index_in_range) {
        result.generation_match = handle.generation == slotGeneration;
        result.slot_occupied = occupied;
    }
    return result;
}

bool shouldSkipBindlessSlotLookup(BindlessSlotHandle handle, u32 heapCapacity, bool initialized) {
    if (!initialized || !handle.isValid()) {
        return true;
    }
    return bindlessHeapIsEmpty(handle.kind, heapCapacity);
}

u32 packBindlessBindingIndex(u32 binding, u32 arrayIndex) {
    return (binding << 24u) | (arrayIndex & 0xFFFFFFu);
}

bool unpackBindlessBindingIndex(u32 packed, u32& binding, u32& arrayIndex) {
    binding = packed >> 24u;
    arrayIndex = packed & 0xFFFFFFu;
    return binding <= kBindlessBindingUniformBuffers;
}

u64 packBindlessSlotHandle(BindlessSlotHandle handle) {
    return (static_cast<u64>(static_cast<u8>(handle.kind)) << 61u) |
           (static_cast<u64>(handle.index) << 32u) | static_cast<u64>(handle.generation);
}

BindlessSlotHandle unpackBindlessSlotHandle(u64 packed) {
    BindlessSlotHandle handle{};
    handle.kind = static_cast<BindlessHeapKind>((packed >> 61u) & 0x7u);
    handle.index = static_cast<u32>((packed >> 32u) & 0x1FFFFFFFu);
    handle.generation = static_cast<u32>(packed & 0xFFFFFFFFu);
    return handle;
}

namespace {

[[maybe_unused]] void clampSizesToDesc(BindlessArraySizes& sizes, const BindlessDesc& desc) {
    const u32 textures = std::max(1u, std::min(desc.maxTextures, kMaxTextures));
    const u32 buffers = std::max(1u, std::min(desc.maxBuffers, kMaxBuffers));
    const u32 samplers = std::max(1u, std::min(desc.maxSamplers, kMaxSamplers));
    sizes.sampledImages = std::min(sizes.sampledImages, textures);
    sizes.storageImages = std::min(sizes.storageImages, sizes.sampledImages);
    sizes.storageBuffers = std::min(sizes.storageBuffers, buffers);
    sizes.uniformBuffers = std::min(sizes.uniformBuffers, sizes.storageBuffers);
    sizes.samplers = std::min(sizes.samplers, samplers);
}

void setAddressTableEntry(const BindlessGpuHeapState& gpu, u32 index, u64 address) {
    if (gpu.addressTableMapped != nullptr && index < gpu.addressTableEntries) {
        gpu.addressTableMapped[index] = address;
    }
}

bool sameSamplerDesc(const SamplerDesc& a, const SamplerDesc& b) {
    return a.minFilter == b.minFilter && a.magFilter == b.magFilter && a.addressMode == b.addressMode &&
           a.anisotropy == b.anisotropy && a.maxAnisotropy == b.maxAnisotropy && a.minLod == b.minLod &&
           a.maxLod == b.maxLod && a.mipLodBias == b.mipLodBias && a.compareEnable == b.compareEnable &&
           a.compareOp == b.compareOp;
}

BindlessHeapKind kindForResourceType(BindlessResourceType type, bool& flag, bool& ok) {
    ok = true;
    flag = false;
    switch (type) {
    case BindlessResourceType::SampledImage:
        return BindlessHeapKind::Texture;
    case BindlessResourceType::StorageImage:
        flag = true;
        return BindlessHeapKind::Texture;
    case BindlessResourceType::Sampler:
        return BindlessHeapKind::Sampler;
    case BindlessResourceType::StorageBuffer:
        return BindlessHeapKind::Buffer;
    case BindlessResourceType::UniformBuffer:
        flag = true;
        return BindlessHeapKind::Buffer;
    case BindlessResourceType::Invalid:
        break;
    }
    ok = false;
    return BindlessHeapKind::Texture;
}

} // namespace

const char* bindlessBackendName(BindlessBackend backend) {
    switch (backend) {
    case BindlessBackend::None:
        return "none";
    case BindlessBackend::DescriptorSet:
        return "descriptor-set";
    case BindlessBackend::DescriptorBuffer:
        return "descriptor-buffer";
    }
    return "unknown";
}

void BindlessDescriptors::init(const VulkanDevice& device) {
    // Existing consumers bind descriptorSetHandle() with vkCmdBindDescriptorSets: keep that backend.
    BindlessDesc desc{};
    desc.backend = BindlessBackendPreference::DescriptorSet;
    (void)init(device, desc);
}

bool BindlessDescriptors::init(const VulkanDevice& device, const BindlessDesc& desc) {
    if (m_initialized) {
        return m_backend != BindlessBackend::None;
    }

    m_device = &device;
    m_textureSlots.clear();
    m_bufferSlots.clear();
    m_samplerSlots.clear();
    m_freeTextureIndices.clear();
    m_freeBufferIndices.clear();
    m_freeSamplerIndices.clear();
    m_retired.clear();
    m_samplerCache.clear();
    m_frameSerial = 0;
    m_descriptorUpdateCount = 0;
    m_descriptorClearCount = 0;
    m_gpu = BindlessGpuHeapState{};
    m_backend = BindlessBackend::None;
    m_pool = nullptr;
    m_layout = nullptr;
    m_set = nullptr;
    m_arraySizes = BindlessArraySizes{};

#if defined(FUSE_VULKAN_BACKEND)
    if (device.isValid()) {
        (void)createGpuHeap(device, desc);
    }
#else
    // Stub builds may pass a null-backed device reference: never touch it.
    (void)device;
    (void)desc;
#endif
    m_initialized = true;
    return m_backend != BindlessBackend::None;
}

bool BindlessDescriptors::createGpuHeap([[maybe_unused]] const VulkanDevice& device,
                                        [[maybe_unused]] const BindlessDesc& desc) {
#if defined(FUSE_VULKAN_BACKEND)
    using namespace bindless_detail;
    auto vkDevice = static_cast<VkDevice>(device.nativeHandle());
    auto physicalDevice = static_cast<VkPhysicalDevice>(device.nativePhysicalDevice());

    BindlessBackend backend = BindlessBackend::DescriptorSet;
    if (desc.backend != BindlessBackendPreference::DescriptorSet && descriptorBufferAvailable(device)) {
        backend = BindlessBackend::DescriptorBuffer;
    }
    for (;;) {
        const bool descriptorBuffer = backend == BindlessBackend::DescriptorBuffer;
        BindlessArraySizes sizes =
            computeBindlessArraySizes(descriptorBuffer ? descriptorBufferLimits(device) : device.info().descriptorLimits);
        clampSizesToDesc(sizes, desc);
        void* pool = nullptr;
        void* layout = nullptr;
        void* set = nullptr;
        bool created = createVulkanBindlessDescriptors(device, sizes, backend, pool, layout, set);
        if (created && descriptorBuffer &&
            !createDescriptorBuffer(device, static_cast<VkDescriptorSetLayout>(layout), m_gpu)) {
            destroyVulkanBindlessDescriptors(device, pool, layout, set);
            created = false;
        }
        if (created) {
            m_pool = pool;
            m_layout = layout;
            m_set = set;
            m_arraySizes = sizes;
            m_backend = backend;
            break;
        }
        if (!descriptorBuffer) {
            return false;
        }
        backend = BindlessBackend::DescriptorSet; // descriptor-indexing fallback
    }

    // Buffer-address table (binding 5): u64 per buffer slot.
    if (desc.bufferAddressTable && device.info().bufferDeviceAddress && physicalDevice != VK_NULL_HANDLE) {
        HostVisibleBuffer table{};
        const u32 entries = m_arraySizes.storageBuffers;
        if (createHostVisibleBuffer(vkDevice, physicalDevice, static_cast<VkDeviceSize>(entries) * sizeof(u64),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "fuse.bindless.address_table", table)) {
            m_gpu.addressTable = table.buffer;
            m_gpu.addressTableMemory = table.memory;
            m_gpu.addressTableMapped = static_cast<u64*>(table.mapped);
            m_gpu.addressTableAddress = table.address;
            m_gpu.addressTableEntries = entries;
            const VkDeviceSize range = static_cast<VkDeviceSize>(entries) * sizeof(u64);
            if (m_backend == BindlessBackend::DescriptorBuffer) {
#if defined(VK_EXT_descriptor_buffer)
                VkDescriptorAddressInfoEXT addressInfo{};
                addressInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT;
                addressInfo.address = table.address;
                addressInfo.range = range;
                VkDescriptorGetInfoEXT info{};
                info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT;
                info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                info.data.pStorageBuffer = &addressInfo;
                writeDescriptorBufferEntry(vkDevice, m_gpu, kBindlessBindingBufferAddressTable, 0, info);
#endif
            } else {
                VkDescriptorBufferInfo bufferInfo{table.buffer, 0, range};
                VkWriteDescriptorSet write{};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = static_cast<VkDescriptorSet>(m_set);
                write.dstBinding = kBindlessBindingBufferAddressTable;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &bufferInfo;
                vkUpdateDescriptorSets(vkDevice, 1, &write, 0, nullptr);
            }
        }
    }
    return true;
#else
    return false;
#endif
}

void BindlessDescriptors::destroyGpuHeap([[maybe_unused]] const VulkanDevice& device) {
#if defined(FUSE_VULKAN_BACKEND)
    using namespace bindless_detail;
    if (device.isValid()) {
        auto vkDevice = static_cast<VkDevice>(device.nativeHandle());
        for (Slot& slot : m_samplerSlots) {
            if (slot.ownedSampler != nullptr) {
                vkDestroySampler(vkDevice, static_cast<VkSampler>(slot.ownedSampler), nullptr);
                slot.ownedSampler = nullptr;
            }
        }
        destroyDescriptorBuffer(vkDevice, m_gpu);
        HostVisibleBuffer table{};
        table.buffer = static_cast<VkBuffer>(m_gpu.addressTable);
        table.memory = static_cast<VkDeviceMemory>(m_gpu.addressTableMemory);
        table.mapped = m_gpu.addressTableMapped;
        destroyHostVisibleBuffer(vkDevice, table);
    }
    destroyVulkanBindlessDescriptors(device, m_pool, m_layout, m_set);
#endif
    m_gpu = BindlessGpuHeapState{};
    m_pool = nullptr;
    m_layout = nullptr;
    m_set = nullptr;
    m_backend = BindlessBackend::None;
}

void BindlessDescriptors::destroy(const VulkanDevice& device) {
    destroyGpuHeap(device);
    m_textureSlots.clear();
    m_bufferSlots.clear();
    m_samplerSlots.clear();
    m_freeTextureIndices.clear();
    m_freeBufferIndices.clear();
    m_freeSamplerIndices.clear();
    m_retired.clear();
    m_samplerCache.clear();
    m_initialized = false;
    m_device = nullptr;
    m_frameSerial = 0;
    m_descriptorUpdateCount = 0;
    m_descriptorClearCount = 0;
}

u32 BindlessDescriptors::pipelineCreateFlags() const {
#if defined(FUSE_VULKAN_BACKEND) && defined(VK_EXT_descriptor_buffer)
    if (m_backend == BindlessBackend::DescriptorBuffer) {
        return static_cast<u32>(VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT);
    }
#endif
    return 0u;
}

void BindlessDescriptors::bind([[maybe_unused]] void* commandBuffer, [[maybe_unused]] u32 pipelineBindPoint,
                               [[maybe_unused]] void* pipelineLayout, [[maybe_unused]] u32 setIndex) const {
#if defined(FUSE_VULKAN_BACKEND)
    auto cmd = static_cast<VkCommandBuffer>(commandBuffer);
    auto layout = static_cast<VkPipelineLayout>(pipelineLayout);
    const auto bindPoint = static_cast<VkPipelineBindPoint>(pipelineBindPoint);
    if (cmd == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) {
        return;
    }
    if (m_backend == BindlessBackend::DescriptorBuffer) {
        bindless_detail::bindDescriptorBuffer(m_gpu, cmd, bindPoint, layout, setIndex);
    } else if (m_backend == BindlessBackend::DescriptorSet && m_set != nullptr) {
        VkDescriptorSet set = static_cast<VkDescriptorSet>(m_set);
        vkCmdBindDescriptorSets(cmd, bindPoint, layout, setIndex, 1, &set, 0, nullptr);
    }
#endif
}

u64 BindlessDescriptors::bufferAddressAt(u32 index) const {
    if (m_gpu.addressTableMapped == nullptr || index >= m_gpu.addressTableEntries) {
        return 0;
    }
    return m_gpu.addressTableMapped[index];
}

u32 BindlessDescriptors::shaderHandle(BindlessSlotHandle handle) const {
    if (!validateSlot(handle)) {
        return kBindlessInvalidShaderHandle;
    }
    const bool flag = slotsFor(handle.kind)[handle.index].storage;
    BindlessResourceType type = BindlessResourceType::Invalid;
    switch (handle.kind) {
    case BindlessHeapKind::Texture:
        type = flag ? BindlessResourceType::StorageImage : BindlessResourceType::SampledImage;
        break;
    case BindlessHeapKind::Buffer:
        type = flag ? BindlessResourceType::UniformBuffer : BindlessResourceType::StorageBuffer;
        break;
    case BindlessHeapKind::Sampler:
        type = BindlessResourceType::Sampler;
        break;
    }
    return packBindlessShaderHandle(type, handle.index, handle.generation);
}

BindlessSlotHandle BindlessDescriptors::slotHandleFromShaderHandle(u32 packed) const {
    bool flag = false;
    bool ok = false;
    const BindlessHeapKind kind = kindForResourceType(bindlessShaderHandleType(packed), flag, ok);
    if (!ok) {
        return BindlessSlotHandle::invalid();
    }
    const BindlessSlotHandle live = slotHandleAt(kind, bindlessShaderHandleIndex(packed));
    if (!live.isValid() || slotsFor(kind)[live.index].storage != flag ||
        (live.generation & kBindlessHandleGenerationMask) != bindlessShaderHandleGeneration(packed)) {
        return BindlessSlotHandle::invalid();
    }
    return live;
}

bool BindlessDescriptors::validateShaderHandle(u32 packed) const {
    return slotHandleFromShaderHandle(packed).isValid();
}

std::vector<u32>& BindlessDescriptors::freeListFor(BindlessHeapKind kind) {
    switch (kind) {
    case BindlessHeapKind::Texture:
        return m_freeTextureIndices;
    case BindlessHeapKind::Buffer:
        return m_freeBufferIndices;
    case BindlessHeapKind::Sampler:
        return m_freeSamplerIndices;
    }
    return m_freeTextureIndices;
}

void BindlessDescriptors::releaseSlotResources(BindlessHeapKind kind, u32 index) {
    Slot& slot = slotsFor(kind)[index];
    if (slot.descriptorWritten) {
        // Bindings are PARTIALLY_BOUND: a released slot keeps its stale descriptor (null writes
        // need VK_EXT_robustness2::nullDescriptor); shaders must stop indexing it.
        ++m_descriptorClearCount;
    }
    if (kind == BindlessHeapKind::Buffer) {
        setAddressTableEntry(m_gpu, index, 0);
    }
    if (slot.ownedSampler != nullptr) {
#if defined(FUSE_VULKAN_BACKEND)
        if (m_device != nullptr && m_device->isValid()) {
            vkDestroySampler(static_cast<VkDevice>(m_device->nativeHandle()), static_cast<VkSampler>(slot.ownedSampler),
                             nullptr);
        }
#endif
        slot.ownedSampler = nullptr;
    }
    slot.storage = false;
    slot.descriptorWritten = false;
}

bool BindlessDescriptors::retireSlot(BindlessSlotHandle handle) {
    return retireSlot(handle, m_frameSerial);
}

bool BindlessDescriptors::retireSlot(BindlessSlotHandle handle, u64 retireSerial) {
    if (!canFreeSlot(handle)) {
        return false;
    }
    Slot& slot = slotsFor(handle.kind)[handle.index];
    slot.occupied = false;
    slot.retired = true;
    ++slot.generation;
    if (slot.generation == 0) {
        slot.generation = 1;
    }
    m_retired.push_back(RetiredSlot{handle.kind, handle.index, retireSerial});
    return true;
}

u32 BindlessDescriptors::collectRetired(u64 completedSerial) {
    u32 reclaimed = 0;
    usize keep = 0;
    for (usize i = 0; i < m_retired.size(); ++i) {
        const RetiredSlot entry = m_retired[i];
        if (entry.serial > completedSerial) {
            m_retired[keep++] = entry;
            continue;
        }
        releaseSlotResources(entry.kind, entry.index);
        slotsFor(entry.kind)[entry.index].retired = false;
        freeListFor(entry.kind).push_back(entry.index);
        ++reclaimed;
    }
    m_retired.resize(keep);
    return reclaimed;
}

u32 BindlessDescriptors::collectRetiredFromTimeline([[maybe_unused]] void* timelineSemaphore) {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_device == nullptr || !m_device->isValid() || timelineSemaphore == nullptr) {
        return 0;
    }
    uint64_t value = 0;
    if (vkGetSemaphoreCounterValue(static_cast<VkDevice>(m_device->nativeHandle()),
                                   static_cast<VkSemaphore>(timelineSemaphore), &value) != VK_SUCCESS) {
        return 0;
    }
    return collectRetired(static_cast<u64>(value));
#else
    return 0;
#endif
}

u32 BindlessDescriptors::retiredCount(BindlessHeapKind kind) const {
    u32 count = 0;
    for (const RetiredSlot& entry : m_retired) {
        count += entry.kind == kind ? 1u : 0u;
    }
    return count;
}

bool BindlessDescriptors::isSlotRetired(BindlessHeapKind kind, u32 index) const {
    const std::vector<Slot>& slots = slotsFor(kind);
    return index < slots.size() && slots[index].retired;
}

BindlessSlotHandle BindlessDescriptors::acquireSampler(const SamplerDesc& desc) {
    if (!m_initialized) {
        return BindlessSlotHandle::invalid();
    }
    for (SamplerCacheEntry& entry : m_samplerCache) {
        if (sameSamplerDesc(entry.desc, desc) && validateSlot(entry.slot)) {
            ++entry.refs;
            return entry.slot;
        }
    }

    void* samplerHandle = nullptr;
#if defined(FUSE_VULKAN_BACKEND)
    if (m_device != nullptr && m_device->isValid()) {
        const VulkanDeviceInfo& info = m_device->info();
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = static_cast<VkFilter>(desc.magFilter);
        samplerInfo.minFilter = static_cast<VkFilter>(desc.minFilter);
        samplerInfo.mipmapMode =
            desc.minFilter == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        const auto address = static_cast<VkSamplerAddressMode>(desc.addressMode);
        samplerInfo.addressModeU = address;
        samplerInfo.addressModeV = address;
        samplerInfo.addressModeW = address;
        samplerInfo.mipLodBias = desc.mipLodBias;
        samplerInfo.anisotropyEnable = desc.anisotropy && info.samplerAnisotropy ? VK_TRUE : VK_FALSE;
        samplerInfo.maxAnisotropy =
            samplerInfo.anisotropyEnable == VK_TRUE ? std::clamp(desc.maxAnisotropy, 1.f, info.maxSamplerAnisotropy) : 1.f;
        samplerInfo.compareEnable = desc.compareEnable ? VK_TRUE : VK_FALSE;
        samplerInfo.compareOp = static_cast<VkCompareOp>(desc.compareOp);
        samplerInfo.minLod = desc.minLod;
        samplerInfo.maxLod = desc.maxLod;
        samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        auto vkDevice = static_cast<VkDevice>(m_device->nativeHandle());
        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(vkDevice, &samplerInfo, nullptr, &sampler) == VK_SUCCESS) {
            samplerHandle = sampler;
            nameVkObject(vkDevice, vk_object_type::kSampler, samplerHandle,
                         desc.name != nullptr ? desc.name : "fuse.bindless.sampler");
        }
    }
#endif

    const BindlessSlotHandle slot = allocateSamplerSlot();
    if (!slot.isValid()) {
#if defined(FUSE_VULKAN_BACKEND)
        if (samplerHandle != nullptr) {
            vkDestroySampler(static_cast<VkDevice>(m_device->nativeHandle()), static_cast<VkSampler>(samplerHandle),
                             nullptr);
        }
#endif
        return BindlessSlotHandle::invalid();
    }
    m_samplerSlots[slot.index].ownedSampler = samplerHandle;
    if (bindlessNativeHandleReady(samplerHandle)) {
        updateVulkanDescriptor(slot, nullptr, nullptr, samplerHandle, false);
    }
    SamplerCacheEntry entry{};
    entry.desc = desc;
    entry.desc.name = nullptr;
    entry.slot = slot;
    entry.refs = 1;
    m_samplerCache.push_back(entry);
    return slot;
}

void BindlessDescriptors::releaseSampler(BindlessSlotHandle handle) {
    for (usize i = 0; i < m_samplerCache.size(); ++i) {
        SamplerCacheEntry& entry = m_samplerCache[i];
        if (entry.slot != handle) {
            continue;
        }
        if (entry.refs > 1u) {
            --entry.refs;
            return;
        }
        (void)retireSlot(handle, m_frameSerial);
        m_samplerCache.erase(m_samplerCache.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

void BindlessDescriptors::updateVulkanDescriptor(BindlessSlotHandle handle, const Texture* texture,
                                               const Buffer* buffer, void* samplerHandle, bool clear) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!vulkanDescriptorsReady() || m_device == nullptr || !m_device->isValid() || !handle.isValid()) {
        return;
    }
    if (clear) {
        // Bindings are PARTIALLY_BOUND: a released slot may keep its stale descriptor as long as
        // shaders stop indexing it (null writes need VK_EXT_robustness2::nullDescriptor).
        ++m_descriptorClearCount;
        return;
    }
    if (rejectStaleSlotHandle(handle)) {
        return;
    }

    const BindlessBindingIndex binding = bindingIndexForHandle(handle);
    auto vkDevice = static_cast<VkDevice>(m_device->nativeHandle());

    VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    VkDescriptorImageInfo imageInfo{};
    VkDescriptorBufferInfo bufferInfo{};
    u64 bufferAddress = 0;
    VkSampler sampler = VK_NULL_HANDLE;

    switch (handle.kind) {
    case BindlessHeapKind::Texture: {
        const bool storage = slotIsStorageTexture(handle.index);
        descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        const ImageUsage requiredUsage = storage ? ImageUsage::Storage : ImageUsage::Sampled;
        if (texture == nullptr || !bindlessNativeHandleReady(texture->view) ||
            (static_cast<u32>(texture->desc.usage) & static_cast<u32>(requiredUsage)) == 0u) {
            // CPU slot stays valid; the view lacks the usage the descriptor type needs
            // (VUID-VkWriteDescriptorSet-descriptorType-00336/00339).
            return;
        }
        imageInfo.imageView = static_cast<VkImageView>(texture->view);
        imageInfo.imageLayout = storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        break;
    }
    case BindlessHeapKind::Buffer: {
        const bool uniform = slotIsUniformBuffer(handle.index);
        descriptorType = uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        const BufferUsage requiredUsage = uniform ? BufferUsage::Uniform : BufferUsage::Storage;
        if (buffer == nullptr || !bindlessNativeHandleReady(buffer->handle) ||
            (static_cast<u32>(buffer->desc.usage) & static_cast<u32>(requiredUsage)) == 0u) {
            // CPU slot stays valid; the GPU descriptor would violate VUID-VkWriteDescriptorSet-00330/00331.
            return;
        }
        bufferInfo.buffer = static_cast<VkBuffer>(buffer->handle);
        bufferInfo.offset = 0;
        bufferInfo.range = buffer->desc.size > 0 ? buffer->desc.size : VK_WHOLE_SIZE;
        bufferAddress = buffer->deviceAddress;
        break;
    }
    case BindlessHeapKind::Sampler: {
        if (!bindlessNativeHandleReady(samplerHandle)) {
            return;
        }
        sampler = static_cast<VkSampler>(samplerHandle);
        imageInfo.sampler = sampler;
        break;
    }
    }

    if (m_backend == BindlessBackend::DescriptorBuffer) {
#if defined(VK_EXT_descriptor_buffer)
        VkDescriptorAddressInfoEXT addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT;
        VkDescriptorGetInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT;
        info.type = descriptorType;
        switch (descriptorType) {
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            info.data.pStorageImage = &imageInfo;
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            info.data.pSampledImage = &imageInfo;
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            info.data.pSampler = &sampler;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            if (bufferAddress == 0u) {
                return; // descriptor-buffer buffer descriptors are address based
            }
            addressInfo.address = bufferAddress;
            addressInfo.range = buffer->desc.size;
            if (descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                info.data.pUniformBuffer = &addressInfo;
            } else {
                info.data.pStorageBuffer = &addressInfo;
            }
            break;
        default:
            return;
        }
        if (!bindless_detail::writeDescriptorBufferEntry(vkDevice, m_gpu, binding.binding, binding.arrayIndex, info)) {
            return;
        }
#else
        return;
#endif
    } else {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = static_cast<VkDescriptorSet>(m_set);
        write.dstBinding = binding.binding;
        write.dstArrayElement = binding.arrayIndex;
        write.descriptorCount = 1;
        write.descriptorType = descriptorType;
        if (handle.kind == BindlessHeapKind::Buffer) {
            write.pBufferInfo = &bufferInfo;
        } else {
            write.pImageInfo = &imageInfo;
        }
        vkUpdateDescriptorSets(vkDevice, 1, &write, 0, nullptr);
    }
    ++m_descriptorUpdateCount;
    slotsFor(handle.kind)[handle.index].descriptorWritten = true;
#else
    (void)handle;
    (void)texture;
    (void)buffer;
    (void)samplerHandle;
    (void)clear;
#endif
}

const std::vector<BindlessDescriptors::Slot>& BindlessDescriptors::slotsFor(BindlessHeapKind kind) const {
    switch (kind) {
    case BindlessHeapKind::Texture:
        return m_textureSlots;
    case BindlessHeapKind::Buffer:
        return m_bufferSlots;
    case BindlessHeapKind::Sampler:
        return m_samplerSlots;
    }
    return m_textureSlots;
}

std::vector<BindlessDescriptors::Slot>& BindlessDescriptors::slotsFor(BindlessHeapKind kind) {
    switch (kind) {
    case BindlessHeapKind::Texture:
        return m_textureSlots;
    case BindlessHeapKind::Buffer:
        return m_bufferSlots;
    case BindlessHeapKind::Sampler:
        return m_samplerSlots;
    }
    return m_textureSlots;
}

u32 BindlessDescriptors::maxCountFor(BindlessHeapKind kind) const {
    // The CPU heap follows the Vulkan array lengths once the set exists.
    switch (kind) {
    case BindlessHeapKind::Texture:
        return gpuTextureCapacity();
    case BindlessHeapKind::Buffer:
        return gpuBufferCapacity();
    case BindlessHeapKind::Sampler:
        return gpuSamplerCapacity();
    }
    return 0;
}

BindlessSlotHandle BindlessDescriptors::allocateSlot(std::vector<Slot>& slots, std::vector<u32>& freeList,
                                                     u32 maxCount, BindlessHeapKind kind, bool storageFlag,
                                                     u32 flaggedLimit) {
    if (!m_initialized) {
        return BindlessSlotHandle::invalid();
    }

    // Storage textures / uniform buffers must also fit their (possibly shorter) descriptor array.
    const u32 limit = storageFlag ? std::min(maxCount, flaggedLimit) : maxCount;
    for (usize i = freeList.size(); i-- > 0;) {
        const u32 index = freeList[i];
        if (index >= limit) {
            continue;
        }
        freeList.erase(freeList.begin() + static_cast<std::ptrdiff_t>(i));
        Slot& slot = slots[index];
        slot.occupied = true;
        slot.storage = storageFlag;
        return BindlessSlotHandle{kind, index, slot.generation};
    }

    if (slots.size() >= limit) {
        return BindlessSlotHandle::invalid();
    }

    const u32 index = static_cast<u32>(slots.size());
    slots.push_back(Slot{1, true, storageFlag});
    return BindlessSlotHandle{kind, index, 1};
}

void BindlessDescriptors::freeSlot(std::vector<Slot>& slots, std::vector<u32>& freeList,
                                 BindlessSlotHandle handle) {
    if (!canFreeSlot(handle)) {
        return;
    }
    if (bindlessSlotIndexOutOfRange(handle.kind, handle.index, static_cast<u32>(slots.size()))) {
        return;
    }

    releaseSlotResources(handle.kind, handle.index);
    Slot& slot = slots[handle.index];
    slot.occupied = false;
    ++slot.generation;
    if (slot.generation == 0) {
        slot.generation = 1;
    }
    freeList.push_back(handle.index);
}

BindlessSlotHandle BindlessDescriptors::allocateTextureSlot(bool storage) {
    return allocateSlot(m_textureSlots, m_freeTextureIndices, gpuTextureCapacity(), BindlessHeapKind::Texture, storage,
                        gpuStorageTextureCapacity());
}

BindlessSlotHandle BindlessDescriptors::allocateBufferSlot(bool uniform) {
    return allocateSlot(m_bufferSlots, m_freeBufferIndices, gpuBufferCapacity(), BindlessHeapKind::Buffer, uniform,
                        gpuUniformBufferCapacity());
}

BindlessSlotHandle BindlessDescriptors::allocateSamplerSlot() {
    return allocateSlot(m_samplerSlots, m_freeSamplerIndices, gpuSamplerCapacity(), BindlessHeapKind::Sampler, false,
                        gpuSamplerCapacity());
}

void BindlessDescriptors::freeTextureSlot(BindlessSlotHandle handle) {
    if (handle.kind != BindlessHeapKind::Texture) {
        return;
    }
    freeSlot(m_textureSlots, m_freeTextureIndices, handle);
}

void BindlessDescriptors::freeBufferSlot(BindlessSlotHandle handle) {
    if (handle.kind != BindlessHeapKind::Buffer) {
        return;
    }
    freeSlot(m_bufferSlots, m_freeBufferIndices, handle);
}

void BindlessDescriptors::freeSamplerSlot(BindlessSlotHandle handle) {
    if (handle.kind != BindlessHeapKind::Sampler) {
        return;
    }
    freeSlot(m_samplerSlots, m_freeSamplerIndices, handle);
}

void BindlessDescriptors::freeSlot(BindlessSlotHandle handle) {
    switch (handle.kind) {
    case BindlessHeapKind::Texture:
        freeTextureSlot(handle);
        break;
    case BindlessHeapKind::Buffer:
        freeBufferSlot(handle);
        break;
    case BindlessHeapKind::Sampler:
        freeSamplerSlot(handle);
        break;
    }
}

bool BindlessDescriptors::validateSlot(BindlessSlotHandle handle) const {
    return preflightSlot(handle).can_validate();
}

BindlessSlotPreflight BindlessDescriptors::preflightSlot(BindlessSlotHandle handle) const {
    const u32 capacity = heapCapacity(handle.kind);
    u32 generation = 0;
    bool occupied = false;
    if (!bindlessHeapIsEmpty(handle.kind, capacity) &&
        !bindlessSlotIndexOutOfRange(handle.kind, handle.index, capacity)) {
        const Slot& slot = slotsFor(handle.kind)[handle.index];
        generation = slot.generation;
        occupied = slot.occupied;
    }
    return preflightBindlessSlotHandle(handle, capacity, generation, occupied, m_initialized);
}

bool BindlessDescriptors::slotGenerationMismatch(BindlessSlotHandle handle) const {
    return preflightSlot(handle).is_generation_mismatch();
}

bool BindlessDescriptors::rejectStaleSlotHandle(BindlessSlotHandle handle) const {
    return !validateSlot(handle);
}

bool BindlessDescriptors::shouldSkipSlotLookup(BindlessSlotHandle handle) const {
    return shouldSkipBindlessSlotLookup(handle, heapCapacity(handle.kind), m_initialized);
}

bool BindlessDescriptors::canFreeSlot(BindlessSlotHandle handle) const {
    return validateSlot(handle);
}

bool BindlessDescriptors::slotIndexOutOfRange(BindlessHeapKind kind, u32 index) const {
    return bindlessSlotIndexOutOfRange(kind, index, heapCapacity(kind));
}

bool BindlessDescriptors::isSlotOccupied(BindlessHeapKind kind, u32 index) const {
    const std::vector<Slot>& slots = slotsFor(kind);
    if (bindlessHeapIsEmpty(kind, static_cast<u32>(slots.size()))) {
        return false;
    }
    if (bindlessSlotIndexOutOfRange(kind, index, static_cast<u32>(slots.size()))) {
        return false;
    }
    return slots[index].occupied;
}

u32 BindlessDescriptors::slotGeneration(BindlessHeapKind kind, u32 index) const {
    const std::vector<Slot>& slots = slotsFor(kind);
    if (bindlessHeapIsEmpty(kind, static_cast<u32>(slots.size()))) {
        return 0;
    }
    if (bindlessSlotIndexOutOfRange(kind, index, static_cast<u32>(slots.size()))) {
        return 0;
    }
    return slots[index].generation;
}

bool BindlessDescriptors::slotIsStorageTexture(u32 index) const {
    if (bindlessHeapIsEmpty(BindlessHeapKind::Texture, static_cast<u32>(m_textureSlots.size()))) {
        return false;
    }
    if (bindlessSlotIndexOutOfRange(BindlessHeapKind::Texture, index,
                                    static_cast<u32>(m_textureSlots.size()))) {
        return false;
    }
    return m_textureSlots[index].storage;
}

bool BindlessDescriptors::slotIsUniformBuffer(u32 index) const {
    if (bindlessHeapIsEmpty(BindlessHeapKind::Buffer, static_cast<u32>(m_bufferSlots.size()))) {
        return false;
    }
    if (bindlessSlotIndexOutOfRange(BindlessHeapKind::Buffer, index,
                                    static_cast<u32>(m_bufferSlots.size()))) {
        return false;
    }
    return m_bufferSlots[index].storage;
}

BindlessSlotHandle BindlessDescriptors::slotHandleAt(BindlessHeapKind kind, u32 index) const {
    if (!m_initialized) {
        return BindlessSlotHandle::invalid();
    }

    const std::vector<Slot>& slots = slotsFor(kind);
    if (bindlessHeapIsEmpty(kind, static_cast<u32>(slots.size()))) {
        return BindlessSlotHandle::invalid();
    }
    if (bindlessSlotIndexOutOfRange(kind, index, static_cast<u32>(slots.size()))) {
        return BindlessSlotHandle::invalid();
    }

    const Slot& slot = slots[index];
    if (!slot.occupied) {
        return BindlessSlotHandle::invalid();
    }

    return BindlessSlotHandle{kind, index, slot.generation};
}

BindlessBindingIndex BindlessDescriptors::bindingIndexForHandle(BindlessSlotHandle handle) const {
    if (shouldSkipSlotLookup(handle) || rejectStaleSlotHandle(handle)) {
        return {};
    }

    switch (handle.kind) {
    case BindlessHeapKind::Texture:
        return bindlessTextureBinding(handle.index, m_textureSlots[handle.index].storage);
    case BindlessHeapKind::Buffer:
        return bindlessBufferBinding(handle.index, m_bufferSlots[handle.index].storage);
    case BindlessHeapKind::Sampler:
        return bindlessSamplerBinding(handle.index);
    }
    return {};
}

BindlessBindingIndex BindlessDescriptors::bindingIndexForSlot(BindlessHeapKind kind, u32 index) const {
    if (heapIsEmpty(kind)) {
        return {};
    }
    if (slotIndexOutOfRange(kind, index)) {
        return {};
    }
    return bindingIndexForHandle(slotHandleAt(kind, index));
}

u32 BindlessDescriptors::heapLiveCount(BindlessHeapKind kind) const {
    const std::vector<Slot>& slots = slotsFor(kind);
    if (bindlessHeapIsEmpty(kind, static_cast<u32>(slots.size()))) {
        return 0;
    }
    return countLiveSlots(slots);
}

u32 BindlessDescriptors::heapFreeCount(BindlessHeapKind kind) const {
    switch (kind) {
    case BindlessHeapKind::Texture:
        return static_cast<u32>(m_freeTextureIndices.size());
    case BindlessHeapKind::Buffer:
        return static_cast<u32>(m_freeBufferIndices.size());
    case BindlessHeapKind::Sampler:
        return static_cast<u32>(m_freeSamplerIndices.size());
    }
    return 0;
}

bool BindlessDescriptors::heapIsEmpty(BindlessHeapKind kind) const {
    return bindlessHeapIsEmpty(kind, heapCapacity(kind));
}

bool BindlessDescriptors::heapAtCapacity(BindlessHeapKind kind) const {
    return bindlessHeapAtCapacity(heapCapacity(kind), heapFreeCount(kind), maxCountFor(kind));
}

bool BindlessDescriptors::resizeHeap(BindlessHeapKind kind, u32 newCapacity) {
    if (!m_initialized) {
        return false;
    }

    newCapacity = std::min(clampHeapCapacity(kind, newCapacity), maxCountFor(kind));
    if (newCapacity == 0u) {
        return true;
    }

    std::vector<Slot>& slots = slotsFor(kind);
    std::vector<u32>* freeList = nullptr;
    switch (kind) {
    case BindlessHeapKind::Texture:
        freeList = &m_freeTextureIndices;
        break;
    case BindlessHeapKind::Buffer:
        freeList = &m_freeBufferIndices;
        break;
    case BindlessHeapKind::Sampler:
        freeList = &m_freeSamplerIndices;
        break;
    }

    if (newCapacity <= slots.size()) {
        return true;
    }

    const u32 oldSize = static_cast<u32>(slots.size());
    slots.resize(newCapacity);
    if (freeList != nullptr) {
        for (u32 index = newCapacity; index > oldSize; --index) {
            freeList->push_back(index - 1u);
        }
    }
    return true;
}

u32 BindlessDescriptors::heapCapacity(BindlessHeapKind kind) const {
    return static_cast<u32>(slotsFor(kind).size());
}

u32 BindlessDescriptors::registerTexture(const Texture& texture, bool storage) {
    const BindlessSlotHandle handle = allocateTextureSlot(storage);
    if (!handle.isValid()) {
        return UINT32_MAX;
    }
    if (bindlessNativeHandleReady(texture.view)) {
        updateVulkanDescriptor(handle, &texture, nullptr, nullptr, false);
    }
    return handle.index;
}

u32 BindlessDescriptors::registerBuffer(const Buffer& buffer, bool uniform) {
    const BindlessSlotHandle handle = allocateBufferSlot(uniform);
    if (!handle.isValid()) {
        return UINT32_MAX;
    }
    if (bindlessNativeHandleReady(buffer.handle)) {
        updateVulkanDescriptor(handle, nullptr, &buffer, nullptr, false);
        setAddressTableEntry(m_gpu, handle.index, buffer.deviceAddress);
    }
    return handle.index;
}

u32 BindlessDescriptors::registerSampler(void* samplerHandle) {
    const BindlessSlotHandle handle = allocateSamplerSlot();
    if (!handle.isValid()) {
        return UINT32_MAX;
    }
    if (bindlessNativeHandleReady(samplerHandle)) {
        updateVulkanDescriptor(handle, nullptr, nullptr, samplerHandle, false);
    }
    return handle.index;
}

void BindlessDescriptors::unregisterTexture(u32 index) {
    freeTextureSlot(BindlessSlotHandle{BindlessHeapKind::Texture, index,
                                       slotGeneration(BindlessHeapKind::Texture, index)});
}

void BindlessDescriptors::unregisterBuffer(u32 index) {
    freeBufferSlot(BindlessSlotHandle{BindlessHeapKind::Buffer, index,
                                      slotGeneration(BindlessHeapKind::Buffer, index)});
}

void BindlessDescriptors::unregisterSampler(u32 index) {
    freeSamplerSlot(BindlessSlotHandle{BindlessHeapKind::Sampler, index,
                                        slotGeneration(BindlessHeapKind::Sampler, index)});
}

u32 BindlessDescriptors::registeredTextureCount() const {
    return countLiveSlots(m_textureSlots);
}

u32 BindlessDescriptors::registeredBufferCount() const {
    return countLiveSlots(m_bufferSlots);
}

u32 BindlessDescriptors::registeredSamplerCount() const {
    return countLiveSlots(m_samplerSlots);
}

BindlessSlotHandle BindlessDescriptors::registerTextureSlot(const Texture& texture, bool storage) {
    const BindlessSlotHandle handle = allocateTextureSlot(storage);
    if (handle.isValid() && bindlessNativeHandleReady(texture.view)) {
        updateVulkanDescriptor(handle, &texture, nullptr, nullptr, false);
    }
    return handle;
}

BindlessSlotHandle BindlessDescriptors::registerBufferSlot(const Buffer& buffer, bool uniform) {
    const BindlessSlotHandle handle = allocateBufferSlot(uniform);
    if (handle.isValid() && bindlessNativeHandleReady(buffer.handle)) {
        updateVulkanDescriptor(handle, nullptr, &buffer, nullptr, false);
        setAddressTableEntry(m_gpu, handle.index, buffer.deviceAddress);
    }
    return handle;
}

BindlessSlotHandle BindlessDescriptors::registerSamplerSlot(void* samplerHandle) {
    const BindlessSlotHandle handle = allocateSamplerSlot();
    if (handle.isValid() && bindlessNativeHandleReady(samplerHandle)) {
        updateVulkanDescriptor(handle, nullptr, nullptr, samplerHandle, false);
    }
    return handle;
}

void BindlessDescriptors::unregisterSlot(BindlessSlotHandle handle) {
    freeSlot(handle);
}

} // namespace fuse::renderer
