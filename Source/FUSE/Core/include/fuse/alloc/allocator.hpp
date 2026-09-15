#pragma once

#include <fuse/alloc/alloc_stats.hpp>
#include <fuse/types.hpp>

#include <cstddef>

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

namespace detail {

usize alignUp(usize value, usize alignment);

} // namespace detail

} // namespace fuse::alloc
