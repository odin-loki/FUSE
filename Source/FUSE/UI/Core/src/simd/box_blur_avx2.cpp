// Compiled with -mavx2 -mfma -mf16c; called only when forge::lane::detect() reports AVX2 + FMA.
#include "box_blur_kernel.hpp"
namespace fuse::ui::simd {
void boxVerticalAvx2(const float* s, float* d, int stride, int h, int r, float inv) {
    detail::boxVerticalLane<forge::lane::x86_64_v3>(s, d, stride, h, r, inv);
}
}
