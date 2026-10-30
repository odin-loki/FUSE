// AArch64: NEON is baseline (Advanced SIMD is mandatory in ARMv8-A), so this TU needs no extra flags and no detection.
#include "charts_simd_kernel.hpp"
#if defined(FUSE_UI_CHARTS_LANE) && defined(__aarch64__) && defined(__ARM_NEON)
namespace fuse::ui::charts::simd {
namespace {
struct neon128 : forge::lane::caps<forge::lane::neon> {
    using caps_type = forge::lane::caps<forge::lane::neon>;
    static constexpr const char* name = "neon";
};
} // namespace
void minMaxNeon(const float* v, std::size_t n, float& lo, float& hi) { detail::minMaxLane<neon128>(v, n, lo, hi); }
} // namespace fuse::ui::charts::simd
#endif
