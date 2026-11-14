#include "box_blur.hpp"
#include <cstring>
#include <utility>
#include "box_blur_kernel.hpp"
#if defined(FUSE_UI_HAS_FORGE_LANE)
#include <forge/lane/dispatch.hpp>
#endif

namespace fuse::ui::simd {
#if defined(FUSE_UI_HAS_FORGE_LANE) && (defined(__x86_64__) || defined(_M_X64))
void boxVerticalAvx2(const float*, float*, int, int, int, float);
void fillSpanAvx2(unsigned*, int, unsigned);
#endif
namespace {
using Fn = void (*)(const float*, float*, int, int, int, float);
using FillFn = void (*)(unsigned*, int, unsigned);
struct Pick { Fn fn; const char* name; FillFn fill; };
void scalarFill(unsigned* d, int n, unsigned v) { for (int i = 0; i < n; ++i) d[i] = v; }
void scalarFn(const float* s, float* d, int stride, int h, int r, float inv) { detail::boxVerticalScalar(s, d, stride, h, r, inv, 0); }
#if defined(__aarch64__) && defined(__ARM_NEON)
void neonFn(const float* s, float* d, int stride, int h, int r, float inv) {
    detail::boxVerticalLane<forge::lane::apple_m>(s, d, stride, h, r, inv); // NEON is baseline on aarch64
}
void neonFill(unsigned* d, int n, unsigned v) { detail::fillSpanLane<forge::lane::apple_m>(d, n, v); }
#endif
Pick pick() {
#if defined(FUSE_UI_HAS_FORGE_LANE) && (defined(__x86_64__) || defined(_M_X64))
    const auto& f = forge::lane::detect();
    if (f.avx2 && f.fma && f.os_avx) return {&boxVerticalAvx2, "avx2", &fillSpanAvx2};
#endif
#if defined(__aarch64__) && defined(__ARM_NEON)
    return {&neonFn, "neon", &neonFill};
#endif
    return {&scalarFn, "scalar", &scalarFill};
}
const Pick& active() { static const Pick p = pick(); return p; }
} // namespace
void boxVertical(const float* s, float* d, int stride, int h, int r, float inv) { active().fn(s, d, stride, h, r, inv); }
void fillSpan(unsigned* d, int n, unsigned v) { if (n > 0) active().fill(d, n, v); }
void boxHorizontal(float* buf, float* tmp, float* scratch, int w, int h, int r, float inv) {
    // buf: h rows x w px -> scratch: w rows x h px (pixels of 16 bytes moved whole)
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            std::memcpy(scratch + (std::size_t(x) * std::size_t(h) + std::size_t(y)) * 4,
                        buf + (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4, 16);
    active().fn(scratch, tmp, h * 4, w, r, inv);
    for (int x = 0; x < w; ++x)
        for (int y = 0; y < h; ++y)
            std::memcpy(buf + (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4,
                        tmp + (std::size_t(x) * std::size_t(h) + std::size_t(y)) * 4, 16);
}
void boxHorizontalN(float* buf, float* tmp, float* scratch, int w, int h, int r, float inv, int passes) {
    // one transpose each way for all passes (box passes commute), instead of two per pass
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            std::memcpy(scratch + (std::size_t(x) * std::size_t(h) + std::size_t(y)) * 4,
                        buf + (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4, 16);
    float* a = scratch;
    float* b = tmp;
    for (int i = 0; i < passes; ++i) { active().fn(a, b, h * 4, w, r, inv); std::swap(a, b); }
    for (int x = 0; x < w; ++x)
        for (int y = 0; y < h; ++y)
            std::memcpy(buf + (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4,
                        a + (std::size_t(x) * std::size_t(h) + std::size_t(y)) * 4, 16);
}
const char* boxTarget() { return active().name; }
} // namespace fuse::ui::simd
