// FUSE Relight: linear index of one radiance-cache cell on a cubic grid.
#pragma once

#include <cstdint>
#include <limits>

namespace fuse::relight::render::pathtrace {

/// Index of cell (`x`, `y`, `z`) on a `resolution` cube.
/// `resolution <= 0` returns `UINT32_MAX`. Each coordinate wraps into `0 .. resolution - 1`
/// with a positive modulo, so negative inputs wrap. The index is
/// `(z * resolution + y) * resolution + x`, computed in uint64 and saturated to `UINT32_MAX`
/// when a multiply overflows or the result does not fit in uint32.
inline std::uint32_t cache_cell_index(int x, int y, int z, int resolution) {
    if (resolution <= 0) {
        return UINT32_MAX;
    }
    const std::uint64_t res = static_cast<std::uint64_t>(resolution);
    const auto wrap = [res](int coord) -> std::uint64_t {
        std::int64_t mod = static_cast<std::int64_t>(coord) % static_cast<std::int64_t>(res);
        if (mod < 0) {
            mod += static_cast<std::int64_t>(res);
        }
        return static_cast<std::uint64_t>(mod);
    };
    const std::uint64_t cx = wrap(x);
    const std::uint64_t cy = wrap(y);
    const std::uint64_t cz = wrap(z);

    constexpr std::uint64_t kMaxU64 = std::numeric_limits<std::uint64_t>::max();
    if (cz > kMaxU64 / res) {
        return UINT32_MAX;
    }
    const std::uint64_t zScaled = cz * res;
    if (zScaled > kMaxU64 - cy) {
        return UINT32_MAX;
    }
    const std::uint64_t zy = zScaled + cy;
    if (zy > kMaxU64 / res) {
        return UINT32_MAX;
    }
    const std::uint64_t scaled = zy * res;
    if (scaled > kMaxU64 - cx) {
        return UINT32_MAX;
    }
    const std::uint64_t index = scaled + cx;
    if (index > UINT32_MAX) {
        return UINT32_MAX;
    }
    return static_cast<std::uint32_t>(index);
}

} // namespace fuse::relight::render::pathtrace
