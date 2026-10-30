// fuse_ui_css_gradient: CSS multi-stop linear-gradient(<angle> | to <side>, <color-stop>#) on backgrounds.
//   * parsing: angles (deg / turn / rad), side and corner keywords, stop offsets, missing offsets spread evenly
//   * the legacy 2-stop forms keep their old meaning
//   * paint: the background's pixels follow Gradient::linearAngle over the border box (multi-stop, any angle)
#include "test_util.hpp"

#include <cmath>
#include <cstdio>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
int ch(u32 p, int k) { return int((p >> (8 * k)) & 0xFFu); }
bool near(Color a, f32 r, f32 g, f32 b) { return std::abs(a.r - r) < 1e-3f && std::abs(a.g - g) < 1e-3f && std::abs(a.b - b) < 1e-3f; }
} // namespace

int main() {
    // parse
    {
        auto sh = StyleSheet::parse(".a { background: linear-gradient(90deg, red, lime 25%, blue); }\n"
                                    ".b { background: linear-gradient(to top right, #000, #fff); }\n"
                                    ".c { background: linear-gradient(0.5turn, red, rgba(0,0,255,0.5) 10%, lime, white 90%, black); }\n"
                                    ".d { background: linear-gradient(vertical, red, blue); }\n"
                                    ".e { background: linear-gradient(red, blue); }");
        UI_CHECK(sh.has_value());
        UiContext ui({.viewport = {400, 300}, .font = &font()});
        w::install(ui);
        if (sh) ui.styles().addSheet(std::move(*sh));
        NodeHandle n[5];
        const char* cls[5] = {"a", "b", "c", "d", "e"};
        for (int i = 0; i < 5; ++i) {
            n[i] = w::label(ui, ui.root(), "");
            ui.addClass(n[i], cls[i]);
        }
        ui.update();
        const ComputedStyle& a = ui.computed(n[0]);
        UI_CHECK(a.gradient == GradientDir::Angle && a.gradientAngle == 90.f && a.gradientStopCount == 3);
        UI_CHECK(a.gradientOffsets[0] == 0.f && std::abs(a.gradientOffsets[1] - 0.25f) < 1e-6f && a.gradientOffsets[2] == 1.f);
        UI_CHECK(near(a.gradientColors[1], 0, 1, 0) && near(a.background, 1, 0, 0) && near(a.background2, 0, 0, 1));
        UI_CHECK(ui.computed(n[1]).gradientAngle == 45.f && ui.computed(n[1]).gradientStopCount == 2);
        const ComputedStyle& c = ui.computed(n[2]);
        UI_CHECK(c.gradientAngle == 180.f && c.gradientStopCount == 5);
        // 0, 0.1, (0.1 + 0.9) / 2 = 0.5, 0.9, 1
        UI_CHECK(std::abs(c.gradientOffsets[2] - 0.5f) < 1e-6f && std::abs(c.gradientOffsets[3] - 0.9f) < 1e-6f);
        UI_CHECK(std::abs(c.gradientColors[1].a - 0.5f) < 1e-6f);
        UI_CHECK(ui.computed(n[3]).gradient == GradientDir::Vertical);
        UI_CHECK(ui.computed(n[4]).gradient == GradientDir::Angle && ui.computed(n[4]).gradientAngle == 180.f);
        std::vector<StyleDiagnostic> d;
        (void)StyleSheet::parse(".x { background: linear-gradient(45deg, red); }", &d);
        UI_CHECK(!d.empty());
    }
    // paint: a 200 x 100 box, 90deg red -> lime 25% -> blue; also a 3-stop vertical one for a second angle
    {
        auto sh = StyleSheet::parse(".g { width: 200px; height: 100px; background: linear-gradient(90deg, red, lime 25%, blue); }\n"
                                    ".h { width: 100px; height: 100px; background: linear-gradient(to bottom, white, black 50%, red); }");
        UiContext ui({.viewport = {400, 300}, .font = &font()});
        w::install(ui);
        if (sh) ui.styles().addSheet(std::move(*sh));
        const NodeHandle g = w::label(ui, ui.root(), "");
        const NodeHandle h = w::label(ui, ui.root(), "");
        ui.addClass(g, "g");
        ui.addClass(h, "h");
        ui.update();
        Image img;
        render(ui, img);
        const Rect r = ui.rect(g);
        UI_CHECK(r.w == 200.f && r.h == 100.f);
        auto px = [&](f32 x, f32 y) { return img.at(int(x), int(y)); };
        const u32 p0 = px(r.x + 0.5f, r.y + 50.f), p25 = px(r.x + 50.5f, r.y + 50.f), pEnd = px(r.right() - 0.5f, r.y + 50.f);
        UI_CHECK(ch(p0, 0) > 245 && ch(p0, 1) < 10);
        UI_CHECK(ch(p25, 1) > 245 && ch(p25, 0) < 10 && ch(p25, 2) < 10);
        UI_CHECK(ch(pEnd, 2) > 245 && ch(pEnd, 1) < 10);
        const u32 mid = px(r.x + 125.5f, r.y + 50.f); // halfway lime -> blue
        UI_CHECK(std::abs(ch(mid, 1) - 128) < 6 && std::abs(ch(mid, 2) - 128) < 6);
        const Rect q = ui.rect(h);
        const u32 top = px(q.x + 50.f, q.y + 0.5f), half = px(q.x + 50.f, q.y + 50.5f), bot = px(q.x + 50.f, q.bottom() - 0.5f);
        UI_CHECK(ch(top, 0) > 245 && ch(top, 2) > 245);
        UI_CHECK(ch(half, 0) < 10 && ch(half, 1) < 10);
        UI_CHECK(ch(bot, 0) > 245 && ch(bot, 1) < 10);
    }
    return finish("fuse_ui_css_gradient");
}
