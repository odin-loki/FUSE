// UI3 colour glyph and MSDF gates (one binary per gate, FUSE_TEXT_GATE picks it):
//   bitmap_emoji  data/bitmap_cbdt_test.ttf and data/bitmap_sbix_test.ttf (make_bitmap_emoji_test_font.py): CBDT / sbix
//                 strikes at 16 / 32 / 64 ppem in red / green / blue. Strike choice per size, placement (bearings,
//                 sbix origin, 'dupe'), premultiplication, scaling, the RGBA atlas page and the UI path.
//   msdf          MSDF glyphs (GlyphAtlas::getMsdf, kGlyphMsdf draw commands) against coverage rasterised at the
//                 target size (64 / 128 / 256 px: mean error and artifact fraction), UiContext::setMsdfThreshold (with
//                 DPI scale) and GPU parity of colour + MSDF glyphs: the compute kernel bit-exact, the SDF-quad
//                 backend within its blend tolerance (skipped without a Vulkan device).
// Optional --out=<dir> writes the rendered images.
#include "test_util.hpp"

#if FUSE_TEXT_COLOR_HAVE_VK
#include "fuse/ui/render/ui_vk.hpp"
#include "vk_headless.hpp"
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

using namespace fuse::ui;
using namespace fuse::text;
namespace w = fuse::ui::widgets;
using fuse::ui::test::finish;

