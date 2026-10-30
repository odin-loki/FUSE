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
    std::printf("box blur target=%s mismatches=%d\n", fuse::ui::simd::boxTarget(), bad);
    return bad ? 1 : 0;
}
