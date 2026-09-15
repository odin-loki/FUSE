#include <fuse/renderer/vk/bindless.hpp>

namespace fuse::renderer {

namespace {

u32 allocateSlot(std::vector<u32>& freeList, std::vector<u32>& slots, u32 maxCount) {
    if (!freeList.empty()) {
        const u32 index = freeList.back();
        freeList.pop_back();
        slots[index] = 1;
        return index;
    }
    if (slots.size() >= maxCount) {
        return UINT32_MAX;
    }
    const u32 index = static_cast<u32>(slots.size());
    slots.push_back(1);
    return index;
}

void releaseSlot(std::vector<u32>& freeList, std::vector<u32>& slots, u32 index) {
    if (index >= slots.size()) {
        return;
    }
    slots[index] = 0;
    freeList.push_back(index);
}

} // namespace

void BindlessDescriptors::init(const VulkanDevice& device) {
    if (m_initialized) {
        return;
    }

    m_textureSlots.clear();
    m_bufferSlots.clear();
    m_samplerSlots.clear();
    m_freeTextureIndices.clear();
    m_freeBufferIndices.clear();
    m_freeSamplerIndices.clear();

    (void)device;
    // VkDescriptorPool/Set/Layout deferred to B2.4 — indices only for B2.3.
    m_pool = nullptr;
    m_layout = nullptr;
    m_set = nullptr;
    m_initialized = true;
}

void BindlessDescriptors::destroy(const VulkanDevice& device) {
    (void)device;
    m_textureSlots.clear();
    m_bufferSlots.clear();
    m_samplerSlots.clear();
    m_freeTextureIndices.clear();
    m_freeBufferIndices.clear();
    m_freeSamplerIndices.clear();
    m_pool = nullptr;
    m_layout = nullptr;
    m_set = nullptr;
    m_initialized = false;
}

u32 BindlessDescriptors::registerTexture(const Texture& texture, bool storage) {
    (void)texture;
    (void)storage;
    if (!m_initialized) {
        return UINT32_MAX;
    }
    return allocateSlot(m_freeTextureIndices, m_textureSlots, kMaxTextures);
}

u32 BindlessDescriptors::registerBuffer(const Buffer& buffer) {
    (void)buffer;
    if (!m_initialized) {
        return UINT32_MAX;
    }
    return allocateSlot(m_freeBufferIndices, m_bufferSlots, kMaxBuffers);
}

u32 BindlessDescriptors::registerSampler(void* samplerHandle) {
    (void)samplerHandle;
    if (!m_initialized) {
        return UINT32_MAX;
    }
    return allocateSlot(m_freeSamplerIndices, m_samplerSlots, kMaxSamplers);
}

void BindlessDescriptors::unregisterTexture(u32 index) {
    releaseSlot(m_freeTextureIndices, m_textureSlots, index);
}

void BindlessDescriptors::unregisterBuffer(u32 index) {
    releaseSlot(m_freeBufferIndices, m_bufferSlots, index);
}

void BindlessDescriptors::unregisterSampler(u32 index) {
    releaseSlot(m_freeSamplerIndices, m_samplerSlots, index);
}

} // namespace fuse::renderer
