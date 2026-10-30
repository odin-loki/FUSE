#include "box_blur.hpp"
#include "box_blur_kernel.hpp"
#if defined(FUSE_UI_HAS_FORGE_LANE)
#include <forge/lane/dispatch.hpp>
#endif

namespace fuse::ui::simd {
#if defined(FUSE_UI_HAS_FORGE_LANE) && (defined(__x86_64__) || defined(_M_X64))
void boxVerticalAvx2(const float*, float*, int, int, int, float);
#endif
namespace {
using Fn = void (*)(const float*, float*, int, int, int, float);
struct Pick { Fn fn; const char* name; };
void scalarFn(const float* s, float* d, int stride, int h, int r, float inv) { detail::boxVerticalScalar(s, d, stride, h, r, inv, 0); }
#if defined(__aarch64__) && defined(__ARM_NEON)
void neonFn(const float* s, float* d, int stride, int h, int r, float inv) {
    detail::boxVerticalLane<forge::lane::apple_m>(s, d, stride, h, r, inv); // NEON is baseline on aarch64
}
#endif
Pick pick() {
#if defined(FUSE_UI_HAS_FORGE_LANE) && (defined(__x86_64__) || defined(_M_X64))
    const auto& f = forge::lane::detect();
    if (f.avx2 && f.fma && f.os_avx) return {&boxVerticalAvx2, "avx2"};
#endif
#if defined(__aarch64__) && defined(__ARM_NEON)
    return {&neonFn, "neon"};
#endif
    return {&scalarFn, "scalar"};
}
const Pick& active() { static const Pick p = pick(); return p; }
} // namespace
void boxVertical(const float* s, float* d, int stride, int h, int r, float inv) { active().fn(s, d, stride, h, r, inv); }
const char* boxTarget() { return active().name; }
} // namespace fuse::ui::simd
