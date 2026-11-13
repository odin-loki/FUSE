// Vertical box-filter pass of the CPU backdrop blur, vectorised with FORGE lane (forge::lane::vec) and picked at
// run time with forge::lane::detect(). Results are bit-identical to the scalar reference (same per-element
// operation order, no contraction), so CPU raster goldens do not change.
#pragma once
namespace fuse::ui::simd {
/// rows of `stride` floats, `h` rows; dst[i] = mean of src rows clamp(i-r .. i+r).
void boxVertical(const float* src, float* dst, int stride, int h, int r, float inv);
/// Horizontal pass over `h` rows of `w` RGBA pixels (4 floats each): pixel-transpose, the vertical lane sweep, and
/// back. `scratch` holds w*h*4 floats. Bit-identical to the scalar per-channel sliding window.
void boxHorizontalN(float* buf, float* tmp, float* scratch, int w, int h, int r, float inv, int passes);
void boxHorizontal(float* buf, float* tmp, float* scratch, int w, int h, int r, float inv);
/// Stores `value` into dst[0..n) with FORGE-lane vector stores (u32 bits carried in float lanes, moves only).
void fillSpan(unsigned* dst, int n, unsigned value);
/// Name of the target picked ("avx512", "avx2", "neon", "scalar").
const char* boxTarget();
}
