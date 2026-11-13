#pragma once
#include <forge/lane/vec.hpp>
#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>

namespace fuse::ui::simd::detail {
inline void boxVerticalScalar(const float* src, float* dst, int stride, int h, int r, float inv, int j0) {
    for (int j = j0; j < stride; ++j) {
        auto at = [&](int i) { return src[std::size_t(std::clamp(i, 0, h - 1)) * std::size_t(stride) + std::size_t(j)]; };
        float acc = 0.f;
        for (int i = -r; i <= r; ++i) acc += at(i);
        for (int i = 0; i < h; ++i) {
            dst[std::size_t(i) * std::size_t(stride) + std::size_t(j)] = acc * inv;
            acc += at(i + r + 1) - at(i - r);
        }
    }
}
template <forge::lane::Target Tg>
void boxVerticalLane(const float* src, float* dst, int stride, int h, int r, float inv) {
    // row-major sweep with a running-sum row (same per-column op order as the scalar reference, so identical
    // results): every row is read and written contiguously instead of one cache-hostile column strip at a time
    namespace L = forge::lane;
    using V = L::vec<float, Tg>;
    constexpr int N = V::N;
    const V vinv = L::splat<Tg>(inv);
    const int jv = stride - stride % N;
    thread_local std::vector<float> accBuf;
    accBuf.assign(std::size_t(stride), 0.f);
    float* acc = accBuf.data();
    auto row = [&](int i) { return src + std::size_t(std::clamp(i, 0, h - 1)) * std::size_t(stride); };
    for (int i = -r; i <= r; ++i) {
        const float* s = row(i);
        for (int j = 0; j < jv; j += N) L::store(acc + j, L::load<Tg>(acc + j) + L::load<Tg>(s + j));
    }
    for (int i = 0; i < h; ++i) {
        const float* add = row(i + r + 1);
        const float* sub = row(i - r);
        float* d = dst + std::size_t(i) * std::size_t(stride);
        for (int j = 0; j < jv; j += N) {
            const V a = L::load<Tg>(acc + j);
            L::store(d + j, a * vinv);
            L::store(acc + j, a + (L::load<Tg>(add + j) - L::load<Tg>(sub + j)));
        }
    }
    if (jv < stride) boxVerticalScalar(src, dst, stride, h, r, inv, jv);
}
template <forge::lane::Target Tg>
void fillSpanLane(unsigned* dst, int n, unsigned value) {
    namespace L = forge::lane;
    constexpr int N = L::vec<float, Tg>::N;
    const auto v = L::splat<Tg>(std::bit_cast<float>(value));
    int i = 0;
    float* f = reinterpret_cast<float*>(dst);
    for (; i + N <= n; i += N) L::store(f + i, v);
    for (; i < n; ++i) dst[i] = value;
}
} // namespace fuse::ui::simd::detail
