#include <fuse/legacy/parallel_for.hpp>

#include <atomic>

namespace fuse::legacy {

u32 parallel_for_smoke_sum(u32 begin, u32 end, u32 grainSize) {
    std::atomic<u32> sum{0};
    parallel_for_indices(begin, end, grainSize, [&](u32 i) {
        sum.fetch_add(i, std::memory_order_relaxed);
    });
    return sum.load(std::memory_order_acquire);
}

} // namespace fuse::legacy
