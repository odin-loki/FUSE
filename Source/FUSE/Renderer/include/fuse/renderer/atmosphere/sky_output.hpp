#pragma once

#include <fuse/types.hpp>

namespace fuse::renderer {

/// Triangular-PDF dither offset in (-1, 1) LSB for a pixel and frame (sum of two decorrelated
/// uniform hashes minus one). Added before rounding it makes the quantisation error mean- and
/// variance-independent of the signal, which removes contouring in slow sky gradients.
f32 sky_dither_tpdf(u32 x, u32 y, u32 frame);

/// Quantise `value` (clamped to [0, 1]) to a `bits`-bit unorm code (1..16 bits), adding
/// `dither_lsb` least-significant-bits of offset before round-to-nearest.
u32 sky_quantize_unorm(f32 value, u32 bits, f32 dither_lsb = 0.f);

} // namespace fuse::renderer
