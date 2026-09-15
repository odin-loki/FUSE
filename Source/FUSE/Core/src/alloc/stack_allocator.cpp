#include <fuse/alloc/stack_allocator.hpp>

#include <fuse/alloc/alloc_stats.hpp>

namespace fuse::alloc {

StackAllocator::StackAllocator(u32 capacityBytes, const char* name)
    : m_name(name != nullptr ? name : "stack")
    , m_storage(capacityBytes) {
    m_stats.totalBytes = capacityBytes;
}

StackMarker StackAllocator::pushMark() {
    return m_offset;
}

void StackAllocator::popToMark(StackMarker mark) {
    if (mark > m_offset) {
        detail::recordFailedAlloc(m_stats);
        notifyStats(m_name, m_stats);
        return;
    }

    const usize released = m_offset - mark;
    m_offset = mark;
    detail::recordFree(m_stats, released);
    notifyStats(m_name, m_stats);
}

void StackAllocator::reset() {
    m_offset = 0;
    m_stats.usedBytes = 0;
    notifyStats(m_name, m_stats);
}

void* StackAllocator::alloc(AllocInfo info) {
    if (info.size == 0) {
        return nullptr;
    }

    const usize aligned = detail::alignUp(m_offset, info.alignment);
    if (aligned + info.size > m_storage.size()) {
        detail::recordFailedAlloc(m_stats);
        notifyStats(m_name, m_stats);
        return nullptr;
    }

    m_offset = static_cast<u32>(aligned + info.size);
    detail::recordBumpAlloc(m_stats, m_offset, m_storage.size());
    return m_storage.data() + aligned;
}

void StackAllocator::free(void* ptr, usize size) {
    if (ptr == nullptr || size == 0) {
        return;
    }

    const auto* block = static_cast<const u8*>(ptr);
    const usize blockEnd = static_cast<usize>(block - m_storage.data()) + size;
    if (blockEnd != m_offset) {
        detail::recordFailedAlloc(m_stats);
        notifyStats(m_name, m_stats);
        return;
    }

    m_offset = static_cast<u32>(blockEnd - size);
    detail::recordFree(m_stats, size);
    notifyStats(m_name, m_stats);
}

} // namespace fuse::alloc
