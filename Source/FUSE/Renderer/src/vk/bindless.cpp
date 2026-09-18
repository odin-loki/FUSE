#include <fuse/renderer/vk/bindless.hpp>

namespace fuse::renderer {

namespace {

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

    m_textureSlots.clear();
    m_bufferSlots.clear();
    m_samplerSlots.clear();
    m_freeTextureIndices.clear();
    m_freeBufferIndices.clear();
    m_freeSamplerIndices.clear();

    (void)device;
    // VkDescriptorPool/Set/Layout deferred to B2.4 — CPU heap only for B2.3 deepen.
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

BindlessSlotHandle BindlessDescriptors::allocateSlot(std::vector<Slot>& slots, std::vector<u32>& freeList,
                                                     u32 maxCount, BindlessHeapKind kind, bool storageFlag) {
    if (!m_initialized) {
        return BindlessSlotHandle::invalid();
    }

    if (!freeList.empty()) {
        const u32 index = freeList.back();
        freeList.pop_back();
        Slot& slot = slots[index];
        slot.occupied = true;
        slot.storage = storageFlag;
        return BindlessSlotHandle{kind, index, slot.generation};
    }

    if (slots.size() >= maxCount) {
        return BindlessSlotHandle::invalid();
    }

    const u32 index = static_cast<u32>(slots.size());
    slots.push_back(Slot{1, true, storageFlag});
    return BindlessSlotHandle{kind, index, 1};
}

void BindlessDescriptors::freeSlot(std::vector<Slot>& slots, std::vector<u32>& freeList,
                                 BindlessSlotHandle handle) {
    if (!m_initialized || !handle.isValid()) {
        return;
    }
    if (bindlessHeapIsEmpty(handle.kind, static_cast<u32>(slots.size()))) {
        return;
    }
    if (bindlessSlotIndexOutOfRange(handle.kind, handle.index, static_cast<u32>(slots.size()))) {
        return;
    }

    Slot& slot = slots[handle.index];
    if (!slot.occupied || slot.generation != handle.generation) {
        return;
    }

    slot.occupied = false;
    slot.storage = false;
    ++slot.generation;
    if (slot.generation == 0) {
        slot.generation = 1;
    }
    freeList.push_back(handle.index);
}

BindlessSlotHandle BindlessDescriptors::allocateTextureSlot(bool storage) {
    return allocateSlot(m_textureSlots, m_freeTextureIndices, kMaxTextures, BindlessHeapKind::Texture, storage);
}

BindlessSlotHandle BindlessDescriptors::allocateBufferSlot(bool uniform) {
    return allocateSlot(m_bufferSlots, m_freeBufferIndices, kMaxBuffers, BindlessHeapKind::Buffer, uniform);
}

BindlessSlotHandle BindlessDescriptors::allocateSamplerSlot() {
    return allocateSlot(m_samplerSlots, m_freeSamplerIndices, kMaxSamplers, BindlessHeapKind::Sampler, false);
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
    if (!m_initialized || !handle.isValid()) {
        return false;
    }

    const std::vector<Slot>& slots = slotsFor(handle.kind);
    if (bindlessHeapIsEmpty(handle.kind, static_cast<u32>(slots.size()))) {
        return false;
    }
    if (bindlessSlotIndexOutOfRange(handle.kind, handle.index, static_cast<u32>(slots.size()))) {
        return false;
    }

    const Slot& slot = slots[handle.index];
    return slot.occupied && slot.generation == handle.generation;
}

bool BindlessDescriptors::slotGenerationMismatch(BindlessSlotHandle handle) const {
    if (!m_initialized || !handle.isValid()) {
        return false;
    }

    const std::vector<Slot>& slots = slotsFor(handle.kind);
    if (bindlessHeapIsEmpty(handle.kind, static_cast<u32>(slots.size()))) {
        return false;
    }
    if (bindlessSlotIndexOutOfRange(handle.kind, handle.index, static_cast<u32>(slots.size()))) {
        return false;
    }

    const Slot& slot = slots[handle.index];
    return !slot.occupied || slot.generation != handle.generation;
}

bool BindlessDescriptors::rejectStaleSlotHandle(BindlessSlotHandle handle) const {
    return !validateSlot(handle);
}

bool BindlessDescriptors::canFreeSlot(BindlessSlotHandle handle) const {
    if (!m_initialized || !handle.isValid()) {
        return false;
    }

    const std::vector<Slot>& slots = slotsFor(handle.kind);
    if (bindlessHeapIsEmpty(handle.kind, static_cast<u32>(slots.size()))) {
        return false;
    }
    if (bindlessSlotIndexOutOfRange(handle.kind, handle.index, static_cast<u32>(slots.size()))) {
        return false;
    }

    const Slot& slot = slots[handle.index];
    return slot.occupied && slot.generation == handle.generation;
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
    if (rejectStaleSlotHandle(handle)) {
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

bool BindlessDescriptors::tryBindingIndexForHandle(BindlessSlotHandle handle,
                                                   BindlessBindingIndex& out) const {
    if (rejectStaleSlotHandle(handle)) {
        out = {};
        return false;
    }

    out = bindingIndexForHandle(handle);
    return true;
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

bool BindlessDescriptors::canAllocateSlot(BindlessHeapKind kind) const {
    if (!m_initialized) {
        return false;
    }

    const u32 maxCount = maxCountFor(kind);
    switch (kind) {
    case BindlessHeapKind::Texture:
        return !m_freeTextureIndices.empty() || m_textureSlots.size() < maxCount;
    case BindlessHeapKind::Buffer:
        return !m_freeBufferIndices.empty() || m_bufferSlots.size() < maxCount;
    case BindlessHeapKind::Sampler:
        return !m_freeSamplerIndices.empty() || m_samplerSlots.size() < maxCount;
    }
    return false;
}

bool BindlessDescriptors::heapAtCapacity(BindlessHeapKind kind) const {
    if (!m_initialized) {
        return false;
    }
    return !canAllocateSlot(kind);
}

bool BindlessDescriptors::canResizeHeap(BindlessHeapKind kind, u32 requestedCapacity) const {
    (void)kind;
    (void)requestedCapacity;
    return m_initialized;
}

bool BindlessDescriptors::resizeHeap(BindlessHeapKind kind, u32 newCapacity) {
    if (!m_initialized) {
        return false;
    }

    newCapacity = clampHeapCapacity(kind, newCapacity);
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
    (void)texture;
    const BindlessSlotHandle handle = allocateTextureSlot(storage);
    return handle.isValid() ? handle.index : UINT32_MAX;
}

u32 BindlessDescriptors::registerBuffer(const Buffer& buffer, bool uniform) {
    (void)buffer;
    const BindlessSlotHandle handle = allocateBufferSlot(uniform);
    return handle.isValid() ? handle.index : UINT32_MAX;
}

u32 BindlessDescriptors::registerSampler(void* samplerHandle) {
    (void)samplerHandle;
    const BindlessSlotHandle handle = allocateSamplerSlot();
    return handle.isValid() ? handle.index : UINT32_MAX;
}

void BindlessDescriptors::unregisterTexture(u32 index) {
    freeTextureSlot(BindlessSlotHandle{BindlessHeapKind::Texture, index, slotGeneration(BindlessHeapKind::Texture, index)});
}

void BindlessDescriptors::unregisterBuffer(u32 index) {
    freeBufferSlot(BindlessSlotHandle{BindlessHeapKind::Buffer, index, slotGeneration(BindlessHeapKind::Buffer, index)});
}

void BindlessDescriptors::unregisterSampler(u32 index) {
    freeSamplerSlot(BindlessSlotHandle{BindlessHeapKind::Sampler, index, slotGeneration(BindlessHeapKind::Sampler, index)});
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
    (void)texture;
    return allocateTextureSlot(storage);
}

BindlessSlotHandle BindlessDescriptors::registerBufferSlot(const Buffer& buffer, bool uniform) {
    (void)buffer;
    return allocateBufferSlot(uniform);
}

BindlessSlotHandle BindlessDescriptors::registerSamplerSlot(void* samplerHandle) {
    (void)samplerHandle;
    return allocateSamplerSlot();
}

void BindlessDescriptors::unregisterSlot(BindlessSlotHandle handle) {
    freeSlot(handle);
}

} // namespace fuse::renderer
