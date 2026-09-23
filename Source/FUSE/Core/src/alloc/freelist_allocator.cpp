#include <fuse/alloc/freelist_allocator.hpp>

#include <fuse/alloc/alloc_stats.hpp>
#include <fuse/alloc/leak_detector.hpp>

namespace fuse::alloc {

FreeListAllocator::FreeListAllocator(u32 capacityBytes, const char* name)
    : m_name(name != nullptr ? name : "freelist") {
    m_capacity = capacityBytes - (capacityBytes % kMinAlign);
    if (m_capacity < kMinBlock) {
        m_capacity = 0;
    }
    m_storage.resize(m_capacity);
    m_stats.totalBytes = m_capacity;
    seedArena();
}

FreeListAllocator::BlockHeader* FreeListAllocator::headerAt(u32 offset) {
    return reinterpret_cast<BlockHeader*>(m_storage.data() + offset);
}

const FreeListAllocator::BlockHeader* FreeListAllocator::headerAt(u32 offset) const {
    return reinterpret_cast<const BlockHeader*>(m_storage.data() + offset);
}

bool FreeListAllocator::isFree(u32 offset) const {
    if (offset >= m_capacity) {
        return false;
    }
    return (headerAt(offset)->flags & kFreeBit) != 0u;
}

void FreeListAllocator::failAlloc() {
    detail::recordFailedAlloc(m_stats);
    notifyStats(m_name, m_stats);
}

void FreeListAllocator::insertFree(u32 offset) {
    BlockHeader* h = headerAt(offset);
    h->flags = kFreeBit;
    h->nextFree = m_freeHead;
    m_freeHead = offset;
}

void FreeListAllocator::unlinkFree(u32 offset) {
    BlockHeader* h = headerAt(offset);
    if (m_freeHead == offset) {
        m_freeHead = h->nextFree;
        h->nextFree = kInvalid;
        return;
    }

    u32 prev = m_freeHead;
    while (prev != kInvalid) {
        BlockHeader* p = headerAt(prev);
        if (p->nextFree == offset) {
            p->nextFree = h->nextFree;
            h->nextFree = kInvalid;
            return;
        }
        prev = p->nextFree;
    }
}

void FreeListAllocator::patchNextPrevSize(u32 offset) {
    const u32 next = offset + headerAt(offset)->size;
    if (next < m_capacity) {
        headerAt(next)->prevSize = headerAt(offset)->size;
    }
}

void FreeListAllocator::seedArena() {
    m_freeHead = kInvalid;
    if (m_capacity < kMinBlock) {
        return;
    }

    BlockHeader* h = headerAt(0);
    h->size = m_capacity;
    h->prevSize = 0;
    h->flags = kFreeBit;
    h->nextFree = kInvalid;
    m_freeHead = 0;
}

void* FreeListAllocator::allocate(u32 size, u32 alignment) {
    return alloc({size, alignment, nullptr});
}

void* FreeListAllocator::alloc(AllocInfo info) {
    if (info.size == 0) {
        return nullptr;
    }

    const usize alignment = info.alignment == 0 ? 1u : info.alignment;
    if (!detail::isSupportedAlignment(alignment) || m_capacity < kMinBlock || info.size > m_capacity) {
        failAlloc();
        return nullptr;
    }

    u32 takenBase = static_cast<u32>(detail::alignUp(static_cast<usize>(kHeaderSize) + info.size, kMinAlign));
    if (takenBase < kMinBlock) {
        takenBase = kMinBlock;
    }

    u32 cur = m_freeHead;
    while (cur != kInvalid) {
        BlockHeader* h = headerAt(cur);
        const u32 nextFree = h->nextFree;
        const u32 blockOff = cur;
        const u32 blockSize = h->size;
        const u32 origPrev = h->prevSize;

        u32 headerPos = 0;
        u32 taken = takenBase;
        bool found = false;

        usize payload = detail::alignUp(static_cast<usize>(blockOff) + kHeaderSize, alignment);
        if (payload >= static_cast<usize>(blockOff) + kHeaderSize && payload + info.size >= payload) {
            for (;;) {
                if (payload < kHeaderSize || payload > m_capacity) {
                    break;
                }
                headerPos = static_cast<u32>(payload - kHeaderSize);
                if (headerPos < blockOff || headerPos + taken > blockOff + blockSize) {
                    break;
                }

                const u32 prefix = headerPos - blockOff;
                if (prefix == 0u || prefix >= kMinBlock) {
                    const u32 suffix = (blockOff + blockSize) - (headerPos + taken);
                    if (suffix > 0u && suffix < kMinBlock) {
                        taken += suffix;
                    }
                    found = true;
                    break;
                }
                if (alignment <= 1u) {
                    break;
                }
                if (payload + alignment <= payload) {
                    break;
                }
                payload += alignment;
            }
        }

        if (found) {
            const u32 origEnd = blockOff + blockSize;

            if (headerPos == blockOff) {
                unlinkFree(blockOff);
            } else {
                headerAt(blockOff)->size = headerPos - blockOff;
                patchNextPrevSize(blockOff);
            }

            BlockHeader* live = headerAt(headerPos);
            live->size = taken;
            live->prevSize = (headerPos == blockOff) ? origPrev : (headerPos - blockOff);
            live->flags = 0;
            live->nextFree = kInvalid;

            const u32 suffixStart = headerPos + taken;
            if (suffixStart < origEnd) {
                BlockHeader* suffixHdr = headerAt(suffixStart);
                suffixHdr->size = origEnd - suffixStart;
                suffixHdr->prevSize = taken;
                suffixHdr->nextFree = kInvalid;
                insertFree(suffixStart);
                patchNextPrevSize(suffixStart);
            } else {
                patchNextPrevSize(headerPos);
            }

            detail::recordBlockAlloc(m_stats, taken, m_capacity);
            notifyStats(m_name, m_stats);
            u8* payload = m_storage.data() + static_cast<usize>(headerPos) + kHeaderSize;
            if constexpr (kLeakDetectorEnabled) {
                LeakDetector::recordAlloc(payload, info.size, info.tag != nullptr ? info.tag : m_name);
            }
            return payload;
        }

        cur = nextFree;
    }

    failAlloc();
    return nullptr;
}

void FreeListAllocator::free(void* ptr, usize /*size*/) {
    if (ptr == nullptr) {
        return;
    }

    auto* p = static_cast<u8*>(ptr);
    u8* base = m_storage.data();
    if (p < base + kHeaderSize || p >= base + m_capacity) {
        failAlloc();
        return;
    }

    const usize payloadOff = static_cast<usize>(p - base);
    const u32 offset = static_cast<u32>(payloadOff - kHeaderSize);
    if ((offset % kMinAlign) != 0u || offset >= m_capacity) {
        failAlloc();
        return;
    }

    BlockHeader* h = headerAt(offset);
    if ((h->flags & kFreeBit) != 0u || h->size < kMinBlock) {
        failAlloc();
        return;
    }

    const u32 released = h->size;
    const u32 next = offset + h->size;
    if (next < m_capacity && isFree(next)) {
        unlinkFree(next);
        h->size += headerAt(next)->size;
    }

    u32 block = offset;
    if (h->prevSize >= kMinBlock && offset >= h->prevSize) {
        const u32 prev = offset - h->prevSize;
        if (isFree(prev)) {
            unlinkFree(prev);
            headerAt(prev)->size += h->size;
            block = prev;
        }
    }

    patchNextPrevSize(block);
    insertFree(block);
    if constexpr (kLeakDetectorEnabled) {
        LeakDetector::recordFree(ptr);
    }
    detail::recordFree(m_stats, released);
    notifyStats(m_name, m_stats);
}

void FreeListAllocator::reset() {
    if constexpr (kLeakDetectorEnabled) {
        LeakDetector::releaseRange(m_storage.data(), m_storage.size());
    }
    seedArena();
    m_stats.usedBytes = 0;
    notifyStats(m_name, m_stats);
}

} // namespace fuse::alloc
