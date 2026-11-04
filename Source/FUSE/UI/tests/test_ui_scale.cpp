// fuse_ui_scale: UiContext::setScale / AppWindow::setScale (mobile density, plan UI1 / UI5). Layout and input stay
// in UI units; the draw list, damage and the presented image come out in device pixels with glyphs rasterised at the
// device size. Checks: geometry doubles exactly at scale 2, glyph quads are whole device pixels, input in UI units
// still clicks, damage is in device pixels, and the 2x frame box-filtered to 1x matches the 1x frame.
#include "fuse/app/app.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace fuse::ui;
namespace app = fuse::app;
namespace w = fuse::ui::widgets;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
struct Scene {
    NodeHandle button, label;
    int clicks = 0;
};
void build(UiContext& ui, Scene& s) {
    const NodeHandle p = w::panel(ui, ui.root(), "p");
    (void)ui.addStyleSheet("#p { padding: 8px; gap: 6px; }");
    s.button = w::button(ui, p, "Press", "b");
    s.label = w::label(ui, p, "Scale test 123", "l");
    ui.connect(s.button, SignalKind::Clicked, [&s](UiContext&, NodeHandle, const SignalArgs&) { ++s.clicks; });
}
} // namespace

int main() {
    app::Application a{{.name = "fuse_ui_scale", .loadSettings = false}};
    app::AppWindow& w1 = a.createWindow({.title = "1x", .width = 200, .height = 100});
    app::AppWindow& w2 = a.createWindow({.title = "2x", .width = 200, .height = 100});
    Scene s1, s2;
    build(w1.ui(), s1);
    build(w2.ui(), s2);
    w2.setScale(2.f); // keeps the device size (logical 100 x 50) ...
    CHECK(w2.size().w == 100.f && w2.size().h == 50.f);
    w2.resize(200, 100); // ... then the same logical size as w1
    CHECK(w2.pixelSize().w == 400.f && w2.pixelSize().h == 200.f);
    a.step(0.f);
    CHECK(w1.image().width == 200 && w1.image().height == 100);
    CHECK(w2.image().width == 400 && w2.image().height == 200);

    // same layout in UI units
    const Rect b1 = w1.ui().rect(s1.button), b2 = w2.ui().rect(s2.button);
    CHECK(b1.x == b2.x && b1.y == b2.y && b1.w == b2.w && b1.h == b2.h);

    // geometry doubles; glyphs are whole device pixels
    const auto& c1 = w1.ui().drawList().cmds();
    const auto& c2 = w2.ui().drawList().cmds();
    std::size_t rects = 0, glyphs = 0;
    for (std::size_t i = 0, j = 0; i < c1.size() && j < c2.size(); ++i, ++j) {
        while (i < c1.size() && c1[i].op == DrawOp::Glyph) ++i;
        while (j < c2.size() && c2[j].op == DrawOp::Glyph) ++j;
        if (i == c1.size() || j == c2.size()) break;
        CHECK(c1[i].op == c2[j].op);
        CHECK(std::abs(c1[i].rect.x * 2.f - c2[j].rect.x) < 1e-3f && std::abs(c1[i].rect.w * 2.f - c2[j].rect.w) < 1e-3f);
        CHECK(std::abs(c1[i].radius[0] * 2.f - c2[j].radius[0]) < 1e-3f);
        ++rects;
    }
    for (const DrawCmd& c : c2) {
        if (c.op != DrawOp::Glyph) continue;
        ++glyphs;
        CHECK(std::abs(c.rect.w - f32(c.tw)) < 1e-3f && std::abs(c.rect.h - f32(c.th)) < 1e-3f);
        CHECK(std::abs(c.rect.x - std::round(c.rect.x)) < 1e-3f && std::abs(c.rect.y - std::round(c.rect.y)) < 1e-3f);
    }
    CHECK(rects >= 2 && glyphs > 5);

    // input stays in UI units
    w2.click({b2.x + b2.w * 0.5f, b2.y + b2.h * 0.5f});
    a.step(0.f);
    CHECK(s2.clicks == 1);

    // damage is in device pixels
    w2.ui().setText(s2.label, "Changed");
    w2.ui().update(0.f);
    const Rect d = w2.ui().damage(), l = w2.ui().rect(s2.label);
    CHECK(d.x <= l.x * 2.f && d.y <= l.y * 2.f && d.x + d.w >= (l.x + l.w) * 2.f - 1.f && d.y + d.h >= (l.y + l.h) * 2.f - 1.f);
    CHECK(d.x + d.w <= 400.f + 1.f && d.y + d.h <= 200.f + 1.f);

    // 2x frame box-filtered to 1x ~ 1x frame (text hinting differs slightly)
    w1.ui().setText(s1.label, "Changed");
    w1.click({b1.x + b1.w * 0.5f, b1.y + b1.h * 0.5f});
    w1.ui().pointerMove({-100.f, -100.f});
    w2.ui().pointerMove({-100.f, -100.f});
    w1.requestRedraw();
    w2.requestRedraw();
    a.step(0.f);
    const Image& i1 = w1.image();
    const Image& i2 = w2.image();
    double sum = 0.0;
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 200; ++x)
            for (int ch = 0; ch < 24; ch += 8) {
                int avg = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) avg += int((i2.at(2 * x + dx, 2 * y + dy) >> ch) & 0xFF);
                sum += std::abs(avg / 4 - int((i1.at(x, y) >> ch) & 0xFF));
            }
    const double mean = sum / (200.0 * 100.0 * 3.0);
    std::printf("fuse_ui_scale: %zu rects, %zu glyphs, 2x->1x mean error %.2f\n", rects, glyphs, mean);
    CHECK(mean < 4.0);
    if (g_fail) std::fprintf(stderr, "fuse_ui_scale: %d failures\n", g_fail);
    return g_fail ? 1 : 0;
}
