// Compiled with AVX2 + FMA flags; called only when forge::lane::detect() reports them.
#include "charts_simd_kernel.hpp"
#if defined(FUSE_UI_CHARTS_LANE) && (defined(__x86_64__) || defined(_M_X64))
namespace fuse::ui::charts::simd {
void minMaxAvx2(const float* v, std::size_t n, float& lo, float& hi) { detail::minMaxLane<forge::lane::x86_64_v3>(v, n, lo, hi); }
}
#endif
