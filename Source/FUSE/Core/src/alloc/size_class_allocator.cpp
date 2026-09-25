#include <fuse/alloc/size_class_allocator.hpp>

#include <fuse/alloc/alloc_stats.hpp>

#include <cstring>
#include <new>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace fuse::alloc {

namespace {

static_assert(sizeof(void*) <= SizeClassAllocator::kAlignment, "free-list link must fit the smallest block");

constexpr usize kSmallStep = 16u;
constexpr usize kSmallMax = 128u;
constexpr u32 kSmallClasses = 8u;
constexpr usize kUnsizedHeader = SizeClassAllocator::kAlignment;

/// floor(log2(v)) for v > 0.
u32 floorLog2(usize v) {
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanReverse64(&index, static_cast<unsigned long long>(v));
    return static_cast<u32>(index);
#else
    return static_cast<u32>(63 - __builtin_clzll(static_cast<unsigned long long>(v)));
#endif
}

usize alignUp16(usize v) { return (v + (SizeClassAllocator::kAlignment - 1u)) & ~(SizeClassAllocator::kAlignment - 1u); }

void* systemAlloc(usize bytes) {
    return ::operator new(bytes, std::align_val_t{SizeClassAllocator::kAlignment}, std::nothrow);
}

void systemFree(void* p) { ::operator delete(p, std::align_val_t{SizeClassAllocator::kAlignment}); }

/// Locks only when the allocator is shared between threads.
class OptionalLock {
public:
    OptionalLock(std::mutex& mutex, bool enabled) : m_mutex(enabled ? &mutex : nullptr) {
        if (m_mutex != nullptr) {
            m_mutex->lock();
        }
    }
    ~OptionalLock() {
        if (m_mutex != nullptr) {
            m_mutex->unlock();
        }
    }
    OptionalLock(const OptionalLock&) = delete;
    OptionalLock& operator=(const OptionalLock&) = delete;

private:
    std::mutex* m_mutex;
};

} // namespace

u32 SizeClassAllocator::classIndex(usize size) {
    if (size <= kSmallMax) {
        return size == 0u ? 0u : static_cast<u32>((size + kSmallStep - 1u) / kSmallStep - 1u);
    }
    if (size > kMaxClassBytes) {
        return kClassCount;
    }
    // Four classes per power of two above 128 B: (5..8) * 2^(p-2) for p = floor(log2(size - 1)).
    const usize v = size - 1u;
    const u32 p = floorLog2(v);
    const u32 sub = static_cast<u32>(v >> (p - 2u)) - 4u;
    return kSmallClasses + (p - 7u) * 4u + sub;
}

usize SizeClassAllocator::classBytes(u32 index) {
    if (index < kSmallClasses) {
        return (static_cast<usize>(index) + 1u) * kSmallStep;
    }
    const u32 rel = index - kSmallClasses;
    const u32 p = 7u + rel / 4u;
    const usize sub = rel % 4u;
    return (5u + sub) << (p - 2u);
}

SizeClassAllocator::SizeClassAllocator(const SizeClassAllocatorDesc& desc)
    : m_name(desc.name != nullptr ? desc.name : "sizeclass"),
      m_pageBytes(alignUp16(desc.pageBytes < 4096u ? 4096u : desc.pageBytes)),
      m_threadSafe(desc.threadSafe) {
    if (desc.reserveBytes > 0u) {
        (void)refill(desc.reserveBytes);
    }
}

SizeClassAllocator::~SizeClassAllocator() { releaseAll(); }

bool SizeClassAllocator::refill(usize minBytes) {
    const usize payload = alignUp16(minBytes > m_pageBytes ? minBytes : m_pageBytes);
    const usize bytes = sizeof(PageHeader) + payload;
    auto* page = static_cast<PageHeader*>(systemAlloc(bytes));
    if (page == nullptr) {
        return false;
    }
    page->next = m_pages;
    page->bytes = bytes;
    m_pages = page;
    // The unused tail of the previous page is abandoned (at most one block of waste per refill).
    m_cursor = reinterpret_cast<u8*>(page) + sizeof(PageHeader);
    m_pageEnd = reinterpret_cast<u8*>(page) + bytes;
    m_reserved += bytes;
    ++m_systemAllocs;
    m_stats.totalBytes = m_reserved;
    return true;
}

void* SizeClassAllocator::allocateLocked(usize size) {
    if (size == 0u) {
        return nullptr;
    }
    const u32 cls = classIndex(size);
    if (cls == kClassCount) {
        const usize bytes = sizeof(OversizeHeader) + size;
        auto* header = static_cast<OversizeHeader*>(systemAlloc(bytes));
        if (header == nullptr) {
            detail::recordFailedAlloc(m_stats);
            return nullptr;
        }
        header->prev = nullptr;
        header->next = m_oversize;
        header->bytes = bytes;
        if (m_oversize != nullptr) {
            m_oversize->prev = header;
        }
        m_oversize = header;
        m_reserved += bytes;
        ++m_systemAllocs;
        m_stats.totalBytes = m_reserved;
        m_stats.usedBytes += size;
        ++m_stats.allocCount;
        detail::updatePeak(m_stats);
        return header + 1;
    }

    const usize blockBytes = classBytes(cls);
    void* block = nullptr;
    if (FreeBlock* head = m_free[cls]) {
        m_free[cls] = head->next;
        block = head;
    } else {
        if (m_cursor == nullptr || static_cast<usize>(m_pageEnd - m_cursor) < blockBytes) {
            if (!refill(blockBytes)) {
                detail::recordFailedAlloc(m_stats);
                return nullptr;
            }
        }
        block = m_cursor;
        m_cursor += blockBytes;
    }
    m_stats.usedBytes += blockBytes;
    ++m_stats.allocCount;
    detail::updatePeak(m_stats);
    return block;
}

