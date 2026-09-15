#pragma once

#include <fuse/types.hpp>
#include <functional>

namespace fuse::jobs {

/// Parallel index range (stub — executes serially when workers == 0).
template <typename Body>
void parallel_for(u32 begin, u32 end, u32 grainSize, const Body& body) {
    if (grainSize == 0) {
        grainSize = 1;
    }
    for (u32 i = begin; i < end; i += grainSize) {
        const u32 chunkEnd = (i + grainSize < end) ? (i + grainSize) : end;
        for (u32 j = i; j < chunkEnd; ++j) {
            body(j);
        }
    }
}

inline void parallel_for(u32 count, const std::function<void(u32)>& body) {
    parallel_for(0, count, 1, body);
}

} // namespace fuse::jobs
