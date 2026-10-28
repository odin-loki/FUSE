#pragma once

#include <fuse/types.hpp>

#include <cstdint>

namespace fuse::math {

namespace detail {

/// Insert a zero between each of the low 16 bits. Bit i of `v` lands at bit 2i.
[[nodiscard]] FUSE_HOST_DEVICE inline std::uint32_t mortonSpread16(std::uint32_t v) {
    v &= 0x0000FFFFu;
    v = (v | (v << 8)) & 0x00FF00FFu;
    v = (v | (v << 4)) & 0x0F0F0F0Fu;
    v = (v | (v << 2)) & 0x33333333u;
    v = (v | (v << 1)) & 0x55555555u;
    return v;
}

/// Inverse of mortonSpread16: pack even bits 0,2,4,... back into the low 16 bits.
[[nodiscard]] FUSE_HOST_DEVICE inline std::uint32_t mortonCompact16(std::uint32_t v) {
    v &= 0x55555555u;
    v = (v | (v >> 1)) & 0x33333333u;
    v = (v | (v >> 2)) & 0x0F0F0F0Fu;
    v = (v | (v >> 4)) & 0x00FF00FFu;
    v = (v | (v >> 8)) & 0x0000FFFFu;
    return v;
}

} // namespace detail

/// 2D Morton (Z-order) code. Interleaves the low 16 bits of `x` and `y`.
/// Bit 0 of the result is bit 0 of `x`; bit 1 is bit 0 of `y`. Higher bits of `x` and `y` are ignored.
[[nodiscard]] FUSE_HOST_DEVICE inline std::uint32_t morton2(std::uint32_t x, std::uint32_t y) {
    return detail::mortonSpread16(x) | (detail::mortonSpread16(y) << 1);
}

/// Low 16 bits of `x` from a code produced by morton2.
[[nodiscard]] FUSE_HOST_DEVICE inline std::uint32_t morton2_x(std::uint32_t code) {
    return detail::mortonCompact16(code);
}

/// Low 16 bits of `y` from a code produced by morton2.
[[nodiscard]] FUSE_HOST_DEVICE inline std::uint32_t morton2_y(std::uint32_t code) {
    return detail::mortonCompact16(code >> 1);
}

} // namespace fuse::math
