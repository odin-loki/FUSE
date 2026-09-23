#include <fuse/renderer/vk/bindless.hpp>

#include <algorithm>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>

#include <array>
#endif

namespace fuse::renderer {

namespace {

#if defined(FUSE_VULKAN_BACKEND)
VkDescriptorSetLayoutBinding makeBinding(u32 binding, VkDescriptorType type, u32 count) {
    VkDescriptorSetLayoutBinding layoutBinding{};
    layoutBinding.binding = binding;
    layoutBinding.descriptorType = type;
    layoutBinding.descriptorCount = count;
    layoutBinding.stageFlags = VK_SHADER_STAGE_ALL;
    return layoutBinding;
}

bool createVulkanBindlessDescriptors(const VulkanDevice& device, const BindlessArraySizes& sizes, void*& outPool,
                                     void*& outLayout, void*& outSet) {
    if (!device.isValid()) {
        return false;
    }

    auto vkDevice = static_cast<VkDevice>(device.nativeHandle());

    std::array<VkDescriptorSetLayoutBinding, 5> bindings = {
        makeBinding(kBindlessBindingStorageImages, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizes.storageImages),
        makeBinding(kBindlessBindingSampledImages, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sizes.sampledImages),
        makeBinding(kBindlessBindingSamplers, VK_DESCRIPTOR_TYPE_SAMPLER, sizes.samplers),
        makeBinding(kBindlessBindingStorageBuffers, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sizes.storageBuffers),
        makeBinding(kBindlessBindingUniformBuffers, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sizes.uniformBuffers),
    };

    // UPDATE_AFTER_BIND is only legal per descriptor type the device enabled (VUID-03005/03007...).
    const VulkanDeviceInfo& features = device.info();
    auto flagsFor = [](bool updateAfterBind) -> VkDescriptorBindingFlags {
        return VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
               (updateAfterBind ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT : 0u);
    };
    std::array<VkDescriptorBindingFlags, 5> bindingFlags = {
        flagsFor(features.storageImageUpdateAfterBind),
        flagsFor(features.sampledImageUpdateAfterBind),
        flagsFor(features.sampledImageUpdateAfterBind),
        flagsFor(features.storageBufferUpdateAfterBind),
        flagsFor(features.uniformBufferUpdateAfterBind),
    };

    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
    bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    bindingFlagsInfo.bindingCount = static_cast<u32>(bindingFlags.size());
    bindingFlagsInfo.pBindingFlags = bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = static_cast<u32>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    layoutInfo.pNext = &bindingFlagsInfo;

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(vkDevice, &layoutInfo, nullptr, &layout) != VK_SUCCESS) {
        return false;
    }

    std::array<VkDescriptorPoolSize, 5> poolSizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizes.storageImages},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sizes.sampledImages},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, sizes.samplers},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sizes.storageBuffers},
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

void BindlessDescriptors::init(const VulkanDevice& device) {
    if (m_initialized) {
        return;
    }

    m_device = &device;
    m_textureSlots.clear();
    m_bufferSlots.clear();
    m_samplerSlots.clear();
    m_freeTextureIndices.clear();
    m_freeBufferIndices.clear();
    m_freeSamplerIndices.clear();
    m_descriptorUpdateCount = 0;
    m_descriptorClearCount = 0;

    m_arraySizes = BindlessArraySizes{};
#if defined(FUSE_VULKAN_BACKEND)
    if (device.isValid()) {
        m_arraySizes = computeBindlessArraySizes(device.info().descriptorLimits);
    }
    if (!createVulkanBindlessDescriptors(device, m_arraySizes, m_pool, m_layout, m_set)) {
        m_pool = nullptr;
        m_layout = nullptr;
        m_set = nullptr;
    }
#else
    // Stub builds may pass a null-backed device reference: never touch it.
    (void)device;
    m_pool = nullptr;
    m_layout = nullptr;
    m_set = nullptr;
#endif
    m_initialized = true;
}

void BindlessDescriptors::destroy(const VulkanDevice& device) {
#if defined(FUSE_VULKAN_BACKEND)
    destroyVulkanBindlessDescriptors(device, m_pool, m_layout, m_set);
#else
    m_pool = nullptr;
    m_layout = nullptr;
    m_set = nullptr;
#endif
    m_textureSlots.clear();
    m_bufferSlots.clear();
    m_samplerSlots.clear();
    m_freeTextureIndices.clear();
    m_freeBufferIndices.clear();
    m_freeSamplerIndices.clear();
    m_initialized = false;
    m_device = nullptr;
    m_descriptorUpdateCount = 0;
    m_descriptorClearCount = 0;
}

