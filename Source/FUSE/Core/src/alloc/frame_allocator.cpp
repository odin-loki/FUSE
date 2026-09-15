#include <fuse/alloc/frame_allocator.hpp>

#include <fuse/alloc/alloc_stats.hpp>

namespace fuse::alloc {

FrameAllocator::FrameAllocator(u32 capacityBytes, const char* name)
    : m_name(name != nullptr ? name : "frame") {
    m_buffers[0].resize(capacityBytes);
    m_buffers[1].resize(capacityBytes);
    m_stats.totalBytes = capacityBytes;
}

u8* FrameAllocator::activeBuffer() {
    return m_buffers[m_activeIndex].data();
}

const u8* FrameAllocator::activeBuffer() const {
    return m_buffers[m_activeIndex].data();
}

void FrameAllocator::reset() {
    m_offset = 0;
    m_stats.usedBytes = 0;
    notifyStats(m_name, m_stats);
}

void FrameAllocator::advanceFrame() {
    m_previousUsedBytes = m_offset;
    m_activeIndex = 1u - m_activeIndex;
    m_offset = 0;
    m_stats.usedBytes = 0;
    notifyStats(m_name, m_stats);
}

const u8* FrameAllocator::previousFrameData() const {
    const u32 previousIndex = 1u - m_activeIndex;
    return m_buffers[previousIndex].data();
}

void* FrameAllocator::allocate(u32 size, u32 alignment) {
    return alloc({size, alignment, nullptr});
}

void* FrameAllocator::alloc(AllocInfo info) {
    if (info.size == 0) {
        return nullptr;
    }

    const usize aligned = detail::alignUp(m_offset, info.alignment);
    if (aligned + info.size > m_buffers[m_activeIndex].size()) {
        detail::recordFailedAlloc(m_stats);
        notifyStats(m_name, m_stats);
        return nullptr;
    }

    m_offset = static_cast<u32>(aligned + info.size);
    detail::recordBumpAlloc(m_stats, m_offset, m_buffers[m_activeIndex].size());
    notifyStats(m_name, m_stats);
    return activeBuffer() + aligned;
}

void FrameAllocator::free(void* /*ptr*/, usize /*size*/) {
    // Bump allocator — individual frees are no-ops until reset/advanceFrame.
}

} // namespace fuse::alloc
