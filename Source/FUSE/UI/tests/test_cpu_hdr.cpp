// fuse_ui_cpu_hdr: the float CPU raster (CpuRasterizer::render into HdrImage). SDR content quantised matches the
// 8-bit raster within a code or two (no per-blend rounding); color::hdr colours and Image::hdrPixels keep values above
// SDR white; Rgba16F stores binary16 values; scissored renders leave other pixels alone.
#include "hdr_scene.hpp"
#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace C = fuse::ui::color;

namespace {
void compare8(const char* what, const Image& ref, const HdrImage& f, int maxAllowed, double fracOver1) {
    Image q;
    f.toRgba8(q);
    int maxErr = 0;
    std::size_t over1 = 0;
    for (std::size_t i = 0; i < ref.pixels.size(); ++i) {
        int m = 0;
        for (int k = 0; k < 4; ++k) m = std::max(m, std::abs(int((q.pixels[i] >> (8 * k)) & 0xFFu) - int((ref.pixels[i] >> (8 * k)) & 0xFFu)));
        maxErr = std::max(maxErr, m);
        over1 += m > 1 ? 1 : 0;
    }
    std::printf("%-10s max err %d, >1 on %zu / %zu\n", what, maxErr, over1, ref.pixels.size());
    UI_CHECK(maxErr <= maxAllowed);
    UI_CHECK(double(over1) <= fracOver1 * double(ref.pixels.size()));
}
} // namespace

int main() {
    // 1. widgets + text (SDR): float vs 8-bit
    {
        UiContext ui({.viewport = {488, 400}, .font = &font()});
        widgets::install(ui);
        buildSample(ui);
        ui.update();
        Image ref;
        render(ui, ref);
        HdrImage f;
        f.resize(488, 400);
        CpuRasterizer r;
        r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        r.render(ui.drawList(), f);
        compare8("sample", ref, f, 3, 0.005);
    }
    // 2. every op, SDR part: the scene with HDR colours clipped by the 8-bit path; compare where nothing exceeds 1
    Image im;
    buildHdrImage(im);
    {
        CpuRasterizer r;
        const u32 id = r.addImage(&im);
        Image im8 = im;
        im8.hdrPixels.clear();
        const u32 id8 = r.addImage(&im8);
        DrawList dl;
        buildHdrScene(dl, id);
        Image ref;
        ref.resize(488, 400, 0xFF000000u);
        r.render(dl, ref);
        HdrImage f;
        f.resize(488, 400);
        r.render(dl, f);
        {
            DrawList sdr;
            buildHdrScene(sdr, id8, false);
            Image ref8;
            ref8.resize(488, 400, 0xFF000000u);
            r.render(sdr, ref8);
            HdrImage fs;
            fs.resize(488, 400);
            r.render(sdr, fs);
            compare8("scene-sdr", ref8, fs, 3, 0.01);
        }
        std::size_t bright = 0;
        for (const PixelF& p : f.pixels) bright += p.r > 1.f || p.g > 1.f || p.b > 1.f ? 1 : 0;
        std::printf("scene      %zu px above SDR white\n", bright);
        UI_CHECK(bright > 10000);
        const f32 w4 = C::hdr(Color{1, 1, 1, 1}, 4.f).r;
        std::printf("hdr        4x white %.4f (want %.4f), 50%% %.4f, image 2.5x %.4f\n", double(f.at(250, 320).r), double(w4),
                    double(f.at(305, 320).r), double(f.at(459, 369).r));
        UI_CHECK(std::abs(f.at(250, 320).r - w4) < 1e-4f);
        UI_CHECK(std::abs(f.at(305, 320).r - w4 * 0.5f) < 1e-4f); // over black (stripe gap)
        UI_CHECK(f.at(459, 369).r > 1.4f);                          // image texel from hdrPixels (r 1.0 at 2.5x)
        UI_CHECK(f.at(100, 200).r > 1.f);                          // HDR rect inside the rounded clip
        UI_CHECK(f.at(21, 171).r < 0.5f);                          // corner outside the rounded clip
        // 3. Rgba16F: everything stored is a binary16 value, within half precision of Rgba32F
        HdrImage h;
        h.format = PixelFormat::Rgba16F;
        h.resize(488, 400);
        r.render(dl, h);
        f32 worst = 0.f;
        bool exact = true;
        for (std::size_t i = 0; i < h.pixels.size(); ++i) {
            const f32* a = &h.pixels[i].r;
            const f32* b = &f.pixels[i].r;
            for (int k = 0; k < 4; ++k) {
                exact = exact && C::halfToFloat(C::floatToHalf(a[k])) == a[k];
                worst = std::max(worst, std::abs(a[k] - b[k]) / std::max(std::abs(b[k]), 1e-2f));
            }
        }
        std::printf("rgba16f    worst relative diff vs 32F %.5f\n", double(worst));
        UI_CHECK(exact);
        UI_CHECK(worst < 2e-3f);
        std::vector<u64> row(488);
        h.packHalfRow(320, 0, 488, row.data());
        UI_CHECK_EQ(u16(row[250]), C::floatToHalf(w4));
        // 4. scissor: a damage repaint over a cleared box equals the same box of the full frame
        HdrImage d = f;
        for (int y = 160; y < 290; ++y)
            for (int x = 230; x < 480; ++x) d.pixels[std::size_t(y) * 488 + std::size_t(x)] = {0, 0, 0, 1};
        d.pixels[0] = {9.f, 9.f, 9.f, 1.f}; // outside: must stay
        r.render(dl, d, {230, 160, 250, 130}); // (not cutting the rounded clip, whose shape follows the scissor)
        bool same = true;
        for (int y = 160; y < 290; ++y)
            for (int x = 230; x < 480; ++x) {
                const PixelF& a = d.at(x, y);
                const PixelF& b = f.at(x, y);
                same = same && a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
            }
        UI_CHECK(same);
        UI_CHECK(d.pixels[0].r == 9.f);
    }
    // 5. binary16 helpers: exact round trip of every half, quantizeHalf == the round trip over a float sweep
    {
        bool rt = true, q = true;
        for (u32 h = 0; h < 65536; ++h) {
            const u16 b = u16(h);
            if (((b >> 10) & 31u) == 31u && (b & 1023u)) continue; // NaN
            rt = rt && C::floatToHalf(C::halfToFloat(b)) == b;
        }
        for (u32 i = 0; i < 2000000; ++i) {
            u32 x = i * 2147u + (i >> 3);
            f32 f;
            std::memcpy(&f, &x, 4);
            if (!(f == f)) continue;
            const f32 a = C::quantizeHalf(f), b = C::halfToFloat(C::floatToHalf(f));
            q = q && std::memcmp(&a, &b, 4) == 0;
        }
        UI_CHECK(rt);
        UI_CHECK(q);
    }
    return finish("fuse_ui_cpu_hdr");
}
