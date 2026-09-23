#pragma once

#include <fuse/alloc/alloc_stats.hpp>
#include <fuse/types.hpp>

#include <cstddef>
#include <new>
#include <vector>

namespace fuse::alloc {

struct AllocInfo {
    usize size = 0;
    usize alignment = alignof(std::max_align_t);
    const char* tag = nullptr;
};

/// Common allocator surface for FUSE memory domains (B1.3 stub hierarchy).
class IAllocator {
public:
    virtual ~IAllocator() = default;

    virtual void* alloc(AllocInfo info) = 0;
    virtual void free(void* ptr, usize size) = 0;
    virtual void reset() = 0;

    virtual AllocStats stats() const = 0;
    virtual const char* name() const = 0;
};

/// Base alignment of every allocator arena. Offset-based alignment inside an arena only yields an
/// aligned address when the arena itself is at least this aligned, so requests up to this value
/// are honoured and larger (or non power-of-two) alignments fail the allocation.
inline constexpr usize kArenaAlignment = 256u;

namespace detail {

usize alignUp(usize value, usize alignment);

/// True for power-of-two alignments no larger than kArenaAlignment (0 is treated as 1).
inline bool isSupportedAlignment(usize alignment) {
    return alignment <= kArenaAlignment && (alignment & (alignment - 1u)) == 0u;
}

/// std::allocator replacement that places arena storage on a kArenaAlignment boundary.
template <typename T>
struct ArenaStorageAllocator {
    using value_type = T;

    ArenaStorageAllocator() = default;
    template <typename U>
    ArenaStorageAllocator(const ArenaStorageAllocator<U>& /*other*/) {}

    T* allocate(std::size_t n) {
        return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{kArenaAlignment}));
    }
    void deallocate(T* ptr, std::size_t /*n*/) { ::operator delete(ptr, std::align_val_t{kArenaAlignment}); }

    template <typename U>
    bool operator==(const ArenaStorageAllocator<U>& /*other*/) const { return true; }
};

/// Byte arena backing store (kArenaAlignment-aligned data()).
using ArenaBytes = std::vector<u8, ArenaStorageAllocator<u8>>;

} // namespace detail

} // namespace fuse::alloc
