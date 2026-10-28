#pragma once

namespace fuse::physics {

/// True when a cloth pin mask uses only the low 4 bits (corners).
[[nodiscard]] inline bool cloth_pin_ok(unsigned mask) {
    return (mask & ~0b1111u) == 0u;
}

} // namespace fuse::physics
