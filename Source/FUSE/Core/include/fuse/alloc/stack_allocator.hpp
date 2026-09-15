#pragma once

#include <fuse/alloc/allocator.hpp>

#include <vector>

namespace fuse::alloc {

using StackMarker = u32;

/// LIFO scratch stack with mark/rollback (B1.3 stub).
/// Not thread-safe — allocate from worker-local instances only.
class StackAllocator final : public IAllocator {
public:
    explicit StackAllocator(u32 capacityBytes = 64u * 1024u, const char* name = "stack");

    void* alloc(AllocInfo info) override;
    void free(void* ptr, usize size) override;
    void reset() override;

    AllocStats stats() const override { return m_stats; }
    const char* name() const override { return m_name; }

    StackMarker pushMark();
    void popToMark(StackMarker mark);

    template <typename T>
    T* allocate(u32 count = 1) {
        return static_cast<T*>(alloc({static_cast<usize>(sizeof(T) * count), alignof(T), nullptr}));
    }

private:
    const char* m_name = "stack";
    std::vector<u8> m_storage;
    u32 m_offset = 0;
    AllocStats m_stats{};
};

} // namespace fuse::alloc
