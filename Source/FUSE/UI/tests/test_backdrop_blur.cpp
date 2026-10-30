// fuse_ui_backdrop_blur (plan UI4): DrawOp::Backdrop / `backdrop-blur` (CSS backdrop-filter: blur()).
//   * pixels under the rounded rect are blurred: a hard stripe edge becomes a smooth ramp, its mean colour is kept
//   * pixels outside the rect (and in its rounded corners) are untouched; a tint is blended over the blur
//   * the style property reaches the draw list (Backdrop emitted under the node's background)
// Optional --out=<png>.
#include "test_util.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdio>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
int ch(u32 p, int k) { return int((p >> (8 * k)) & 0xFFu); }
void scene(DrawList& dl) {
    for (int i = 0; i < 16; ++i) // 20 px vertical stripes, red / blue
        dl.rect({f32(i * 20), 0.f, 20.f, 200.f}, i % 2 ? Color::rgb(0x2050E0) : Color::rgb(0xE04020));
    for (int i = 0; i < 8; ++i) dl.rect({10.f + f32(i) * 40.f, 150.f, 18.f, 18.f}, Color::rgb(0xF0F0F0), 9.f);
}
} // namespace

int main(int argc, char** argv) {
    const Rect card{60.f, 40.f, 200.f, 120.f};
    Image ref, img;
    {
        DrawList dl;
        scene(dl);
        ref.resize(320, 200, 0xFF000000u);
        CpuRasterizer().render(dl, ref);
        dl.clear();
        scene(dl);
        dl.backdrop(card, 12.f, {16.f, 16.f, 16.f, 16.f});
        img.resize(320, 200, 0xFF000000u);
        CpuRasterizer().render(dl, img);
    }
    // outside: identical; rounded corner pixel: identical
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 320; ++x)
            if (!card.contains({f32(x) + 0.5f, f32(y) + 0.5f})) UI_CHECK(img.at(x, y) == ref.at(x, y));
    UI_CHECK(img.at(61, 41) == ref.at(61, 41));
    // inside: across the red|blue edge at x = 140 the reference jumps, the blur ramps
    {
        int maxStep = 0, refStep = 0;
        for (int x = 120; x < 160; ++x) {
            maxStep = std::max(maxStep, std::abs(ch(img.at(x + 1, 100), 0) - ch(img.at(x, 100), 0)));
            refStep = std::max(refStep, std::abs(ch(ref.at(x + 1, 100), 0) - ch(ref.at(x, 100), 0)));
        }
        std::printf("  stripe edge: max step %d (unblurred %d)\n", maxStep, refStep);
        UI_CHECK(refStep > 150 && maxStep < 40);
        long sumA = 0, sumB = 0; // mean over two full stripe periods is preserved
        for (int x = 100; x < 180; ++x) { sumA += ch(img.at(x, 100), 0); sumB += ch(ref.at(x, 100), 0); }
        UI_CHECK(std::abs(sumA - sumB) < 80 * 6);
    }
    // tint over the blur
    {
        DrawList dl;
        scene(dl);
        dl.backdrop(card, 12.f, {16.f, 16.f, 16.f, 16.f}, Color::rgb(0xFFFFFF, 0.25f));
        Image t;
        t.resize(320, 200, 0xFF000000u);
        CpuRasterizer().render(dl, t);
        UI_CHECK(ch(t.at(140, 100), 1) > ch(img.at(140, 100), 1) + 30);
        const std::string out = argc > 1 && std::string(argv[1]).rfind("--out=", 0) == 0 ? std::string(argv[1]).substr(6) : "";
        if (!out.empty()) {
            Image both;
            both.resize(640, 200, 0xFF000000u);
            for (int y = 0; y < 200; ++y)
                for (int x = 0; x < 320; ++x) { both.pixels[std::size_t(y) * 640 + x] = ref.at(x, y); both.pixels[std::size_t(y) * 640 + 320 + x] = t.at(x, y); }
            stbi_write_png(out.c_str(), 640, 200, 4, both.pixels.data(), 640 * 4);
        }
    }
    // style plumbing: backdrop-blur on a class emits a Backdrop under the background
    {
        UiContext ui({.viewport = {320, 200}, .font = &font()});
        w::install(ui);
        auto sheet = StyleSheet::parse(".glass { backdrop-blur: 12px; background: rgba(255,255,255,0.2); }");
        UI_CHECK(sheet.has_value());
        if (sheet) ui.styles().addSheet(std::move(*sheet));
        const NodeHandle n = w::label(ui, ui.root(), "Frosted");
        ui.addClass(n, "glass");
        ui.update();
        UI_CHECK(ui.computed(n).backdropBlur == 12.f);
        int bi = -1, bg = -1;
        const auto& cmds = ui.drawList().cmds();
        for (int i = 0; i < int(cmds.size()); ++i) {
            if (cmds[std::size_t(i)].op == DrawOp::Backdrop && bi < 0) bi = i;
            if (cmds[std::size_t(i)].op == DrawOp::Rect && cmds[std::size_t(i)].node == n.index && bg < 0) bg = i;
        }
        UI_CHECK(bi >= 0 && bg > bi);
    }
    // clip paths: a backdrop under a circular clip-path blurs only inside the circle (soft edge), not the card corners
    {
        DrawList dl;
        scene(dl);
        Path circ;
        circ.circle({160.f, 100.f}, 50.f);
        dl.pushClipPath(circ);
        dl.backdrop(card, 12.f, {16.f, 16.f, 16.f, 16.f});
        dl.popClip();
        Image c;
        c.resize(320, 200, 0xFF000000u);
        CpuRasterizer().render(dl, c);
        UI_CHECK(c.at(70, 60) == ref.at(70, 60));     // in the card, outside the circle: untouched
        UI_CHECK(c.at(140, 100) == img.at(140, 100)); // inside the circle: same blur as unclipped
        UI_CHECK(c.at(140, 100) != ref.at(140, 100));
    }
    // background-clip: padding-box / content-box move the background and backdrop inside the border (and padding)
    {
        UiContext ui({.viewport = {320, 200}, .font = &font()});
        w::install(ui);
        auto sheet = StyleSheet::parse(".a { backdrop-blur: 8px; background: #ff0000; border-width: 4px; border-color: #00ff00;"
                                       " padding-left: 10px; padding-top: 6px; border-radius: 12px; width: 120px; height: 60px; }"
                                       ".p { background-clip: padding-box; } .c { background-clip: content-box; }");
        UI_CHECK(sheet.has_value());
        if (sheet) ui.styles().addSheet(std::move(*sheet));
        const NodeHandle a = w::label(ui, ui.root(), "");
        const NodeHandle b = w::label(ui, ui.root(), "");
        const NodeHandle d = w::label(ui, ui.root(), "");
        for (NodeHandle h : {a, b, d}) ui.addClass(h, "a");
        ui.addClass(b, "p");
        ui.addClass(d, "c");
        ui.update();
        UI_CHECK(ui.computed(a).backgroundClip == BackgroundClip::BorderBox);
        UI_CHECK(ui.computed(b).backgroundClip == BackgroundClip::PaddingBox);
        UI_CHECK(ui.computed(d).backgroundClip == BackgroundClip::ContentBox);
        auto find = [&](NodeHandle h, DrawOp op, bool fill) -> const DrawCmd* {
            for (const DrawCmd& c : ui.drawList().cmds())
                if (c.node == h.index && c.op == op && (!fill || c.fill.a > 0.f)) return &c;
            return nullptr;
        };
        const Rect ra = ui.rect(a), rb = ui.rect(b), rd = ui.rect(d);
        const DrawCmd* ba = find(a, DrawOp::Backdrop, false);
        const DrawCmd* bb = find(b, DrawOp::Backdrop, false);
        const DrawCmd* bd = find(d, DrawOp::Backdrop, false);
        UI_CHECK(ba && bb && bd);
        if (ba && bb && bd) {
            UI_CHECK(ba->rect == ra);
            UI_CHECK(bb->rect.x == rb.x + 4.f && bb->rect.y == rb.y + 4.f && bb->rect.w == rb.w - 8.f && bb->radius[0] == 8.f);
            UI_CHECK(bd->rect.x == rd.x + 14.f && bd->rect.y == rd.y + 10.f && bd->radius[0] == 0.f);
        }
        const DrawCmd* fb = find(b, DrawOp::Rect, true);
        UI_CHECK(fb && fb->rect.x == rb.x + 4.f && fb->borderWidth == 0.f);
        bool border = false; // the border still strokes the border box
        for (const DrawCmd& c : ui.drawList().cmds())
            if (c.node == b.index && c.op == DrawOp::Rect && c.borderWidth == 4.f && c.rect == rb) border = true;
        UI_CHECK(border);
    }
    // ColorMatrix: CSS function values on known colours
    {
        auto run = [](const ColorMatrix& m, f32 r, f32 g, f32 b, f32 a = 1.f) {
            const f32 in[4] = {r, g, b, a};
            std::array<f32, 4> o{};
            m.apply(in, o.data());
            return o;
        };
        auto near = [](f32 a, f32 b) { return std::abs(a - b) < 2e-3f; };
        auto o = run(ColorMatrix::invert(1.f), 1, 0, 0);
        UI_CHECK(near(o[0], 0) && near(o[1], 1) && near(o[2], 1));
        o = run(ColorMatrix::grayscale(1.f), 1, 0, 0);
        UI_CHECK(near(o[0], 0.2126f) && near(o[1], 0.2126f) && near(o[2], 0.2126f));
        o = run(ColorMatrix::brightness(0.5f), 0.8f, 0.4f, 0.2f);
        UI_CHECK(near(o[0], 0.4f) && near(o[1], 0.2f) && near(o[2], 0.1f));
        o = run(ColorMatrix::contrast(0.f), 0.9f, 0.1f, 0.3f);
        UI_CHECK(near(o[0], 0.5f) && near(o[1], 0.5f) && near(o[2], 0.5f));
        o = run(ColorMatrix::opacity(0.25f), 1, 1, 1);
        UI_CHECK(near(o[3], 0.25f) && near(o[0], 1.f));
        o = run(ColorMatrix::hueRotate(360.f), 0.3f, 0.6f, 0.9f);
        UI_CHECK(near(o[0], 0.3f) && near(o[1], 0.6f) && near(o[2], 0.9f));
        o = run(ColorMatrix::saturate(1.f), 0.3f, 0.6f, 0.9f);
        UI_CHECK(near(o[0], 0.3f) && near(o[1], 0.6f) && near(o[2], 0.9f));
        o = run(ColorMatrix::sepia(1.f), 1, 1, 1);
        UI_CHECK(near(o[0], 1.f) && near(o[1], 1.f) && o[2] < 0.95f);
        // then(): brightness(0.5) then invert(1) = 1 - 0.5 x
        o = run(ColorMatrix::brightness(0.5f).then(ColorMatrix::invert(1.f)), 0.8f, 0.4f, 0.f);
        UI_CHECK(near(o[0], 0.6f) && near(o[1], 0.8f) && near(o[2], 1.f));
        UI_CHECK(ColorMatrix{}.identity() && !ColorMatrix::invert(1.f).identity());
        // filter-only backdrop (blur 0): an inverted panel, edges untouched, no blur
        DrawList dl;
        scene(dl);
        dl.backdrop(card, 0.f, {0, 0, 0, 0}, ColorMatrix::invert(1.f));
        Image f;
        f.resize(320, 200, 0xFF000000u);
        CpuRasterizer().render(dl, f);
        for (int x : {70, 139, 140, 200})
            for (int k = 0; k < 3; ++k) UI_CHECK(std::abs(ch(f.at(x, 100), k) - (255 - ch(ref.at(x, 100), k))) <= 1);
        UI_CHECK(f.at(30, 100) == ref.at(30, 100));
    }
    // CSS: backdrop-filter parses blur + colour functions in order; background-clip: text clips to the glyphs
    {
        UiContext ui({.viewport = {320, 200}, .font = &font()});
        w::install(ui);
        auto sheet = StyleSheet::parse(".f { backdrop-filter: blur(6px) brightness(120%) hue-rotate(0.5turn) invert(1); background: rgba(0,0,0,0); }"
                                       ".t { background-clip: text; background: #ff00ff; color: rgba(0,0,0,0); font-size: 40px; }");
        UI_CHECK(sheet.has_value());
        if (sheet) ui.styles().addSheet(std::move(*sheet));
        const NodeHandle a = w::label(ui, ui.root(), "x");
        const NodeHandle t = w::label(ui, ui.root(), "HI");
        ui.addClass(a, "f");
        ui.addClass(t, "t");
        ui.update();
        const ComputedStyle& cs = ui.computed(a);
        UI_CHECK(cs.backdropBlur == 6.f && cs.backdropFilterCount == 3);
        UI_CHECK(cs.backdropFilterFns[0] == FilterFn::Brightness && std::abs(cs.backdropFilterAmounts[0] - 1.2f) < 1e-5f);
        UI_CHECK(cs.backdropFilterFns[1] == FilterFn::HueRotate && std::abs(cs.backdropFilterAmounts[1] - 180.f) < 1e-3f);
        UI_CHECK(cs.backdropFilterFns[2] == FilterFn::Invert);
        const DrawCmd* bd = nullptr;
        for (const DrawCmd& c : ui.drawList().cmds())
            if (c.node == a.index && c.op == DrawOp::Backdrop) bd = &c;
        UI_CHECK(bd && bd->stopCount == 4 && bd->blur == 6.f);
        UI_CHECK(ui.computed(t).backgroundClip == BackgroundClip::Text);
        bool clipFirst = false, fill = false;
        for (const DrawCmd& c : ui.drawList().cmds()) {
            if (c.node != t.index && !(c.op == DrawOp::ClipPath)) continue;
            if (c.op == DrawOp::ClipPath && !fill) clipFirst = true;
            if (c.node == t.index && c.op == DrawOp::Rect && c.fill.a > 0.f) fill = true;
        }
        UI_CHECK(clipFirst && fill);
        Image img2;
        img2.resize(320, 200, 0xFF000000u);
        CpuRasterizer r2;
        r2.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        r2.render(ui.drawList(), img2);
        const Rect tr = ui.rect(t);
        int magenta = 0, total = 0;
        for (int y = int(tr.y); y < int(tr.bottom()); ++y)
            for (int x = int(tr.x); x < int(tr.right()); ++x) {
                ++total;
                const u32 p = img2.at(x, y);
                magenta += ch(p, 0) > 200 && ch(p, 2) > 200 && ch(p, 1) < 60;
            }
        std::printf("  background-clip:text: %d of %d px magenta (rect %g,%g %gx%g)\n", magenta, total, tr.x, tr.y, tr.w, tr.h);
        if (std::getenv("FUSE_DUMP_CLIPTEXT")) stbi_write_png(std::getenv("FUSE_DUMP_CLIPTEXT"), 320, 200, 4, img2.pixels.data(), 320 * 4);
        UI_CHECK(magenta > 40 && magenta < total / 2); // the letters only, not the box
    }
    return finish("fuse_ui_backdrop_blur");
}
