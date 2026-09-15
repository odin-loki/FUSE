#pragma once

#include <fuse/jobs/parallel_for.hpp>
#include <fuse/types.hpp>

#include <utility>

namespace fuse::legacy {

/// Routes legacy read-only index loops through `fuse::jobs::parallel_for`.
/// Intended drop-in for Tier A/B sites listed in legacy-parallel-audit.md.
template <typename Fn>
inline void parallel_for_indices(u32 begin, u32 end, u32 grainSize, Fn&& fn) {
    fuse::jobs::parallel_for(begin, end, grainSize, std::forward<Fn>(fn));
}

/// Harmless WP-11 stub: parallel sum of [begin, end) used by runtime smoke.
u32 parallel_for_smoke_sum(u32 begin, u32 end, u32 grainSize);

} // namespace fuse::legacy