namespace {
std::string g_out;

std::string utf8(std::u32string_view cps) {
    std::string s;
    for (char32_t c : cps) appendUtf8(s, c);
    return s;
}
void writePng(const std::string& name, const Image& img) {
    if (g_out.empty()) return;
    stbi_write_png((g_out + "/" + name).c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
}
const std::uint8_t* px4(const ColorImage& img, int x, int y) { return img.rgba.data() + (std::size_t(y) * std::size_t(img.w) + std::size_t(x)) * 4; }

// ---- fuse_text_bitmap_emoji ----------------------------------------------------------------------------------------
int bitmapEmoji() {
    enableColorGlyphs(); // before loading: the strike index is read at load
    UI_CHECK(colorGlyphsEnabled());
    for (const char* name : {"bitmap_cbdt_test", "bitmap_sbix_test"}) {
        const bool sbix = std::strstr(name, "sbix") != nullptr;
        const std::string file = fuse::ui::test::testFontPath(name);
        auto loaded = Font::fromFile(file);
        UI_CHECK(loaded.has_value());
        if (!loaded) continue;
        const Font& f = *loaded;
        std::printf("%s\n", file.c_str());
        UI_CHECK(f.hasColorGlyphs());
        const std::uint32_t e100 = f.glyphIndex(U''), e101 = f.glyphIndex(U''), a = f.glyphIndex(U'A');
        const auto layers = f.colorLayers(e100);
        UI_CHECK(layers.size() == 1 && layers[0].image && layers[0].glyph == e100);
        UI_CHECK(f.colorLayers(a).empty());
        ColorImage img;
        UI_CHECK(!f.renderColor(a, 32.f, 0xFF000000u, img)); // outline glyph: no image
        // pixel size == ppem here (ascent - descent == unitsPerEm)
        struct Pick { float ppem; int strike; std::uint8_t r, g, b; };
        const Pick picks[] = {{8, 16, 255, 0, 0},  {12, 16, 255, 0, 0}, {16, 16, 255, 0, 0}, {20, 32, 0, 255, 0},
                              {32, 32, 0, 255, 0}, {40, 64, 0, 0, 255}, {64, 64, 0, 0, 255}, {100, 64, 0, 0, 255}};
        for (const Pick& p : picks) {
            UI_CHECK(f.renderColor(e100, p.ppem, 0xFF000000u, img));
            if (img.w <= 0) continue;
            const int P = int(p.ppem);
            // placed at the origin, top = 0.8 em above the baseline, about one em square
            UI_CHECK(img.x0 == 0);
            UI_CHECK(std::abs(img.y0 + int(std::lround(0.8f * p.ppem))) <= 1);
            UI_CHECK(std::abs(img.w - P) <= 1 && std::abs(img.h - P) <= 1);
            // inner opaque texel in the strike's colour; the semi-transparent quadrant premultiplied
            const std::uint8_t* in = px4(img, img.w / 4, img.h / 4);
            const std::uint8_t* q = px4(img, img.w * 3 / 4, img.h * 3 / 4);
            std::printf("  ppem %5.1f: %dx%d at (%d, %d), inner (%d %d %d %d), quadrant (%d %d %d %d)\n", double(p.ppem), img.w,
                        img.h, img.x0, img.y0, in[0], in[1], in[2], in[3], q[0], q[1], q[2], q[3]);
            UI_CHECK(in[0] == p.r && in[1] == p.g && in[2] == p.b && in[3] == 255);
            UI_CHECK(std::abs(int(q[3]) - 128) <= 2);
            UI_CHECK(std::abs(int(q[0]) - int(p.r) / 2) <= 2 && std::abs(int(q[2]) - int(p.b) / 2) <= 2);
            for (std::size_t k = 0; k < img.rgba.size(); k += 4) // premultiplied
                UI_CHECK(img.rgba[k] <= img.rgba[k + 3] && img.rgba[k + 1] <= img.rgba[k + 3] && img.rgba[k + 2] <= img.rgba[k + 3]);
            (void)p.strike;
        }
        // native strike: exact (1 px transparent border, opaque inside)
        UI_CHECK(f.renderColor(e100, 32.f, 0xFF000000u, img));
        UI_CHECK(img.w == 32 && img.h == 32 && img.y0 == -26);
        if (img.w == 32) {
            UI_CHECK(px4(img, 0, 0)[3] == 0 && px4(img, 31, 31)[3] == 0);
            UI_CHECK(px4(img, 1, 1)[1] == 255 && px4(img, 1, 1)[3] == 255);
            UI_CHECK(px4(img, 20, 20)[1] == 128 && px4(img, 20, 20)[3] == 128);
        }
        // U+E101: CBDT a half-width image at bearing P/4; sbix a 'dupe' of U+E100 (same bytes)
        ColorImage img101;
        UI_CHECK(f.renderColor(e101, 32.f, 0xFF000000u, img101));
        if (sbix) UI_CHECK(img101.w == img.w && img101.h == img.h && img101.x0 == img.x0 && img101.rgba == img.rgba);
        else UI_CHECK(img101.w == 16 && img101.h == 32 && img101.x0 == 8 && img101.y0 == -26);
        // the atlas colour page holds the same texels
        GlyphAtlas atlas(256);
        const AtlasGlyph ag = atlas.getColor(f, e100, 32.f);
        UI_CHECK(ag.valid && ag.kind == AtlasKind::Color && ag.w == 32 && ag.h == 32 && ag.y0 == -26);
        bool same = true;
        for (int y = 0; y < ag.h; ++y)
            same &= std::memcmp(atlas.colorPixels() + (std::size_t(ag.v + y) * std::size_t(atlas.colorSize()) + ag.u) * 4,
                                img.rgba.data() + std::size_t(y) * std::size_t(img.w) * 4, std::size_t(ag.w) * 4) == 0;
        UI_CHECK(same);
        const std::uint32_t misses = atlas.misses();
        UI_CHECK(atlas.getColor(f, e100, 32.f, 0xFF00FF00u).u == ag.u && atlas.misses() == misses); // bitmaps ignore the text colour
        // through the UI: a label of both glyphs at 20 px (green strike, downscaled) at half opacity
        UiContext ui{{.viewport = {120, 40}, .font = &f}};
        const NodeHandle l = w::label(ui, ui.root(), utf8(U""));
        ui.setCss(l, "font-size", "20px");
        ui.setCss(l, "color", "rgba(255, 255, 255, 0.5)");
        ui.update();
        int colorCmds = 0;
        for (const DrawCmd& c : ui.drawList().cmds()) colorCmds += c.op == DrawOp::Glyph && c.gradient == kGlyphColor;
        UI_CHECK(colorCmds == 2);
        Image out;
        fuse::ui::test::render(ui, out);
        int green = 0;
        for (const u32 p : out.pixels) {
            const int G = int((p >> 8) & 0xFF), R = int(p & 0xFF), B = int((p >> 16) & 0xFF);
            green += G >= 120 && G <= 135 && R < 8 && B < 8; // 0.5 opacity over black
        }
        std::printf("  ui: %d colour glyph commands, %d half-green pixels\n", colorCmds, green);
        UI_CHECK(green > 150);
        writePng(sbix ? "bitmap_sbix.png" : "bitmap_cbdt.png", out);
    }
    return finish("fuse_text_bitmap_emoji");
}

// ---- fuse_text_msdf ------------------------------------------------------------------------------------------------
struct Quality {
    double mean = 0.0;   ///< mean |coverage difference| over the glyph boxes
    double badFrac = 0.0; ///< fraction of those pixels off by more than half
};

Quality compareGlyph(const Font& f, GlyphAtlas& atlas, std::uint32_t glyph, float size, Image& a, Image& b) {
    const AtlasGlyph m = atlas.getMsdf(f, glyph);
    const AtlasGlyph c = atlas.get(f, glyph, size);
    const float k = size / GlyphAtlas::kMsdfPixelSize;
    const int W = int(size * 2.f), H = int(size * 1.6f);
    const float penX = std::floor(size * 0.3f), base = std::floor(size * 1.2f);
    a.resize(W, H, 0xFF000000u);
    b.resize(W, H, 0xFF000000u);
    DrawList dm, dc;
    DrawCmd cmd;
    cmd.op = DrawOp::Glyph;
    cmd.fill = {1, 1, 1, 1};
    cmd.gradient = kGlyphMsdf;
    cmd.blur = GlyphAtlas::kMsdfRange;
    cmd.rect = {penX + float(m.x0) * k, base + float(m.y0) * k, float(m.w) * k, float(m.h) * k};
    cmd.u = m.u; cmd.v = m.v; cmd.tw = m.w; cmd.th = m.h;
    dm.push(cmd);
    cmd.gradient = kGlyphCoverage;
    cmd.blur = 0.f;
    cmd.rect = {penX + float(c.x0), base + float(c.y0), float(c.w), float(c.h)};
    cmd.u = c.u; cmd.v = c.v; cmd.tw = c.w; cmd.th = c.h;
    dc.push(cmd);
    CpuRasterizer r;
    r.setGlyphAtlas(atlas.pixels(), atlas.size());
    r.render(dm, a);
    r.render(dc, b);
    Quality q;
    std::size_t n = 0, bad = 0;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        const int va = int(a.pixels[i] & 0xFF), vb = int(b.pixels[i] & 0xFF);
        if (!va && !vb) continue;
        ++n;
        sum += std::abs(va - vb) / 255.0;
        bad += std::abs(va - vb) > 128;
    }
    q.mean = n ? sum / double(n) : 0.0;
    q.badFrac = n ? double(bad) / double(n) : 0.0;
    return q;
}

#if FUSE_TEXT_COLOR_HAVE_VK
using fuse::ui::render::GpuDrawData;
using fuse::ui::render::UiVkRenderer;
using fuse::ui::test::VkHeadless;
int maxErr(const Image& a, const Image& b, double& mean) {
    int m = 0;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i)
        for (int s = 0; s < 32; s += 8) {
            const int e = std::abs(int((a.pixels[i] >> s) & 0xFF) - int((b.pixels[i] >> s) & 0xFF));
            m = std::max(m, e);
            sum += e;
        }
    mean = sum / double(a.pixels.size() * 4);
    return m;
}
#endif

