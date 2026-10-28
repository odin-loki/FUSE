#pragma once

namespace fuse::renderer {

/// True only for radix digit widths 4, 8, and 16.
[[nodiscard]] inline bool radix_bits_ok(int bits) {
    return bits == 4 || bits == 8 || bits == 16;
}

} // namespace fuse::renderer
