#include <fuse/renderer/skinning/bone_buffer.hpp>

#include <cstring>

namespace fuse::renderer {

namespace {

constexpr usize kMatrixBytes = 16u * sizeof(f32);

} // namespace

BoneBuffer::~BoneBuffer() {
    destroy();
}

bool BoneBuffer::init(ResourceManager& resources, const BoneBufferDesc& desc) {
    destroy();
    if (!resources.isReady() || desc.maxBones == 0u) {
        return false;
    }

    m_desc = desc;
    for (u32 slot = 0; slot < kSlots; ++slot) {
        BufferDesc bufferDesc{};
        bufferDesc.size = static_cast<usize>(desc.maxBones) * kMatrixBytes;
        // Storage: skinning shaders read it; TransferSrc: captures / readback verification.
        bufferDesc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                                    static_cast<u32>(BufferUsage::TransferSrc));
        bufferDesc.memoryUsage = MemoryUsage::CpuToGpu;
        bufferDesc.name = desc.name;
        m_buffers[slot] = resources.createBuffer(bufferDesc);
        const Buffer* buffer = resources.getBuffer(m_buffers[slot]);
        if (buffer == nullptr || buffer->mapped == nullptr) {
            for (u32 i = 0; i <= slot; ++i) {
                if (m_buffers[i].isValid()) {
                    resources.destroyBuffer(m_buffers[i]);
                }
                m_buffers[i] = BufferHandle{};
            }
            return false;
        }
        ++m_stats.buffersCreated;
    }
    m_resources = &resources;
    return true;
}

void BoneBuffer::destroy() {
    if (m_resources != nullptr) {
        for (BufferHandle& handle : m_buffers) {
            if (handle.isValid()) {
                m_resources->destroyBuffer(handle);
            }
            handle = BufferHandle{};
        }
    }
    for (u32& count : m_boneCounts) {
        count = 0;
    }
    m_resources = nullptr;
}

bool BoneBuffer::upload(u32 frameSlot, const void* matrices, u32 boneCount) {
    if (m_resources == nullptr || matrices == nullptr || boneCount > m_desc.maxBones) {
        ++m_stats.rejectedUploads;
        return false;
    }
    const u32 slot = frameSlot % kSlots;
    Buffer* buffer = m_resources->getBuffer(m_buffers[slot]);
    if (buffer == nullptr || buffer->mapped == nullptr) {
        ++m_stats.rejectedUploads;
        return false;
    }
    // HOST_COHERENT mapping: the write is visible to the queue submit that follows.
    const usize bytes = static_cast<usize>(boneCount) * kMatrixBytes;
    std::memcpy(buffer->mapped, matrices, bytes);
    m_boneCounts[slot] = boneCount;
    ++m_stats.uploads;
    m_stats.bytesUploaded += bytes;
    return true;
}

} // namespace fuse::renderer
