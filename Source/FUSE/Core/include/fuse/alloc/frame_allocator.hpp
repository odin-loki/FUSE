#pragma once

#include <fuse/types.hpp>

#include <cstddef>
#include <vector>

namespace fuse::alloc {

/// Per-frame bump allocator for job scratch (WP-04 stub).
/// Not thread-safe — allocate from worker-local instances only.
class FrameAllocator {
public:
    explicit FrameAllocator(u32 capacityBytes = 64u * 1024u);

    void reset();
    void* allocate(u32 size, u32 alignment = alignof(std::max_align_t));

    u32 usedBytes() const { return static_cast<u32>(m_offset); }
    u32 capacityBytes() const { return static_cast<u32>(m_storage.size()); }

    template <typename T>
    T* allocate(u32 count = 1) {
        return static_cast<T*>(allocate(static_cast<u32>(sizeof(T) * count), alignof(T)));
    }

private:
    std::vector<u8> m_storage;
    u32 m_offset = 0;
};

} // namespace fuse::alloc
