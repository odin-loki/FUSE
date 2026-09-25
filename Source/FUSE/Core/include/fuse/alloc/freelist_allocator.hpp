#pragma once

#include <fuse/alloc/allocator.hpp>

#include <vector>

namespace fuse::alloc {

/// Contiguous arena with a first-fit free-block list. Adjacent free blocks coalesce on free.
/// Not thread-safe — allocate from a single thread only.
class FreeListAllocator final : public IAllocator {
public:
    explicit FreeListAllocator(u32 capacityBytes = 64u * 1024u, const char* name = "freelist");

    void* alloc(AllocInfo info) override;
    void free(void* ptr, usize size) override;
    void reset() override;

    AllocStats stats() const override { return m_stats; }
    const char* name() const override { return m_name; }

    void* allocate(u32 size, u32 alignment = alignof(std::max_align_t));

    u32 usedBytes() const { return static_cast<u32>(m_stats.usedBytes); }
    u32 capacityBytes() const { return static_cast<u32>(m_stats.totalBytes); }
    u64 failedAllocations() const { return m_stats.failedAllocs; }

    template <typename T>
    T* allocate(u32 count = 1) {
        return static_cast<T*>(allocate(static_cast<u32>(sizeof(T) * count), alignof(T)));
    }

private:
    static constexpr u32 kInvalid = 0xFFFFFFFFu;
    static constexpr u32 kFreeBit = 1u;
    static constexpr u32 kMinAlign = 16u;
    static constexpr u32 kMinBlock = 32u;

    struct BlockHeader {
        u32 size = 0;
        u32 prevSize = 0;
        u32 flags = 0;
        u32 nextFree = kInvalid;
    };

    static constexpr u32 kHeaderSize = static_cast<u32>(sizeof(BlockHeader));

    BlockHeader* headerAt(u32 offset);
    const BlockHeader* headerAt(u32 offset) const;
    bool isFree(u32 offset) const;
    void failAlloc();
    void insertFree(u32 offset);
    void unlinkFree(u32 offset);
    void patchNextPrevSize(u32 offset);
    void seedArena();

    const char* m_name = "freelist";
    detail::ArenaBytes m_storage;
    u32 m_capacity = 0;
    u32 m_freeHead = kInvalid;
    AllocStats m_stats{};
};

} // namespace fuse::alloc
