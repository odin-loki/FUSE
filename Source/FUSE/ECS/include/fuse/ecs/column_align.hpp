#pragma once

namespace fuse::ecs {

/// True when `align` is a power of two in [4, 64].
[[nodiscard]] inline constexpr bool column_align_ok(int align) {
    if (align < 4 || align > 64) {
        return false;
    }
    return (align & (align - 1)) == 0;
}

} // namespace fuse::ecs
