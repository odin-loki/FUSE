#pragma once

#include <fuse/types.hpp>

namespace fuse::ecs::detail {

/// Shared grain-size clamp for `each_parallel` and TransformSystem dirty-root passes.
[[nodiscard]] inline u32 normalize_batch_size(u32 batchSize) {
    return batchSize == 0 ? 1u : batchSize;
}

} // namespace fuse::ecs::detail
