// Vertical box-filter pass of the CPU backdrop blur, vectorised with FORGE lane (forge::lane::vec) and picked at
// run time with forge::lane::detect(). Results are bit-identical to the scalar reference (same per-element
// operation order, no contraction), so CPU raster goldens do not change.
#pragma once
namespace fuse::ui::simd {
/// rows of `stride` floats, `h` rows; dst[i] = mean of src rows clamp(i-r .. i+r).
void boxVertical(const float* src, float* dst, int stride, int h, int r, float inv);
/// Name of the target picked ("avx512", "avx2", "neon", "scalar").
const char* boxTarget();
}
