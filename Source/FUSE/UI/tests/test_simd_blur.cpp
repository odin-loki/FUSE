// FORGE-lane box blur must be bit-identical to the scalar reference for all widths / radii (golden stability).
#include "../Core/src/simd/box_blur.hpp"
#include "../Core/src/simd/box_blur_kernel.hpp"
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
int main() {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> d(0.f, 1.f);
    int bad = 0;
    for (int stride : {1, 3, 4, 7, 8, 16, 29, 64, 133})
        for (int h : {1, 2, 5, 31})
            for (int r : {0, 1, 3, 9}) {
                std::vector<float> src(std::size_t(stride * h)), a(src.size()), b(src.size());
                for (auto& v : src) v = d(rng);
                float inv = 1.f / float(2 * r + 1);
                fuse::ui::simd::boxVertical(src.data(), a.data(), stride, h, r, inv);
                fuse::ui::simd::detail::boxVerticalScalar(src.data(), b.data(), stride, h, r, inv, 0);
                if (std::memcmp(a.data(), b.data(), a.size() * 4)) { ++bad; std::printf("mismatch %d %d %d\n", stride, h, r); }
            }
    // horizontal: compare with the per-channel scalar sliding window over interleaved RGBA rows
    for (int w : {1, 3, 9, 40}) for (int h : {1, 4, 7}) for (int r : {1, 2, 5}) {
        std::vector<float> buf(std::size_t(w * h * 4)), ref, tmp(buf.size()), sc(buf.size());
        for (auto& v : buf) v = d(rng);
        ref = buf;
        std::vector<float> out(ref.size());
        float inv = 1.f / float(2 * r + 1);
        for (int y = 0; y < h; ++y) for (int k = 0; k < 4; ++k) {
            auto at = [&](int i) { i = std::clamp(i, 0, w - 1); return ref[(std::size_t(y) * w + i) * 4 + k]; };
            float acc = 0.f;
            for (int i = -r; i <= r; ++i) acc += at(i);
            for (int i = 0; i < w; ++i) { out[(std::size_t(y) * w + i) * 4 + k] = acc * inv; acc += at(i + r + 1) - at(i - r); }
        }
        fuse::ui::simd::boxHorizontal(buf.data(), tmp.data(), sc.data(), w, h, r, inv);
        if (std::memcmp(buf.data(), out.data(), out.size() * 4)) { ++bad; std::printf("h mismatch %d %d %d\n", w, h, r); }
    }
    for (int n : {0, 1, 7, 8, 9, 33}) {
        std::vector<unsigned> v(std::size_t(n) + 2, 0u);
        fuse::ui::simd::fillSpan(v.data() + 1, n, 0xFF7F80FFu);
        for (int i = 0; i < n + 2; ++i) if (v[std::size_t(i)] != ((i >= 1 && i <= n) ? 0xFF7F80FFu : 0u)) { ++bad; break; }
    }
    std::printf("box blur target=%s mismatches=%d\n", fuse::ui::simd::boxTarget(), bad);
    return bad ? 1 : 0;
}
