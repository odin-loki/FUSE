#include "charts_simd.hpp"
#include "charts_simd_kernel.hpp"
#if defined(FUSE_UI_CHARTS_LANE)
#include <forge/lane/dispatch.hpp>
#endif

namespace fuse::ui::charts::simd {
#if defined(FUSE_UI_CHARTS_LANE) && (defined(__x86_64__) || defined(_M_X64))
void minMaxAvx2(const float*, std::size_t, float&, float&);
#endif
#if defined(FUSE_UI_CHARTS_LANE) && defined(__aarch64__) && defined(__ARM_NEON)
void minMaxNeon(const float*, std::size_t, float&, float&);
#endif
namespace {
using Fn = void (*)(const float*, std::size_t, float&, float&);
Fn pick() {
#if defined(FUSE_UI_CHARTS_LANE) && (defined(__x86_64__) || defined(_M_X64))
    const auto& f = forge::lane::detect();
    if (f.avx2 && f.fma && f.os_avx) return &minMaxAvx2;
#endif
#if defined(FUSE_UI_CHARTS_LANE) && defined(__aarch64__) && defined(__ARM_NEON)
    return &minMaxNeon;
#endif
    return &detail::minMaxScalar;
}
Fn active() { static const Fn f = pick(); return f; }
} // namespace
void minMaxF32(const float* v, std::size_t n, float& lo, float& hi) { active()(v, n, lo, hi); }
void argMinMaxF32(const float* v, std::size_t n, std::size_t& imin, std::size_t& imax) {
    // value pass on SIMD, then the first index of each extreme (exactly the scalar first-occurrence answer)
    imin = imax = 0;
    if (!n) return;
    float lo, hi;
    minMaxF32(v, n, lo, hi);
    bool fmin = false, fmax = false;
    for (std::size_t i = 0; i < n && !(fmin && fmax); ++i) {
        if (!fmin && v[i] == lo) imin = i, fmin = true;
        if (!fmax && v[i] == hi) imax = i, fmax = true;
    }
}
} // namespace fuse::ui::charts::simd
