// Charts SIMD kernels (min / max scans for ranges and decimation) on forge::lane, with a scalar fallback.
#pragma once
#include <cstddef>
namespace fuse::ui::charts::simd {
void minMaxF32(const float* v, std::size_t n, float& lo, float& hi);
/// Indices of the first minimum and first maximum (NaNs ignored; all-NaN: 0, 0).
void argMinMaxF32(const float* v, std::size_t n, std::size_t& imin, std::size_t& imax);
} // namespace fuse::ui::charts::simd
