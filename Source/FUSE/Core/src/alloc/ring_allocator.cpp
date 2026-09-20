#include <fuse/alloc/ring_allocator.hpp>

#include <fuse/alloc/alloc_stats.hpp>

namespace fuse::alloc {

RingAllocator::RingAllocator(u32 capacityBytes, const char* name)
    : m_name(name != nullptr ? name : "ring")
    , m_storage(capacityBytes)
    , m_capacity(capacityBytes) {
    m_stats.totalBytes = capacityBytes;
}

RingAllocator::Header* RingAllocator::headerAt(u32 offset) {
    return reinterpret_cast<Header*>(m_storage.data() + offset);
}

const RingAllocator::Header* RingAllocator::headerAt(u32 offset) const {
    return reinterpret_cast<const Header*>(m_storage.data() + offset);
}

void RingAllocator::failAlloc() {
    detail::recordFailedAlloc(m_stats);
    notifyStats(m_name, m_stats);
}

bool RingAllocator::place(u32 start, u32 limit, usize size, usize alignment, Header& out) const {
    if (start >= limit || m_capacity < kHeaderBytes) {
        return false;
    }

    const usize headerEnd = static_cast<usize>(start) + kHeaderBytes;
    const usize payload = detail::alignUp(headerEnd, alignment);
    if (payload < headerEnd) {
        return false;
    }
    if (payload > limit || limit - payload < size) {
        return false;
    }

    const usize end = payload + size;
    if (end < payload) {
        return false;
    }

    usize stride = detail::alignUp(end - start, kHeaderBytes);
    if (start + stride > limit) {
        stride = static_cast<usize>(limit - start);
        if (end > start + stride) {
            return false;
        }
    }

    const usize remain = static_cast<usize>(limit) - (static_cast<usize>(start) + stride);
    if (remain > 0u && remain < kHeaderBytes) {
        stride = static_cast<usize>(limit - start);
    }

    if (stride < kHeaderBytes || start + stride > limit) {
        return false;
    }

    out.payloadSize = static_cast<u32>(size);
    out.stride = static_cast<u32>(stride);
    out.payloadOffset = static_cast<u32>(payload);
    out.headerOffset = start;
    return true;
}

void RingAllocator::skipWrapMarkers() {
    while (m_liveCount > 0u && m_used > 0u) {
        if (m_tail + kHeaderBytes > m_capacity) {
            break;
        }
        Header* marker = headerAt(m_tail);
        if (marker->payloadSize != 0u || marker->stride == 0u) {
            break;
        }
        if (m_tail + marker->stride > m_capacity) {
            break;
        }
        m_used -= marker->stride;
        m_tail += marker->stride;
        if (m_tail == m_capacity) {
            m_tail = 0;
        }
        m_stats.usedBytes = m_used;
    }
}

void* RingAllocator::allocate(u32 size, u32 alignment) {
    return alloc({size, alignment, nullptr});
}

void* RingAllocator::alloc(AllocInfo info) {
    if (info.size == 0) {
        return nullptr;
    }

    const usize alignment = info.alignment == 0 ? 1u : info.alignment;
    if (m_capacity < kHeaderBytes || info.size > m_capacity) {
        failAlloc();
        return nullptr;
    }

    if (m_liveCount == 0u) {
        m_head = 0;
        m_tail = 0;
        m_used = 0;
    } else if (m_head == m_tail) {
        failAlloc();
        return nullptr;
    }

    Header placed{};
    u32 headerPos = 0;
    u32 wrapSkip = 0;

    if (m_liveCount == 0u) {
        if (!place(0, m_capacity, info.size, alignment, placed)) {
            failAlloc();
            return nullptr;
        }
        headerPos = 0;
    } else if (m_head < m_tail) {
        if (!place(m_head, m_tail, info.size, alignment, placed)) {
            failAlloc();
            return nullptr;
        }
        headerPos = m_head;
    } else if (place(m_head, m_capacity, info.size, alignment, placed)) {
        headerPos = m_head;
    } else {
        if (!place(0, m_tail, info.size, alignment, placed)) {
            failAlloc();
            return nullptr;
        }
        wrapSkip = m_capacity - m_head;
        if (wrapSkip > 0u && wrapSkip < kHeaderBytes) {
            failAlloc();
            return nullptr;
        }
        if (static_cast<usize>(m_used) + wrapSkip + placed.stride > m_capacity) {
            failAlloc();
            return nullptr;
        }
        headerPos = 0;
    }

    if (wrapSkip > 0u) {
        Header* marker = headerAt(m_head);
        marker->payloadSize = 0;
        marker->stride = wrapSkip;
        marker->payloadOffset = 0;
        marker->headerOffset = m_head;
        m_used += wrapSkip;
        m_head = 0;
    }

    Header* block = headerAt(headerPos);
    placed.headerOffset = headerPos;
    *block = placed;

    m_head = headerPos + placed.stride;
    if (m_head == m_capacity) {
        m_head = 0;
    }
    m_used += placed.stride;
    m_liveCount += 1;

    detail::recordBumpAlloc(m_stats, m_used, m_capacity);
    notifyStats(m_name, m_stats);
    return m_storage.data() + placed.payloadOffset;
}

void RingAllocator::free(void* ptr, usize /*size*/) {
    if (ptr == nullptr) {
        return;
    }
    if (m_liveCount == 0) {
        failAlloc();
        return;
    }

    skipWrapMarkers();
    if (m_liveCount == 0 || m_tail + kHeaderBytes > m_capacity) {
        failAlloc();
        return;
    }

    Header* oldest = headerAt(m_tail);
    if (oldest->payloadSize == 0u || oldest->stride == 0u) {
        failAlloc();
        return;
    }

    u8* oldestPtr = m_storage.data() + oldest->payloadOffset;
    if (oldestPtr != ptr) {
        failAlloc();
        return;
    }

    const u32 stride = oldest->stride;
    m_used -= stride;
    m_tail += stride;
    if (m_tail == m_capacity) {
        m_tail = 0;
    }
    m_liveCount -= 1;

    detail::recordFree(m_stats, stride);
    skipWrapMarkers();

    if (m_liveCount == 0) {
        m_head = 0;
        m_tail = 0;
        m_used = 0;
        m_stats.usedBytes = 0;
    } else {
        m_stats.usedBytes = m_used;
    }
    notifyStats(m_name, m_stats);
}

void RingAllocator::reset() {
    m_head = 0;
    m_tail = 0;
    m_used = 0;
    m_liveCount = 0;
    m_stats.usedBytes = 0;
    notifyStats(m_name, m_stats);
}

} // namespace fuse::alloc