void BindlessDescriptors::updateVulkanDescriptor(BindlessSlotHandle handle, const Texture* texture,
                                               const Buffer* buffer, void* samplerHandle, bool clear) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!vulkanDescriptorsReady() || m_device == nullptr || !m_device->isValid() || !handle.isValid()) {
        return;
    }
    if (!clear && rejectStaleSlotHandle(handle)) {
        return;
    }

    const BindlessBindingIndex binding = bindingIndexForHandle(handle);
    if (!clear && binding.binding == 0u && binding.arrayIndex == 0u &&
        rejectStaleSlotHandle(handle)) {
        return;
    }

    if (clear) {
        // Null writes need VK_EXT_robustness2::nullDescriptor. Bindings are PARTIALLY_BOUND, so a
        // released slot may keep its stale descriptor as long as shaders stop indexing it.
        ++m_descriptorClearCount;
        return;
    }

    auto vkDevice = static_cast<VkDevice>(m_device->nativeHandle());
    VkDescriptorSet set = static_cast<VkDescriptorSet>(m_set);
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding.binding;
    write.dstArrayElement = binding.arrayIndex;
    write.descriptorCount = 1;

    VkDescriptorImageInfo imageInfo{};
    VkDescriptorBufferInfo bufferInfo{};
    VkSampler sampler = VK_NULL_HANDLE;

    switch (handle.kind) {
    case BindlessHeapKind::Texture: {
        const bool storage = slotIsStorageTexture(handle.index);
        write.descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        const ImageUsage requiredUsage = storage ? ImageUsage::Storage : ImageUsage::Sampled;
        if (texture != nullptr &&
            (static_cast<u32>(texture->desc.usage) & static_cast<u32>(requiredUsage)) == 0u) {
            // CPU slot stays valid; the view lacks the usage the descriptor type needs
            // (VUID-VkWriteDescriptorSet-descriptorType-00336/00339).
            return;
        }
        if (!clear && texture != nullptr && bindlessNativeHandleReady(texture->view)) {
            imageInfo.imageView = static_cast<VkImageView>(texture->view);
            imageInfo.imageLayout = slotIsStorageTexture(handle.index) ? VK_IMAGE_LAYOUT_GENERAL
                                                                       : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        } else if (!clear) {
            return;
        }
        write.pImageInfo = &imageInfo;
        break;
    }
    case BindlessHeapKind::Buffer: {
        const bool uniform = slotIsUniformBuffer(handle.index);
        write.descriptorType = uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        const BufferUsage requiredUsage = uniform ? BufferUsage::Uniform : BufferUsage::Storage;
        if (buffer != nullptr &&
            (static_cast<u32>(buffer->desc.usage) & static_cast<u32>(requiredUsage)) == 0u) {
            // CPU slot stays valid; the GPU descriptor would violate VUID-VkWriteDescriptorSet-00330/00331.
            return;
        }
        if (!clear && buffer != nullptr && bindlessNativeHandleReady(buffer->handle)) {
            bufferInfo.buffer = static_cast<VkBuffer>(buffer->handle);
            bufferInfo.offset = 0;
            bufferInfo.range = buffer->desc.size > 0 ? buffer->desc.size : VK_WHOLE_SIZE;
        } else if (!clear) {
            return;
        }
        write.pBufferInfo = &bufferInfo;
        break;
    }
    case BindlessHeapKind::Sampler: {
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        if (!clear && bindlessNativeHandleReady(samplerHandle)) {
            sampler = static_cast<VkSampler>(samplerHandle);
        } else if (!clear) {
            return;
        } else if (!bindlessNativeHandleReady(samplerHandle)) {
            // Null sampler writes are invalid without nullDescriptor; drop the GPU write.
            ++m_descriptorClearCount;
            return;
        }
        imageInfo.sampler = sampler;
        write.pImageInfo = &imageInfo;
        break;
    }
    }

    vkUpdateDescriptorSets(vkDevice, 1, &write, 0, nullptr);
    if (clear) {
        ++m_descriptorClearCount;
    } else {
        ++m_descriptorUpdateCount;
        Slot& slot = slotsFor(handle.kind)[handle.index];
        slot.descriptorWritten = true;
    }
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

    Slot& slot = slots[handle.index];
    if (slot.descriptorWritten) {
        updateVulkanDescriptor(handle, nullptr, nullptr, nullptr, true);
    }
    slot.occupied = false;
    slot.storage = false;
    slot.descriptorWritten = false;
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
