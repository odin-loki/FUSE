#include <fuse/alloc/frame_allocator.hpp>

#include <cstdlib>
#include <cstring>

namespace fuse::alloc {

FrameAllocator::FrameAllocator(u32 capacityBytes) : m_storage(capacityBytes), m_offset(0) {}

void FrameAllocator::reset() {
    m_offset = 0;
}

void* FrameAllocator::allocate(u32 size, u32 alignment) {
    if (size == 0) {
        return nullptr;
    }

    const u32 aligned = (m_offset + alignment - 1u) & ~(alignment - 1u);
    if (aligned + size > m_storage.size()) {
        return nullptr;
    }

    m_offset = aligned + size;
    return m_storage.data() + aligned;
}

} // namespace fuse::alloc
