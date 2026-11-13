#pragma once
#include <cmath>
#include <cstddef>
#if defined(FUSE_UI_CHARTS_LANE)
#include <forge/lane/vec.hpp>
#endif
namespace fuse::ui::charts::simd::detail {
inline void minMaxScalar(const float* v, std::size_t n, float& lo, float& hi) {
    lo = INFINITY, hi = -INFINITY;
    for (std::size_t i = 0; i < n; ++i) {
        const float x = v[i];
        lo = x < lo ? x : lo; // NaN compares false: skipped
        hi = x > hi ? x : hi;
    }
    if (lo > hi) lo = hi = 0.f; // empty / all NaN
}
#if defined(FUSE_UI_CHARTS_LANE)
template <forge::lane::Target Tg>
void minMaxLane(const float* v, std::size_t n, float& lo, float& hi) {
    namespace L = forge::lane;
    using V = L::vec<float, Tg>;
    constexpr std::size_t N = std::size_t(V::N);
    if (n < N * 2) { minMaxScalar(v, n, lo, hi); return; }
    // NaN-safe: forge max/min return the second operand when unordered, so the accumulator goes second
    V mn = L::splat<Tg>(INFINITY), mx = L::splat<Tg>(-INFINITY);
    std::size_t i = 0;
    for (; i + N <= n; i += N) {
        const V x = L::load<Tg>(v + i);
        mn = L::min(x, mn), mx = L::max(x, mx);
    }
    float a[N], b[N];
    L::store(a, mn), L::store(b, mx);
    lo = INFINITY, hi = -INFINITY;
    for (std::size_t k = 0; k < N; ++k) lo = a[k] < lo ? a[k] : lo, hi = b[k] > hi ? b[k] : hi;
    for (; i < n; ++i) lo = v[i] < lo ? v[i] : lo, hi = v[i] > hi ? v[i] : hi;
    if (lo > hi) lo = hi = 0.f;
}
#endif
} // namespace fuse::ui::charts::simd::detail
