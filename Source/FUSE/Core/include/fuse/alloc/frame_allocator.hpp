#pragma once

#include <fuse/alloc/allocator.hpp>

#include <array>
#include <vector>

namespace fuse::alloc {

/// Per-frame bump allocator with ping-pong buffers for one-frame debug retention (B1.3).
/// Not thread-safe — allocate from worker-local instances only.
class FrameAllocator final : public IAllocator {
public:
    explicit FrameAllocator(u32 capacityBytes = 64u * 1024u, const char* name = "frame");

    void* alloc(AllocInfo info) override;
    void free(void* ptr, usize size) override;
    void reset() override;

    AllocStats stats() const override { return m_stats; }
    const char* name() const override { return m_name; }

    void* allocate(u32 size, u32 alignment = alignof(std::max_align_t));
    void advanceFrame();

    u32 usedBytes() const { return static_cast<u32>(m_stats.usedBytes); }
    u32 capacityBytes() const { return static_cast<u32>(m_stats.totalBytes); }
    u32 peakUsedBytes() const { return static_cast<u32>(m_stats.peakUsedBytes); }
    u64 allocationCount() const { return m_stats.allocCount; }
    u64 failedAllocations() const { return m_stats.failedAllocs; }

    /// Read-only view of the previous frame buffer (valid until the next advanceFrame).
    const u8* previousFrameData() const;
    u32 previousFrameUsedBytes() const { return m_previousUsedBytes; }

    template <typename T>
    T* allocate(u32 count = 1) {
        return static_cast<T*>(allocate(static_cast<u32>(sizeof(T) * count), alignof(T)));
    }

private:
    u8* activeBuffer();
    const u8* activeBuffer() const;

    const char* m_name = "frame";
    std::array<std::vector<u8>, 2> m_buffers{};
    u32 m_activeIndex = 0;
    u32 m_offset = 0;
    u32 m_previousUsedBytes = 0;
    AllocStats m_stats{};
};

} // namespace fuse::alloc
