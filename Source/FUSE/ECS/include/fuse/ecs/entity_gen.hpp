#pragma once

namespace fuse::ecs {

/// True when `gen` is in the inclusive range 0..1000000.
[[nodiscard]] inline constexpr bool entity_gen_ok(int gen) {
    return gen >= 0 && gen <= 1000000;
}

} // namespace fuse::ecs
