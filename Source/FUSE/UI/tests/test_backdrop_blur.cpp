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
        UI_CHECK(cs.backdropBlur == 0.f && cs.backdropFilterCount == 4); // blur() stays an ordered entry
        UI_CHECK(cs.backdropFilterFns[0] == FilterFn::Blur && cs.backdropFilterAmounts[0] == 6.f);
        UI_CHECK(cs.backdropFilterFns[1] == FilterFn::Brightness && std::abs(cs.backdropFilterAmounts[1] - 1.2f) < 1e-5f);
        UI_CHECK(cs.backdropFilterFns[2] == FilterFn::HueRotate && std::abs(cs.backdropFilterAmounts[2] - 180.f) < 1e-3f);
        UI_CHECK(cs.backdropFilterFns[3] == FilterFn::Invert);
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
    // element filter layer: isolation (an opacity filter over two overlapping opaque rects shows no seam between them),
    // matrix applies to the subtree only, drop-shadow lands at the offset
    {
        DrawList dl;
        dl.rect({0, 0, 320, 200}, Color::rgb(0x000000));
        dl.pushFilter(ColorMatrix::opacity(0.5f));
        dl.rect({20, 20, 60, 40}, Color::rgb(0xffffff));
        dl.rect({50, 20, 60, 40}, Color::rgb(0xffffff)); // overlaps the first: a layer keeps it one shape
        dl.popFilter();
        dl.pushFilter(ColorMatrix{}, 0.f, DropShadow{{10, 10}, 0.f, Color::rgb(0xff0000)});
        dl.rect({150, 20, 40, 40}, Color::rgb(0x00ff00));
        dl.popFilter();
        Image f;
        f.resize(320, 200, 0xFF000000u);
        CpuRasterizer().render(dl, f);
        const int a = ch(f.at(30, 40), 0), b = ch(f.at(65, 40), 0), c = ch(f.at(100, 40), 0);
        std::printf("  filter layer: opacity %d %d %d\n", a, b, c);
        UI_CHECK(std::abs(a - 128) <= 2 && std::abs(b - a) <= 1 && std::abs(c - a) <= 1);
        UI_CHECK(ch(f.at(170, 40), 1) > 250 && ch(f.at(170, 40), 0) < 5);     // element over its shadow
        UI_CHECK(ch(f.at(195, 65), 0) > 250 && ch(f.at(195, 65), 1) < 5);     // shadow at +10,+10
        UI_CHECK(ch(f.at(145, 15), 0) < 5 && ch(f.at(205, 75), 0) < 5);       // nothing beyond it
    }
    // CSS filter / drop-shadow / url() parse and reach the draw list
    {
        UiContext ui({.viewport = {320, 200}, .font = &font()});
        w::install(ui);
        UI_CHECK(ui.defineFilter("warm", "sepia(1) drop-shadow(2px 3px 4px #ff0000)"));
        UI_CHECK(!ui.defineFilter("bad", "sepia("));
        auto sheet = StyleSheet::parse(".e { filter: blur(2px) grayscale(50%) drop-shadow(4px 5px 6px rgba(0,0,0,0.5)); }"
                                       ".u { filter: url(#warm) invert(1); }"
                                       ".b { backdrop-filter: blur(3px) drop-shadow(#00ff00 1px 2px); background: rgba(0,0,0,0); }");
        UI_CHECK(sheet.has_value());
        if (sheet) ui.styles().addSheet(std::move(*sheet));
        const NodeHandle e = w::label(ui, ui.root(), "e");
        const NodeHandle u = w::label(ui, ui.root(), "u");
        const NodeHandle b = w::label(ui, ui.root(), "b");
        ui.addClass(e, "e");
        ui.addClass(u, "u");
        ui.addClass(b, "b");
        ui.update();
        const ComputedStyle& es = ui.computed(e);
        UI_CHECK(es.filterCount == 3 && es.filterFns[0] == FilterFn::Blur && es.filterAmounts[0] == 2.f);
        UI_CHECK(es.filterFns[2] == FilterFn::DropShadow && es.filterAmounts[2] == 0.f);
        UI_CHECK(es.filterShadows[0].x == 4.f && es.filterShadows[0].y == 5.f && es.filterShadows[0].blur == 6.f);
        UI_CHECK(std::abs(es.filterShadows[0].color.a - 0.5f) < 0.01f);
        const ComputedStyle& us = ui.computed(u);
        UI_CHECK(us.filterCount == 2 && us.filterFns[0] == FilterFn::Url && us.filterFns[1] == FilterFn::Invert);
        const ComputedStyle& bs = ui.computed(b);
        UI_CHECK(bs.backdropFilterCount == 2 && bs.backdropFilterFns[0] == FilterFn::Blur && bs.backdropFilterAmounts[0] == 3.f);
        UI_CHECK(bs.backdropShadows[0].x == 1.f && bs.backdropShadows[0].y == 2.f && bs.backdropShadows[0].color.g > 0.99f);
        int pushE = 0, pushU = 0, pops = 0;
        const DrawCmd* bd = nullptr;
        for (const DrawCmd& c : ui.drawList().cmds()) {
            if (c.op == DrawOp::PushFilter && c.node == e.index) pushE += c.blur == 2.f && c.stopCount == 6;
            // url(#warm) = sepia + shadow stage (inner, chained), then invert after the shadow: a second, outer layer
            if (c.op == DrawOp::PushFilter && c.node == u.index) pushU += (c.stopCount == 6 && c.v == 1) || (c.stopCount == 4 && c.v == 0);
            pops += c.op == DrawOp::PopFilter;
            if (c.op == DrawOp::Backdrop && c.node == b.index) bd = &c;
        }
        std::printf("  css filter: push e %d u %d, pops %d\n", pushE, pushU, pops);
        UI_CHECK(pushE == 1 && pushU == 2 && pops == 3);
        UI_CHECK(bd && bd->stopCount == 6);
    }
    // filter functions apply in CSS order (nested layers); several drop-shadows; url() to SVG <filter> programs
    {
        struct Shot { Image img; Rect r; };
        const auto shot = [](const char* filter, const char* defId = nullptr, const char* defText = nullptr) {
            UiContext ui({.viewport = {240, 200}, .font = &font()});
            w::install(ui);
            if (defId) UI_CHECK(ui.defineFilter(defId, defText));
            std::string css = ".q { background: #0000ff; width: 40px; height: 40px; margin-left: 20px; margin-top: 20px; filter: ";
            css += filter;
            css += "; }";
            auto sheet = StyleSheet::parse(css);
            UI_CHECK(sheet.has_value());
            if (sheet) ui.styles().addSheet(std::move(*sheet));
            const NodeHandle q = w::label(ui, ui.root(), "");
            ui.addClass(q, "q");
            ui.update();
            Shot o;
            o.r = ui.rect(q);
            o.img.resize(240, 200, 0xFF000000u);
            CpuRasterizer().render(ui.drawList(), o.img);
            return o;
        };
        const auto px = [](const Shot& o, f32 dx, f32 dy) { return o.img.at(int(o.r.x + dx), int(o.r.y + dy)); };
        const auto is = [](u32 p, int r, int g, int b) {
            return std::abs(ch(p, 0) - r) < 8 && std::abs(ch(p, 1) - g) < 8 && std::abs(ch(p, 2) - b) < 8;
        };
        const Shot a = shot("invert(1) drop-shadow(50px 0 0 #ff0000)");
        const Shot b = shot("drop-shadow(50px 0 0 #ff0000) invert(1)");
        std::printf("  css order: a %08x %08x  b %08x %08x\n", px(a, 20, 20), px(a, 70, 20), px(b, 20, 20), px(b, 70, 20));
        UI_CHECK(is(px(a, 20, 20), 255, 255, 0) && is(px(a, 70, 20), 255, 0, 0));   // shadow of the inverted element
        UI_CHECK(is(px(b, 20, 20), 255, 255, 0) && is(px(b, 70, 20), 0, 255, 255)); // inverted shadow
        const Shot m = shot("drop-shadow(50px 0 0 #ff0000) drop-shadow(0 50px 0 #00ff00)");
        UI_CHECK(is(px(m, 20, 20), 0, 0, 255) && is(px(m, 70, 20), 255, 0, 0));
        UI_CHECK(is(px(m, 20, 70), 0, 255, 0) && is(px(m, 70, 70), 0, 255, 0)); // second shadow of element + first
        const Shot g = shot("url(#g)", "g",
                            "<svg><filter id=\"g\"><feFlood flood-color=\"#00ff00\" result=\"f\"/>"
                            "<feComposite in=\"f\" in2=\"SourceGraphic\" operator=\"in\"/></filter></svg>");
        std::printf("  svg flood: %08x %08x\n", px(g, 20, 20), px(g, 60, 20));
        UI_CHECK(is(px(g, 20, 20), 0, 255, 0) && ch(px(g, 60, 20), 1) < 80);
        const Shot o = shot("url(#o)", "o",
                            "<filter id=\"o\"><feOffset in=\"SourceGraphic\" dx=\"50\" dy=\"0\" result=\"s\"/>"
                            "<feColorMatrix in=\"s\" type=\"matrix\" values=\"0 0 0 0 1  0 0 0 0 0  0 0 0 0 0  0 0 0 1 0\" result=\"r\"/>"
                            "<feMerge><feMergeNode in=\"r\"/><feMergeNode in=\"SourceGraphic\"/></feMerge></filter>");
        std::printf("  svg filter: %08x %08x\n", px(o, 20, 20), px(o, 70, 20));
        UI_CHECK(is(px(o, 20, 20), 0, 0, 255) && is(px(o, 70, 20), 255, 0, 0));
        FilterProgram prog;
        UI_CHECK(parseSvgFilter("<filter id=\"b\"><feGaussianBlur stdDeviation=\"3\"/><feBlend mode=\"multiply\" in2=\"SourceGraphic\"/></filter>", prog));
        UI_CHECK(prog.ops.size() == 3 && prog.ops[0].op == FeOp::Blur && prog.ops[1].op == FeOp::Blend && prog.ops[2].op == FeOp::Copy);
        UI_CHECK(!parseSvgFilter("<filter><feTurbulence/></filter>", prog));
        UiContext ui({.viewport = {64, 64}, .font = &font()});
        UI_CHECK(ui.defineSvgFilters("<svg><filter id=\"x\"><feFlood/></filter><filter id=\"y\"><feOffset dx=\"2\"/></filter></svg>") == 2);
    }
    return finish("fuse_ui_backdrop_blur");
}
