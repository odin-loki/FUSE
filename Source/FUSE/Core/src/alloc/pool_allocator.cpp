#include <fuse/alloc/pool_allocator.hpp>

#include <fuse/alloc/alloc_stats.hpp>

namespace fuse::alloc {

PoolAllocator::PoolAllocator(u32 blockSize, u32 blockCount, const char* name)
    : m_name(name != nullptr ? name : "pool")
    , m_blockSize(blockSize > 0 ? blockSize : 1u)
    , m_blockCount(blockCount) {
    m_storage.resize(static_cast<usize>(m_blockSize) * m_blockCount);
    m_stats.totalBytes = m_storage.size();
    m_freeList.reserve(m_blockCount);
    for (u32 i = 0; i < m_blockCount; ++i) {
        m_freeList.push_back(m_blockCount - 1u - i);
    }
}

u32 PoolAllocator::availableBlocks() const {
    return static_cast<u32>(m_freeList.size());
}

void* PoolAllocator::alloc(AllocInfo info) {
    if (info.size == 0 || info.size > m_blockSize || m_freeList.empty()) {
        detail::recordFailedAlloc(m_stats);
        notifyStats(m_name, m_stats);
        return nullptr;
    }

    if (info.alignment > 1u && (m_blockSize % info.alignment) != 0u) {
        detail::recordFailedAlloc(m_stats);
        notifyStats(m_name, m_stats);
        return nullptr;
    }

    const u32 blockIndex = m_freeList.back();
    m_freeList.pop_back();

    void* block = m_storage.data() + static_cast<usize>(blockIndex) * m_blockSize;
    detail::recordBlockAlloc(m_stats, m_blockSize, m_stats.totalBytes);
    notifyStats(m_name, m_stats);
    return block;
}

void PoolAllocator::free(void* ptr, usize size) {
    if (ptr == nullptr) {
        return;
    }

    const auto* base = m_storage.data();
    const auto* block = static_cast<const u8*>(ptr);
    if (block < base || block >= base + m_storage.size()) {
        detail::recordFailedAlloc(m_stats);
        notifyStats(m_name, m_stats);
        return;
    }

    const usize byteOffset = static_cast<usize>(block - base);
    if ((byteOffset % m_blockSize) != 0u) {
        detail::recordFailedAlloc(m_stats);
        notifyStats(m_name, m_stats);
        return;
    }

    const u32 blockIndex = static_cast<u32>(byteOffset / m_blockSize);
    m_freeList.push_back(blockIndex);
    detail::recordFree(m_stats, size > 0 ? size : m_blockSize);
    notifyStats(m_name, m_stats);
}

void PoolAllocator::reset() {
    m_freeList.clear();
    for (u32 i = 0; i < m_blockCount; ++i) {
        m_freeList.push_back(m_blockCount - 1u - i);
    }
    m_stats.usedBytes = 0;
    m_stats.allocCount = 0;
    m_stats.freeCount = 0;
    m_stats.failedAllocs = 0;
    m_stats.peakUsedBytes = 0;
    notifyStats(m_name, m_stats);
}

} // namespace fuse::alloc
