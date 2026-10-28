#pragma once

#include <cstdint>

namespace fuse::hybrid {

/// Worker count in `1 .. min(hardware, 64)`. Zero hardware is treated as one core.
[[nodiscard]] inline std::uint32_t clamp_worker_count(std::uint32_t requested, std::uint32_t hardware) {
    constexpr std::uint32_t kMaxWorkers = 64u;
    const std::uint32_t cores = hardware == 0u ? 1u : hardware;
    const std::uint32_t limit = cores < kMaxWorkers ? cores : kMaxWorkers;
    if (requested < 1u) {
        return 1u;
    }
    return requested > limit ? limit : requested;
}

} // namespace fuse::hybrid
