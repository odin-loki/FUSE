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
    return finish("fuse_ui_backdrop_blur");
}
