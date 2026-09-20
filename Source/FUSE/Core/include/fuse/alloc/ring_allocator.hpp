#pragma once

#include <fuse/alloc/allocator.hpp>

#include <vector>

namespace fuse::alloc {

/// Fixed-capacity ring allocator. Alloc advances head; free of the oldest block rewinds.
/// Not thread-safe — allocate from a single thread only.
class RingAllocator final : public IAllocator {
public:
    explicit RingAllocator(u32 capacityBytes = 64u * 1024u, const char* name = "ring");

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
    struct Header {
        u32 payloadSize = 0;
        u32 stride = 0;
        u32 payloadOffset = 0;
        u32 headerOffset = 0;
    };

    static constexpr u32 kHeaderBytes = static_cast<u32>(sizeof(Header));

    Header* headerAt(u32 offset);
    const Header* headerAt(u32 offset) const;
    void failAlloc();
    void skipWrapMarkers();
    bool place(u32 start, u32 limit, usize size, usize alignment, Header& out) const;

    const char* m_name = "ring";
    std::vector<u8> m_storage;
    u32 m_capacity = 0;
    u32 m_head = 0;
    u32 m_tail = 0;
    u32 m_used = 0;
    u32 m_liveCount = 0;
    AllocStats m_stats{};
};

} // namespace fuse::alloc
