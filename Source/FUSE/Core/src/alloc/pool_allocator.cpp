#include <fuse/alloc/pool_allocator.hpp>

#include <fuse/alloc/alloc_stats.hpp>
#include <fuse/alloc/leak_detector.hpp>
#include <fuse/assert.hpp>

namespace fuse::alloc {

PoolAllocator::PoolAllocator(u32 blockSize, u32 blockCount, const char* name)
    : m_name(name != nullptr ? name : "pool")
    , m_blockSize(blockSize > 0 ? blockSize : 1u)
    , m_blockCount(blockCount) {
    m_storage.resize(static_cast<usize>(m_blockSize) * m_blockCount);
    m_stats.totalBytes = m_storage.size();
    m_generations.assign(m_blockCount, 0u);
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

    if (!detail::isSupportedAlignment(info.alignment) ||
        (info.alignment > 1u && (m_blockSize % info.alignment) != 0u)) {
        detail::recordFailedAlloc(m_stats);
        notifyStats(m_name, m_stats);
        return nullptr;
    }

    const u32 blockIndex = m_freeList.back();
    m_freeList.pop_back();
    ++m_generations[blockIndex];

    void* block = m_storage.data() + static_cast<usize>(blockIndex) * m_blockSize;
    detail::recordBlockAlloc(m_stats, m_blockSize, m_stats.totalBytes);
    notifyStats(m_name, m_stats);
    if constexpr (kLeakDetectorEnabled) {
        LeakDetector::recordAlloc(block, info.size, info.tag != nullptr ? info.tag : m_name);
    }
    return block;
}

bool PoolAllocator::blockIndexOf(const void* ptr, u32& outIndex) const {
    if (ptr == nullptr || m_storage.empty()) {
        return false;
    }
    const auto* base = m_storage.data();
    const auto* block = static_cast<const u8*>(ptr);
    if (block < base || block >= base + m_storage.size()) {
        return false;
    }
    const usize byteOffset = static_cast<usize>(block - base);
    if ((byteOffset % m_blockSize) != 0u) {
        return false;
    }
    outIndex = static_cast<u32>(byteOffset / m_blockSize);
    return true;
}

bool PoolAllocator::isLive(const void* ptr) const {
    u32 index = 0;
    return blockIndexOf(ptr, index) && (m_generations[index] & 1u) != 0u;
}

u32 PoolAllocator::generationOf(const void* ptr) const {
    u32 index = 0;
    return blockIndexOf(ptr, index) ? m_generations[index] : 0u;
}

void PoolAllocator::free(void* ptr, usize size) {
    if (ptr == nullptr) {
        return;
    }

    u32 blockIndex = 0;
    if (!blockIndexOf(ptr, blockIndex)) {
        detail::recordFailedAlloc(m_stats);
        notifyStats(m_name, m_stats);
        return;
    }

    if ((m_generations[blockIndex] & 1u) == 0u) {
        // Block is already free: pushing it again would hand the same memory out twice.
        ++m_doubleFrees;
        detail::recordFailedAlloc(m_stats);
        notifyStats(m_name, m_stats);
        FUSE_ASSERT(false, "PoolAllocator: double free (block generation is not live)");
        return;
    }

    ++m_generations[blockIndex];
    m_freeList.push_back(blockIndex);
    if constexpr (kLeakDetectorEnabled) {
        LeakDetector::recordFree(ptr);
    }
    detail::recordFree(m_stats, size > 0 ? size : m_blockSize);
    notifyStats(m_name, m_stats);
}

void PoolAllocator::reset() {
    // Retire every live block: bump live (odd) generations to the next even value so stale
    // pointers from before the reset read as free.
    for (u32& generation : m_generations) {
        generation += generation & 1u;
    }
    m_doubleFrees = 0;
    if constexpr (kLeakDetectorEnabled) {
        LeakDetector::releaseRange(m_storage.data(), m_storage.size());
    }
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