int msdf() {
    enableColorGlyphs();
    enableMsdf();
    UI_CHECK(msdfEnabled());
    const Font& f = fuse::ui::test::font();
    GlyphAtlas atlas(1024);
    // quality: MSDF from 32 px against coverage rasterised at the target size
    Image a, b;
    double worstMean = 0.0, worstBad = 0.0;
    for (const char32_t ch : std::u32string_view(U"AgW@&8eSR%")) {
        const std::uint32_t g = f.glyphIndex(ch);
        const AtlasGlyph m = atlas.getMsdf(f, g);
        UI_CHECK(m.valid && m.kind == AtlasKind::Msdf && m.w > 0);
        for (const float size : {64.f, 128.f, 256.f}) {
            const Quality q = compareGlyph(f, atlas, g, size, a, b);
            worstMean = std::max(worstMean, q.mean);
            worstBad = std::max(worstBad, q.badFrac);
            if (ch == U'@' && size == 256.f) {
                writePng("msdf_at256.png", a);
                writePng("coverage_at256.png", b);
            }
        }
    }
    std::printf("msdf vs coverage (64 / 128 / 256 px): worst mean |error| %.4f, worst fraction off by > 1/2 %.5f\n",
                worstMean, worstBad);
    // measured (MSVC, OpenSans): 0.049 / 0.004 — edge anti-aliasing differences and sub-texel corner fillets only
    UI_CHECK(worstMean < 0.06);
    UI_CHECK(worstBad < 0.006);
    // determinism: the same glyph regenerated in a fresh atlas is byte-identical
    {
        GlyphAtlas a2(1024);
        const std::uint32_t g = f.glyphIndex(U'@');
        const AtlasGlyph m1 = atlas.getMsdf(f, g), m2 = a2.getMsdf(f, g);
        bool same = m1.w == m2.w && m1.h == m2.h;
        for (int y = 0; same && y < m1.h; ++y)
            same = std::memcmp(atlas.colorPixels() + (std::size_t(m1.v + y) * std::size_t(atlas.colorSize()) + m1.u) * 4,
                               a2.colorPixels() + (std::size_t(m2.v + y) * std::size_t(a2.colorSize()) + m2.u) * 4,
                               std::size_t(m1.w) * 4) == 0;
        UI_CHECK(same);
    }

    // UiContext: text at or above the threshold (device pixels) is drawn from MSDFs, smaller text from coverage
    auto colr = Font::fromFile(fuse::ui::test::testFontPath("colrv1_test"));
    UI_CHECK(colr.has_value());
    Font font = std::move(*Font::bundled());
    std::vector<const Font*> chain;
    if (colr) chain.push_back(&*colr);
    font.setFallbacks(chain);
    UiContext ui{{.viewport = {488, 300}, .font = &font}};
    ui.setMsdfThreshold(40.f);
    UI_CHECK(ui.msdfThreshold() == 40.f);
    const NodeHandle col = w::panel(ui, ui.root());
    ui.setCss(col, "flex-direction", "column");
    const NodeHandle big = w::label(ui, col, utf8(U"Big MSDF "));
    ui.setCss(big, "font-size", "56px");
    ui.setCss(big, "color", "#ffcc33");
    const NodeHandle smallText = w::label(ui, col, "small coverage text");
    ui.setCss(smallText, "font-size", "14px");
    const NodeHandle mid = w::label(ui, col, utf8(U"Scaled 30px "));
    ui.setCss(mid, "font-size", "30px");
    ui.update();
    auto count = [&](u8 mode) {
        int n = 0;
        for (const DrawCmd& c : ui.drawList().cmds()) n += c.op == DrawOp::Glyph && c.gradient == mode;
        return n;
    };
    const int msdf1 = count(kGlyphMsdf), cov1 = count(kGlyphCoverage), color1 = count(kGlyphColor);
    std::printf("ui scale 1: %d MSDF, %d coverage, %d colour glyph commands\n", msdf1, cov1, color1);
    UI_CHECK(msdf1 == 7 && color1 == 4 && cov1 >= 25);
    Image ui1;
    fuse::ui::test::render(ui, ui1);
    writePng("msdf_ui.png", ui1);
    // DPI scale 1.5: the 30 px line (45 device px) crosses the threshold
    ui.setRenderScale(1.5f);
    ui.update();
    const int msdf2 = count(kGlyphMsdf);
    std::printf("ui scale 1.5: %d MSDF glyph commands\n", msdf2);
    UI_CHECK(msdf2 == 7 + 10);
    // scaled MSDF quads keep the texel range and stretch their rect
    for (const DrawCmd& c : ui.drawList().cmds())
        if (c.op == DrawOp::Glyph && c.gradient == kGlyphMsdf) UI_CHECK(c.blur == GlyphAtlas::kMsdfRange && c.rect.w > float(c.tw));
    ui.setRenderScale(1.f);
    ui.update();

#if FUSE_TEXT_COLOR_HAVE_VK
    // GPU parity: the UI scene plus loose MSDF quads at odd scales and offsets, under a rounded clip
    if (!UiVkRenderer::kernelAvailable()) {
        std::printf("GPU parity: kernel not compiled in, skipped\n");
        return finish("fuse_text_msdf");
    }
    constexpr u32 W = 488, H = 300;
    VkHeadless vk;
    if (!vk.init(W, H)) {
        std::printf("GPU parity: no Vulkan device, skipped\n");
        return finish("fuse_text_msdf");
    }
    DrawList dl = ui.drawList();
    dl.pushClip({200, 150, 280, 140}, 30.f);
    const std::uint32_t at = font.glyphIndex(U'&');
    for (int k = 0; k < 6; ++k) {
        const AtlasGlyph m = ui.atlas().getMsdf(font, at);
        const float s = 0.9f + 0.73f * float(k), px0 = 205.3f + 41.7f * float(k), py0 = 160.6f + 7.1f * float(k);
        DrawCmd c;
        c.op = DrawOp::Glyph;
        c.gradient = kGlyphMsdf;
        c.blur = GlyphAtlas::kMsdfRange;
        c.fill = Color::rgb(0x40c0ff, 0.85f);
        c.rect = {px0, py0, float(m.w) * s, float(m.h) * s};
        c.u = m.u; c.v = m.v; c.tw = m.w; c.th = m.h;
        dl.push(c);
    }
    dl.popClip();
    Image ref, init, outC, outS;
    init.resize(int(W), int(H), 0xFF202020u);
    ref = init;
    CpuRasterizer cpu;
    cpu.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    cpu.render(dl, ref);
    writePng("msdf_gpu_ref.png", ref);
    UiVkRenderer comp, sdf;
    UI_CHECK(comp.init({.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 1}));
    GpuDrawData data;
    packDrawList(dl, W, H, data, ui.atlas().pixels(), u32(ui.atlas().size()));
    UI_CHECK(data.atlasColor);
    if (comp.valid()) {
        UI_CHECK(comp.upload(0, data));
        vk.run(init, outC, [&](VkCommandBuffer cmd) { comp.record((void*)cmd, (void*)vk.view, 0); });
        double mean = 0.0;
        const int e = maxErr(ref, outC, mean);
        std::printf("GPU compute kernel (%s): max channel error %d, mean %.5f\n", vk.deviceName, e, mean);
        UI_CHECK_EQ(e, 0);
    }
    if (UiVkRenderer::sdfQuadsAvailable()) {
        fuse::ui::render::UiVkDesc d{.physicalDevice = (void*)vk.phys, .device = (void*)vk.device, .framesInFlight = 1};
        UI_CHECK(fuse::ui::render::useSdfQuads(d));
        UI_CHECK(sdf.init(d));
        if (sdf.valid()) {
            UI_CHECK(sdf.upload(0, data));
            vk.run(init, outS, [&](VkCommandBuffer cmd) { sdf.record((void*)cmd, (void*)vk.view, 0); });
            double mean = 0.0;
            const int e = maxErr(ref, outS, mean);
            std::printf("GPU SDF quads: max channel error %d, mean %.5f\n", e, mean);
            UI_CHECK(e <= 2 && mean <= 0.1); // the SDF-quad blend tolerance (fuse_ui_vk_sdf_quads)
        }
    }
#else
    std::printf("GPU parity: fuse_ui_vk not built, skipped\n");
#endif
    return finish("fuse_text_msdf");
}
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--out=", 6) == 0) g_out = argv[i] + 6;
    const std::string gate = FUSE_TEXT_GATE;
    if (gate == "bitmap_emoji") return bitmapEmoji();
    if (gate == "msdf") return msdf();
    std::fprintf(stderr, "unknown gate %s\n", gate.c_str());
    return 2;
}
