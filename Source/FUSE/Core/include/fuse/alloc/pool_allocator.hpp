#pragma once

#include <fuse/alloc/allocator.hpp>

#include <vector>

namespace fuse::alloc {

/// Fixed-size block pool with O(1) freelist alloc/free (B1.3).
/// Each block carries a generation counter (odd = live, even = free). Freeing a block that is not
/// live (double free) is rejected without touching the freelist, counted as a failed op and raises
/// FUSE_ASSERT in debug builds.
/// Not thread-safe — one instance per worker or game thread.
class PoolAllocator final : public IAllocator {
public:
    PoolAllocator(u32 blockSize, u32 blockCount, const char* name = "pool");

    void* alloc(AllocInfo info) override;
    void free(void* ptr, usize size) override;
    void reset() override;

    AllocStats stats() const override { return m_stats; }
    const char* name() const override { return m_name; }

    u32 blockSize() const { return m_blockSize; }
    u32 blockCount() const { return m_blockCount; }
    u32 availableBlocks() const;

    /// True when `ptr` is the start of a block that is currently allocated.
    bool isLive(const void* ptr) const;

    /// Generation of the block containing `ptr` (bumps on every alloc and free); 0 if not a block.
    u32 generationOf(const void* ptr) const;

    /// Double frees rejected since construction/reset.
    u64 doubleFreeCount() const { return m_doubleFrees; }

    template <typename T>
    T* allocate() {
        return static_cast<T*>(alloc({sizeof(T), alignof(T), nullptr}));
    }

private:
    const char* m_name = "pool";
    u32 m_blockSize = 0;
    u32 m_blockCount = 0;
    detail::ArenaBytes m_storage;
    bool blockIndexOf(const void* ptr, u32& outIndex) const;

    std::vector<u32> m_freeList;
    std::vector<u32> m_generations;
    u64 m_doubleFrees = 0;
    AllocStats m_stats{};
};

} // namespace fuse::alloc
