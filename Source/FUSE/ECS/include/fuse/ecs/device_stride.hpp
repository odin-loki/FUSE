#pragma once

namespace fuse::ecs {

/// True when `stride` is a power of two in [4, 256].
[[nodiscard]] inline constexpr bool device_stride_ok(int stride) {
    if (stride < 4 || stride > 256) {
        return false;
    }
    return (stride & (stride - 1)) == 0;
}

} // namespace fuse::ecs
