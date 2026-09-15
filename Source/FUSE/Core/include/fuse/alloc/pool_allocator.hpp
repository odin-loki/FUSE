#pragma once

#include <fuse/alloc/allocator.hpp>

#include <vector>

namespace fuse::alloc {

/// Fixed-size block pool with O(1) freelist alloc/free (B1.3 stub).
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

    template <typename T>
    T* allocate() {
        return static_cast<T*>(alloc({sizeof(T), alignof(T), nullptr}));
    }

private:
    const char* m_name = "pool";
    u32 m_blockSize = 0;
    u32 m_blockCount = 0;
    std::vector<u8> m_storage;
    std::vector<u32> m_freeList;
    AllocStats m_stats{};
};

} // namespace fuse::alloc
