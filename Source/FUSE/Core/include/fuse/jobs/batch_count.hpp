#pragma once

#include <cstdint>

namespace fuse::jobs {

/// Batches required to cover `count` items of size `batch`.
/// A zero batch or zero count returns 0. Otherwise ceil(count / batch), saturating at UINT32_MAX.
[[nodiscard]] inline std::uint32_t job_batch_count(std::uint32_t count, std::uint32_t batch) {
    if (batch == 0u || count == 0u) {
        return 0u;
    }
    const std::uint64_t batches =
        (static_cast<std::uint64_t>(count) + static_cast<std::uint64_t>(batch) - 1ull) / batch;
    if (batches > UINT32_MAX) {
        return UINT32_MAX;
    }
    return static_cast<std::uint32_t>(batches);
}

} // namespace fuse::jobs