void SizeClassAllocator::deallocateLocked(void* ptr, usize size) {
    if (ptr == nullptr) {
        return;
    }
    const u32 cls = classIndex(size);
    if (cls == kClassCount) {
        auto* header = static_cast<OversizeHeader*>(ptr) - 1;
        if (header->prev != nullptr) {
            header->prev->next = header->next;
        } else {
            m_oversize = header->next;
        }
        if (header->next != nullptr) {
            header->next->prev = header->prev;
        }
        m_reserved -= header->bytes;
        m_stats.totalBytes = m_reserved;
        m_stats.usedBytes -= size;
        ++m_stats.freeCount;
        systemFree(header);
        return;
    }
    auto* block = static_cast<FreeBlock*>(ptr);
    block->next = m_free[cls];
    m_free[cls] = block;
    m_stats.usedBytes -= classBytes(cls);
    ++m_stats.freeCount;
}

void* SizeClassAllocator::allocate(usize size) {
    OptionalLock lock(m_mutex, m_threadSafe);
    return allocateLocked(size);
}

void SizeClassAllocator::deallocate(void* ptr, usize size) {
    OptionalLock lock(m_mutex, m_threadSafe);
    deallocateLocked(ptr, size);
}

void* SizeClassAllocator::reallocate(void* ptr, usize oldSize, usize newSize) {
    OptionalLock lock(m_mutex, m_threadSafe);
    if (ptr == nullptr) {
        return allocateLocked(newSize);
    }
    if (newSize == 0u) {
        deallocateLocked(ptr, oldSize);
        return nullptr;
    }
    const u32 oldClass = classIndex(oldSize);
    const u32 newClass = classIndex(newSize);
    if (oldClass == newClass && oldClass != kClassCount) {
        return ptr;
    }
    void* next = allocateLocked(newSize);
    if (next == nullptr) {
        return nullptr;
    }
    std::memcpy(next, ptr, oldSize < newSize ? oldSize : newSize);
    deallocateLocked(ptr, oldSize);
    return next;
}

void* SizeClassAllocator::allocateUnsized(usize size) {
    OptionalLock lock(m_mutex, m_threadSafe);
    auto* base = static_cast<u8*>(allocateLocked(size + kUnsizedHeader));
    if (base == nullptr) {
        return nullptr;
    }
    std::memcpy(base, &size, sizeof(size));
    return base + kUnsizedHeader;
}

void SizeClassAllocator::deallocateUnsized(void* ptr) {
    if (ptr == nullptr) {
        return;
    }
    OptionalLock lock(m_mutex, m_threadSafe);
    u8* base = static_cast<u8*>(ptr) - kUnsizedHeader;
    usize size = 0;
    std::memcpy(&size, base, sizeof(size));
    deallocateLocked(base, size + kUnsizedHeader);
}

void* SizeClassAllocator::alloc(AllocInfo info) {
    if (info.alignment > kAlignment || (info.alignment & (info.alignment - 1u)) != 0u) {
        OptionalLock lock(m_mutex, m_threadSafe);
        detail::recordFailedAlloc(m_stats);
        return nullptr;
    }
    return allocate(info.size);
}

void SizeClassAllocator::free(void* ptr, usize size) { deallocate(ptr, size); }

void SizeClassAllocator::releaseAll() {
    while (m_pages != nullptr) {
        PageHeader* next = m_pages->next;
        systemFree(m_pages);
        m_pages = next;
    }
    while (m_oversize != nullptr) {
        OversizeHeader* next = m_oversize->next;
        systemFree(m_oversize);
        m_oversize = next;
    }
    for (FreeBlock*& head : m_free) {
        head = nullptr;
    }
    m_cursor = nullptr;
    m_pageEnd = nullptr;
    m_reserved = 0;
    m_stats.totalBytes = 0;
    m_stats.usedBytes = 0;
}

void SizeClassAllocator::reset() {
    OptionalLock lock(m_mutex, m_threadSafe);
    releaseAll();
}

AllocStats SizeClassAllocator::stats() const {
    OptionalLock lock(m_mutex, m_threadSafe);
    return m_stats;
}

usize SizeClassAllocator::reservedBytes() const {
    OptionalLock lock(m_mutex, m_threadSafe);
    return m_reserved;
}

u64 SizeClassAllocator::systemAllocations() const {
    OptionalLock lock(m_mutex, m_threadSafe);
    return m_systemAllocs;
}

} // namespace fuse::alloc
