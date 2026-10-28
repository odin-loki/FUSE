#pragma once

namespace fuse::ecs {

/// True when `batch` is in the inclusive range 1..256.
[[nodiscard]] inline constexpr bool parallel_batch_ok(int batch) {
    return batch >= 1 && batch <= 256;
}

} // namespace fuse::ecs
