#pragma once

#include <fuse/alloc/allocator.hpp>
#include <fuse/types.hpp>

#include <mutex>

namespace fuse::alloc {

struct SizeClassAllocatorDesc {
    const char* name = "sizeclass";
    /// Bytes requested from the system heap each time the pool runs dry (one "page"; blocks of
    /// every size class are carved from the current page).
    usize pageBytes = 64u * 1024u;
    /// Size of the first page, obtained up front (0 = lazily, on the first allocation).
    usize reserveBytes = 0;
    /// Guard every operation with a mutex (for pools shared by several threads, e.g. ENet).
    bool threadSafe = false;
};

/// General-purpose pooled allocator for third-party runtimes that allocate per frame through a
/// callback (Lua's lua_Alloc, ENet's enet_malloc): 40 size classes from 16 B to 32 KiB (16-byte
/// steps up to 128 B, then four classes per power of two), each with an intrusive free list.
///
/// Blocks are carved from pages obtained from the system heap; freed blocks go back to their
/// class's free list and are never returned to the system until reset()/destruction. Once the
/// working set of a steady-state frame loop has been reached, alloc/free/realloc therefore
/// perform no system heap allocations at all. Requests above 32 KiB are served directly by the
/// system heap (counted in `systemAllocations()`).
///
/// All blocks are 16-byte aligned. The sized API (`allocate`/`deallocate`/`reallocate`) needs
/// the requested size back on free, as lua_Alloc provides; the unsized API stores it in a
/// 16-byte header, for callbacks that free by pointer only (enet_free).
class SizeClassAllocator final : public IAllocator {
public:
    static constexpr usize kAlignment = 16u;
    static constexpr usize kMaxClassBytes = 32u * 1024u;
    static constexpr u32 kClassCount = 40u;

    explicit SizeClassAllocator(const SizeClassAllocatorDesc& desc = {});
    ~SizeClassAllocator() override;
    SizeClassAllocator(const SizeClassAllocator&) = delete;
    SizeClassAllocator& operator=(const SizeClassAllocator&) = delete;

    // IAllocator: `size` passed to free() must be the size passed to alloc().
    void* alloc(AllocInfo info) override;
    void free(void* ptr, usize size) override;
    /// Releases every page and oversize block back to the system heap. All outstanding blocks
    /// become invalid.
    void reset() override;
    AllocStats stats() const override;
    const char* name() const override { return m_name; }

    /// nullptr when `size` is 0 or the system heap is exhausted.
    void* allocate(usize size);
    void deallocate(void* ptr, usize size);
    /// realloc semantics with the old size supplied: `ptr == nullptr` allocates, `newSize == 0`
    /// frees (returns nullptr). A block that stays in its size class is returned unchanged. On
    /// failure returns nullptr and leaves `ptr` untouched.
    void* reallocate(void* ptr, usize oldSize, usize newSize);

    void* allocateUnsized(usize size);
    void deallocateUnsized(void* ptr);

    /// Bytes currently held from the system heap (pages + live oversize blocks).
    [[nodiscard]] usize reservedBytes() const;
    /// Number of system-heap allocations made so far (page refills + oversize blocks). Constant
    /// across frames once a frame loop's working set is pooled.
    [[nodiscard]] u64 systemAllocations() const;

    /// Size class of a request (kClassCount for oversize) and the block size of a class.
    [[nodiscard]] static u32 classIndex(usize size);
    [[nodiscard]] static usize classBytes(u32 classIndex);

private:
    struct FreeBlock {
        FreeBlock* next;
    };
    // alignas: blocks start right after a header, so every header spans a multiple of kAlignment
    // (on 32-bit targets the members alone are 8 bytes).
    struct alignas(kAlignment) PageHeader {
        PageHeader* next;
        usize bytes;
    };
    struct alignas(kAlignment) OversizeHeader {
        OversizeHeader* prev;
        OversizeHeader* next;
        usize bytes;
        usize pad;
    };
    static_assert(sizeof(PageHeader) % kAlignment == 0u && sizeof(OversizeHeader) % kAlignment == 0u,
                  "blocks after a page / oversize header must stay kAlignment-aligned");

    void* allocateLocked(usize size);
    void deallocateLocked(void* ptr, usize size);
    bool refill(usize minBytes);
    void releaseAll();

    const char* m_name = "sizeclass";
    usize m_pageBytes = 0;
    bool m_threadSafe = false;
    mutable std::mutex m_mutex;

    FreeBlock* m_free[kClassCount] = {};
    PageHeader* m_pages = nullptr;
    u8* m_cursor = nullptr;
    u8* m_pageEnd = nullptr;
    OversizeHeader* m_oversize = nullptr;
    usize m_reserved = 0;
    u64 m_systemAllocs = 0;
    AllocStats m_stats{};
};

} // namespace fuse::alloc
