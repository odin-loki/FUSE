#pragma once
#include <forge/lane/vec.hpp>
#include <algorithm>

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
    namespace L = forge::lane;
    using V = L::vec<float, Tg>;
    constexpr int N = V::N;
    const V vinv = L::splat<Tg>(inv);
    int j = 0;
    for (; j + N <= stride; j += N) {
        auto row = [&](int i) { return src + std::size_t(std::clamp(i, 0, h - 1)) * std::size_t(stride) + std::size_t(j); };
        V acc = L::splat<Tg>(0.f);
        for (int i = -r; i <= r; ++i) acc = acc + L::load<Tg>(row(i));
        for (int i = 0; i < h; ++i) {
            L::store(dst + std::size_t(i) * std::size_t(stride) + std::size_t(j), acc * vinv);
            acc = acc + (L::load<Tg>(row(i + r + 1)) - L::load<Tg>(row(i - r)));
        }
    }
    boxVerticalScalar(src, dst, stride, h, r, inv, j);
}
} // namespace fuse::ui::simd::detail
