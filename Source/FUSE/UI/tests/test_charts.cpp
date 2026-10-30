// fuse::gui::charts gates: every 2D series type renders, axes (value / log / category / multiple), decimation
// (min/max envelope + LTTB), streaming ring, hit testing, widget interaction (hover, zoom, pan, band zoom, select,
// legend, keys), a11y, model binding, PNG export, a 1M-point perf bench and the demo image.
#include "fuse/ui/extras/charts.hpp"
#include "fuse/ui/extras/extras.hpp"
#include "test_util.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

using namespace fuse::ui;
namespace ch = fuse::ui::charts;
namespace w = fuse::ui::widgets;
#define CHECK UI_CHECK

namespace {
void step(UiContext& ui) { ui.update(1.f / 60.f); }
struct Screen {
    UiContext ui{{.viewport = {1200, 800}, .font = &fuse::ui::test::font()}};
    Screen() { w::install(ui); w::registerExtraWidgets(ui); step(ui); }
};
std::vector<f32> dv3d_ticks(const fuse::ui::dv3d::Scene& s) { return fuse::ui::dv3d::axisTicks(s, 1); }
std::string a11yName(const UiContext& ui, NodeHandle n) {
    std::vector<A11ySnapshotNode> v;
    ui.snapshotA11y(v);
    for (const auto& s : v) if (s.node == n) return std::string(s.name);
    return {};
}
ch::TextFns fns(UiContext& ui) {
    ch::TextFns t;
    t.measure = [&ui](std::string_view s, f32 px) { return ui.measureString(s, px); };
    t.draw = [&ui](DrawList& d, std::string_view s, Rect r, Color c, f32 px, TextAlignment a) { ui.paintString(d, s, r, c, px, a, 0); };
    return t;
}
std::array<int, 3> rgb(const Image& img, int x, int y) {
    const u32 v = img.at(x, y);
    return {int(v & 0xFF), int((v >> 8) & 0xFF), int((v >> 16) & 0xFF)};
}
int diffPixels(const Image& a, const Image& b) {
    int n = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) n += a.pixels[i] != b.pixels[i];
    return n;
}
// distinct colours in a region: a crude "something was drawn" probe
int colours(const Image& img, Rect r) {
    std::vector<u32> seen;
    for (int y = int(r.y); y < int(r.bottom()); y += 2)
        for (int x = int(r.x); x < int(r.right()); x += 2)
            if (std::find(seen.begin(), seen.end(), img.at(x, y)) == seen.end() && seen.size() < 64) seen.push_back(img.at(x, y));
    return int(seen.size());
}

ch::Chart sampleChart(ch::SeriesType t) {
    ch::Chart c;
    c.animate = false;
    c.title = "sample";
    ch::Series& s = c.add(t, "a");
    switch (t) {
    case ch::SeriesType::Pie: case ch::SeriesType::Donut:
        s.y = {3, 2, 1, 4}, s.labels = {"w", "x", "y", "z"};
        break;
    case ch::SeriesType::Candlestick: case ch::SeriesType::Ohlc:
        for (int i = 0; i < 20; ++i) { const f32 o = 10 + std::sin(f32(i)) * 3; s.candles.push_back({f32(i), o, o + 2, o - 2, o + (i % 2 ? 1.f : -1.f)}); }
        break;
    case ch::SeriesType::BoxPlot:
        for (int i = 0; i < 5; ++i) s.boxes.push_back({f32(i), 1.f + i, 3.f + i, 4.f + i, 5.f + i, 7.f + i, {9.f + i}});
        break;
    case ch::SeriesType::Histogram: {
        std::mt19937 g(1); std::normal_distribution<f32> d(0, 1);
        for (int i = 0; i < 2000; ++i) s.y.push_back(d(g));
        break;
    }
    case ch::SeriesType::Waterfall:
        s.y = {120, -30, 45, -20, 0, 60, -15, 0}, s.totals = {4, 7};
        s.labels = {"Q1", "Q2", "Q3", "Q4", "H1", "Q5", "Q6", "Total"};
        break;
    case ch::SeriesType::Radar: {
        s.y = {4, 3, 5, 2, 4, 3}, s.labels = {"speed", "power", "range", "cost", "comfort", "safety"};
        ch::Series& b = c.add(t, "b");
        b.y = {2, 5, 3, 4, 2, 5};
        break;
    }
    case ch::SeriesType::Treemap:
        s.y = {6, 6, 4, 3, 2, 2, 1}, s.labels = {"a", "b", "c", "d", "e", "f", "g"};
        break;
    case ch::SeriesType::Sankey:
        s.labels = {"coal", "gas", "solar", "power", "heat", "homes", "industry"};
        s.links = {{0, 3, 30}, {1, 3, 20}, {1, 4, 15}, {2, 3, 10}, {3, 5, 35}, {3, 6, 25}, {4, 5, 10}, {4, 6, 5}};
        break;
    case ch::SeriesType::Gauge:
        s.y = {72}, s.colormap = {{0.f, Color::rgb(0x26A69A)}, {0.6f, Color::rgb(0xFFCA28)}, {0.85f, Color::rgb(0xEF5350)}, {1.f, Color::rgb(0xEF5350)}};
        break;
    case ch::SeriesType::Funnel:
        s.y = {1000, 620, 380, 210, 90}, s.labels = {"visits", "signups", "trials", "paid", "renewed"};
        break;
    case ch::SeriesType::Contour:
    case ch::SeriesType::Heatmap:
        s.rows = 10, s.cols = 12;
        for (u32 i = 0; i < 120; ++i) s.y.push_back(std::sin(f32(i % 12) * 0.5f) * std::cos(f32(i / 12) * 0.4f));
        break;
    default:
        for (int i = 0; i < 30; ++i) { s.x.push_back(f32(i)); s.y.push_back(5 + 4 * std::sin(f32(i) * 0.3f)); }
        if (t == ch::SeriesType::Bubble) for (int i = 0; i < 30; ++i) s.size.push_back(f32(2 + i % 7));
        if (t == ch::SeriesType::StackedArea || t == ch::SeriesType::PercentArea || t == ch::SeriesType::StackedBar ||
            t == ch::SeriesType::PercentBar || t == ch::SeriesType::HorizontalStackedBar || t == ch::SeriesType::HorizontalPercentBar) {
            ch::Series& b = c.add(t, "b");
            for (int i = 0; i < 30; ++i) { b.x.push_back(f32(i)); b.y.push_back(3 + 2 * std::cos(f32(i) * 0.2f)); }
        }
    }
    return c;
}

void gateSeries() {
    Screen s;
    const auto tf = fns(s.ui);
    for (u32 t = 0; t <= u32(ch::SeriesType::Funnel); ++t) {
        ch::Chart c = sampleChart(ch::SeriesType(t));
        DrawList dl;
        ch::RenderStats st;
        ch::render(c, dl, {0, 0, 400, 300}, &tf, &st);
        Image img;
        img.resize(400, 300, 0xFF000000u);
        CpuRasterizer r;
        r.setGlyphAtlas(s.ui.atlas().pixels(), s.ui.atlas().size());
        r.render(dl, img);
        const ch::Layout L = ch::layout(c, {0, 0, 400, 300}, &tf);
        const int k = colours(img, L.plot);
        if (k < 3) std::fprintf(stderr, "series type %u drew %d colours\n", t, k);
        CHECK(k >= 3);
        CHECK(dl.size() > 4);
        CHECK(!ch::describe(c).empty());
        // the tooltip of point 0 names the series or slice
        CHECK(!ch::tooltip(c, 0, 0).empty());
    }
    // polar line
    ch::Chart p = sampleChart(ch::SeriesType::Line);
    p.polar = true;
    DrawList dl;
    ch::render(p, dl, {0, 0, 400, 300}, &tf);
    CHECK(dl.size() > 10);
}

void gateAxes() {
    ch::Axis v;
    auto t = ch::ticks(v, 0, 10);
    CHECK(t.size() >= 3 && t.front() >= 0 && t.back() <= 10);
    ch::Axis lg; lg.type = ch::AxisType::Log;
    t = ch::ticks(lg, 1, 10000);
    CHECK(t.size() == 5 && std::abs(t[2] - 100) < 1e-3f);
    // log mapping: decades are evenly spaced
    const f32 p10 = ch::toPixel(lg, 1, 10000, 10, 0, 400), p100 = ch::toPixel(lg, 1, 10000, 100, 0, 400);
    CHECK(std::abs(p10 - 100) < 0.5f && std::abs(p100 - 200) < 0.5f);
    CHECK(std::abs(ch::fromPixel(lg, 1, 10000, 300, 0, 400) - 1000) < 1.f);
    ch::Axis rv; rv.reversed = true;
    CHECK(std::abs(ch::toPixel(rv, 0, 10, 0, 0, 100) - 100) < 1e-3f);
    ch::Axis cat; cat.type = ch::AxisType::Category; cat.categories = {"a", "b", "c"};
    CHECK(ch::formatTick(cat, 1) == "b");
    // ranges: nice rounding on data, fixed min/max honoured, multiple y axes independent
    ch::Chart c = sampleChart(ch::SeriesType::Line);
    ch::Axis right; right.edge = ch::AxisEdge::Right;
    c.axes.push_back(right);
    ch::Series& s2 = c.add(ch::SeriesType::Line, "big");
    s2.yAxis = 2;
    s2.x = {0, 29}, s2.y = {1000, 5000};
    auto rg = ch::axisRanges(c);
    CHECK(rg.size() == 3 && rg[1].second < 20 && rg[2].second >= 5000 && rg[2].first <= 1000);
    c.axes[1].min = -5, c.axes[1].max = 50;
    rg = ch::axisRanges(c);
    CHECK(rg[1].first == -5 && rg[1].second == 50);
    // zoom / pan / reset
    ch::zoomTo(c, 0, 5, 10);
    rg = ch::axisRanges(c);
    CHECK(rg[0].first == 5 && rg[0].second == 10);
    ch::pan(c, 0, 2);
    rg = ch::axisRanges(c);
    CHECK(rg[0].first == 7 && rg[0].second == 12);
    ch::zoom(c, 0, 0.5f, 9.5f);
    rg = ch::axisRanges(c);
    CHECK(std::abs(rg[0].first - 8.25f) < 1e-3f && std::abs(rg[0].second - 10.75f) < 1e-3f);
    ch::resetZoom(c);
    CHECK(std::isnan(c.axes[0].viewMin));
}

void gateDecimation() {
    // min/max SIMD vs scalar
    std::mt19937 g(7);
    std::uniform_real_distribution<f32> u(-100, 100);
    std::vector<f32> v(100003);
    for (f32& x : v) x = u(g);
    v[777] = NAN;
    f32 lo, hi;
    ch::minMax(v, lo, hi);
    f32 slo = INFINITY, shi = -INFINITY;
    for (f32 x : v) if (x == x) slo = std::min(slo, x), shi = std::max(shi, x);
    CHECK(lo == slo && hi == shi);
    // visibleRange
    std::vector<f32> xs(1000);
    for (int i = 0; i < 1000; ++i) xs[std::size_t(i)] = f32(i);
    auto [a, b] = ch::visibleRange(xs, 100.5f, 200.5f);
    CHECK(a <= 100 && a >= 99 && b >= 201 && b <= 203);
    // min/max decimation keeps every bucket extreme
    const std::size_t N = 1'000'000;
    std::vector<f32> x(N), y(N);
    for (std::size_t i = 0; i < N; ++i) x[i] = f32(i), y[i] = std::sin(f32(i) * 0.001f) + u(g) * 0.01f;
    y[123456] = 50.f, y[654321] = -50.f;
    std::vector<u32> idx;
    ch::decimateMinMax(x, y, 800, idx);
    CHECK(idx.size() <= 800 * 4 && idx.size() >= 800);
    CHECK(std::is_sorted(idx.begin(), idx.end()));
    CHECK(std::find(idx.begin(), idx.end(), 123456u) != idx.end());
    CHECK(std::find(idx.begin(), idx.end(), 654321u) != idx.end());
    CHECK(idx.front() == 0 && idx.back() == N - 1);
    // LTTB
    ch::lttb(x, y, 1000, idx);
    CHECK(idx.size() == 1000 && idx.front() == 0 && idx.back() == N - 1 && std::is_sorted(idx.begin(), idx.end()));
    CHECK(std::find(idx.begin(), idx.end(), 123456u) != idx.end()); // the spike is the largest triangle of its bucket
}

void gateRing() {
    ch::Series s;
    s.capacity = 100;
    for (int i = 0; i < 1000; ++i) s.append(f32(i), f32(i * 2));
    CHECK(s.count() == 100);
    CHECK(s.xs().front() == 900 && s.xs().back() == 999 && s.ys().back() == 1998);
    CHECK(s.x.size() <= 200); // compaction bounds the storage
    std::vector<f32> bx(250), by(250);
    for (int i = 0; i < 250; ++i) bx[std::size_t(i)] = f32(1000 + i), by[std::size_t(i)] = 0;
    s.append(bx, by);
    CHECK(s.count() == 100 && s.xs().front() == 1150);
    s.clear();
    CHECK(s.count() == 0);
}

void gateHit() {
    Screen s;
    const auto tf = fns(s.ui);
    const Rect B{0, 0, 600, 400};
    ch::Chart c = sampleChart(ch::SeriesType::Line);
    const ch::Layout L = ch::layout(c, B, &tf);
    const auto rg = ch::axisRanges(c);
    const f32 px = ch::toPixel(c.axes[0], rg[0].first, rg[0].second, 10, L.plot.x, L.plot.right());
    const f32 py = ch::toPixel(c.axes[1], rg[1].first, rg[1].second, c.series[0].y[10], L.plot.bottom(), L.plot.y);
    ch::Hit h = ch::hitTest(c, B, {px + 2, py - 2}, 10, &tf);
    CHECK(h.series == 0 && h.index == 10);
    CHECK(ch::hitTest(c, B, {px, L.plot.y + 1}, 4, &tf).series == -1 || std::abs(py - L.plot.y) < 6);
    // legend item
    CHECK(L.legendItems.size() == 1 && L.legendItems[0].w > 0);
    h = ch::hitTest(c, B, {L.legendItems[0].x + 3, L.legendItems[0].y + L.legendItems[0].h * 0.5f}, 10, &tf);
    CHECK(h.legendItem == 0);
    // bars
    ch::Chart b = sampleChart(ch::SeriesType::Bar);
    const ch::Layout BL = ch::layout(b, B, &tf);
    const auto brg = ch::axisRanges(b);
    const f32 bx = ch::toPixel(b.axes[0], brg[0].first, brg[0].second, 5, BL.plot.x, BL.plot.right());
    h = ch::hitTest(b, B, {bx, BL.plot.bottom() - 3}, 10, &tf);
    CHECK(h.series == 0 && h.index == 5);
    // pie: the first slice starts at 12 o'clock and runs clockwise
    ch::Chart p = sampleChart(ch::SeriesType::Pie);
    const ch::Layout PL = ch::layout(p, B, &tf);
    const Vec2 ctr{PL.plot.x + PL.plot.w * 0.5f, PL.plot.y + PL.plot.h * 0.5f};
    h = ch::hitTest(p, B, {ctr.x + 20, ctr.y - 40}, 10, &tf);
    CHECK(h.series == 0 && h.index == 0);
    h = ch::hitTest(p, B, {ctr.x - 20, ctr.y - 40}, 10, &tf);
    CHECK(h.series == 0 && h.index == 3);
    // heatmap cell
    ch::Chart hm = sampleChart(ch::SeriesType::Heatmap);
    const ch::Layout HL = ch::layout(hm, B, &tf);
    const auto hrg = ch::axisRanges(hm);
    const Vec2 c13{ch::toPixel(hm.axes[0], hrg[0].first, hrg[0].second, 3, HL.plot.x, HL.plot.right()),
                   ch::toPixel(hm.axes[1], hrg[1].first, hrg[1].second, 1, HL.plot.bottom(), HL.plot.y)};
    h = ch::hitTest(hm, B, c13, 10, &tf);
    CHECK(h.series == 0 && h.index == 1 * 12 + 3);
}

void gateWidget() {
    Screen s;
    UiContext& ui = s.ui;
    const NodeHandle v = ch::chartView(ui, ui.root(), "chart");
    ui.setCss(v, "width", "600px");
    ui.setCss(v, "height", "400px");
    ch::Chart& c = *ch::chartOf(ui, v);
    c = sampleChart(ch::SeriesType::Line);
    c.title = "Signal";
    c.animate = true;
    ch::Series& b = c.add(ch::SeriesType::Scatter, "dots");
    b.x = {2, 4, 6}, b.y = {1, 2, 3};
    ch::chartChanged(ui, v, true);
    step(ui);
    CHECK(c.progress < 1.f);
    for (int i = 0; i < 40; ++i) step(ui);
    CHECK(c.progress == 1.f);
    CHECK(a11yName(ui, v).find("Signal") != std::string::npos && a11yName(ui, v).find("2 series") != std::string::npos);
    const Rect R = ui.rect(v);
    const auto tf = fns(ui);
    const ch::Layout L = ch::layout(c, R, &tf);
    auto rg = ch::axisRanges(c);
    auto at = [&](f32 x, f32 y) {
        return Vec2{ch::toPixel(c.axes[0], rg[0].first, rg[0].second, x, L.plot.x, L.plot.right()),
                    ch::toPixel(c.axes[1], rg[1].first, rg[1].second, y, L.plot.bottom(), L.plot.y)};
    };
    // hover -> tooltip + value
    const Vec2 p = at(12, c.series[0].y[12]);
    ui.pointerMove(p);
    step(ui);
    CHECK(ch::chartHovered(ui, v).series == 0 && ch::chartHovered(ui, v).index == 12);
    CHECK(ch::chartTooltip(ui, v).rfind("a: (12", 0) == 0);
    CHECK(ui.value(v) == 12.f);
    // click selects, Ctrl+click adds
    int toggles = 0;
    ui.connect(v, SignalKind::Toggled, [&](UiContext&, NodeHandle, const SignalArgs&) { ++toggles; });
    ui.pointerDown(p); ui.pointerUp(p); step(ui);
    CHECK(c.selection.size() == 1 && c.selection[0].index == 12);
    const Vec2 q = at(4, 2);
    ui.setModifiers(kModCtrl);
    ui.pointerMove(q); ui.pointerDown(q); ui.pointerUp(q); step(ui);
    ui.setModifiers(0);
    CHECK(c.selection.size() == 2 && toggles == 2);
    // wheel zooms x around the pointer
    const auto r0 = ch::axisRanges(c);
    ui.wheel(p, {0, 2});
    step(ui);
    auto r1 = ch::axisRanges(c);
    CHECK(r1[0].second - r1[0].first < (r0[0].second - r0[0].first) * 0.8f);
    CHECK(r1[1] == r0[1]);
    // drag pans
    ui.pointerMove(p); ui.pointerDown(p);
    ui.pointerMove({p.x - 60, p.y}); ui.pointerUp({p.x - 60, p.y}); step(ui);
    const auto r2 = ch::axisRanges(c);
    CHECK(r2[0].first > r1[0].first + 0.1f);
    CHECK(c.selection.size() == 2); // a drag is not a click
    // Escape resets
    ui.setFocus(v);
    ui.keyDown(Key::Escape); step(ui);
    CHECK(std::isnan(c.axes[0].viewMin));
    // Shift+drag band zoom
    rg = ch::axisRanges(c);
    const Vec2 b0 = at(5, 8), b1 = at(15, 2);
    ui.setModifiers(kModShift);
    ui.pointerMove(b0); ui.pointerDown(b0); ui.pointerMove(b1); ui.pointerUp(b1); step(ui);
    ui.setModifiers(0);
    const auto r3 = ch::axisRanges(c);
    CHECK(std::abs(r3[0].first - 5) < 0.3f && std::abs(r3[0].second - 15) < 0.3f);
    CHECK(std::abs(r3[1].first - 2) < 0.3f && std::abs(r3[1].second - 8) < 0.3f);
    ui.keyDown(Key::Escape); step(ui);
    // keyboard point walk updates the a11y text
    ui.keyDown(Key::Home); step(ui);
    ui.keyDown(Key::Right); step(ui);
    CHECK(ch::chartHovered(ui, v).index == 1);
    CHECK(a11yName(ui, v).rfind("a: (1", 0) == 0);
    // legend click hides a series
    const ch::Layout L2 = ch::layout(c, R, &tf);
    const Rect li = L2.legendItems[1];
    const Vec2 lp{li.x + 4, li.y + li.h * 0.5f};
    ui.pointerMove(lp); ui.pointerDown(lp); ui.pointerUp(lp); step(ui);
    CHECK(!c.series[1].visible);
    CHECK(a11yName(ui, v).find("hidden") != std::string::npos);
    // paints: the tooltip box shows after hovering
    ui.pointerMove(p); step(ui);
    Image img;
    fuse::ui::test::render(ui, img);
    CHECK(colours(img, R) > 6);
}

void gateModel() {
    Screen s;
    UiContext& ui = s.ui;
    const NodeHandle v = ch::chartView(ui, ui.root(), "m");
    w::StringTableModel m("t\tcpu\tmem");
    m.addRow("0\t10\t40");
    m.addRow("1\t20\t35");
    m.addRow("2\t15\t50");
    ch::bindModel(ui, v, {.model = &m.model(), .xColumn = 0, .yColumns = {1, 2}, .type = ch::SeriesType::Bar});
    ch::Chart& c = *ch::chartOf(ui, v);
    CHECK(c.series.size() == 2 && c.series[0].name == "cpu" && c.series[1].name == "mem");
    CHECK(c.series[1].count() == 3 && c.series[1].ys()[2] == 50);
    CHECK(c.series[0].type == ch::SeriesType::Bar);
    m.addRow("3\t30\t20");
    ch::chartModelChanged(ui, v);
    CHECK(c.series[0].count() == 4 && c.series[0].ys()[3] == 30);
}

void gateExport() {
    Screen s;
    UiContext& ui = s.ui;
    const NodeHandle v = ch::chartView(ui, ui.root(), "e");
    *ch::chartOf(ui, v) = sampleChart(ch::SeriesType::Area);
    ch::chartChanged(ui, v);
    const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/fuse_chart_export.png";
    CHECK(ch::exportImage(ui, v, path, 320, 200));
    FILE* f = std::fopen(path.c_str(), "rb");
    CHECK(f != nullptr);
    if (f) {
        unsigned char hdr[24];
        CHECK(std::fread(hdr, 1, 24, f) == 24);
        std::fclose(f);
        CHECK(std::memcmp(hdr, "\x89PNG\r\n\x1a\n", 8) == 0 && std::memcmp(hdr + 12, "IHDR", 4) == 0);
        CHECK(hdr[19] == 64 && hdr[23] == 200); // width 320 (0x140), height 200
        std::remove(path.c_str());
    }
    // renderImage is deterministic
    Image a, b;
    ch::renderImage(ui, sampleChart(ch::SeriesType::Candlestick), 300, 200, a);
    ch::renderImage(ui, sampleChart(ch::SeriesType::Candlestick), 300, 200, b);
    CHECK(diffPixels(a, b) == 0);
}

void gateBench() {
    Screen s;
    const auto tf = fns(s.ui);
    ch::Chart c;
    c.animate = false;
    ch::Series& sr = c.add(ch::SeriesType::Line, "1M");
    const std::size_t N = 1'000'000;
    sr.x.resize(N), sr.y.resize(N);
    std::mt19937 g(3);
    std::normal_distribution<f32> d(0, 1);
    f32 acc = 0;
    for (std::size_t i = 0; i < N; ++i) sr.x[i] = f32(i), sr.y[i] = acc += d(g);
    DrawList dl;
    ch::RenderStats st;
    ch::render(c, dl, {0, 0, 1200, 600}, &tf, &st); // warm
    const int iters = 10;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) { dl.clear(); st = {}; ch::render(c, dl, {0, 0, 1200, 600}, &tf, &st); }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / iters;
    Image img;
    img.resize(1200, 600, 0xFF000000u);
    CpuRasterizer r;
    r.setGlyphAtlas(s.ui.atlas().pixels(), s.ui.atlas().size());
    const auto t1 = std::chrono::steady_clock::now();
    r.render(dl, img);
    const double rms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    // streaming: 1M-capacity ring, 10k appends per frame
    ch::Series ring;
    ring.capacity = N;
    for (std::size_t i = 0; i < N; ++i) ring.append(f32(i), 0.f);
    const auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; ++i) ring.append(f32(N + std::size_t(i)), 1.f);
    const double ams = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count();
    std::printf("bench: 1M-point line -> %zu drawn points, %zu draw commands, render %.2f ms, raster %.2f ms; ring 10k appends %.2f ms\n",
                st.drawnPoints, dl.size(), ms, rms, ams);
    CHECK(st.inputPoints == N);
    CHECK(st.drawnPoints <= 1200 * 4);
    CHECK(dl.size() < 400);
    CHECK(ms < 250.0);
}

void gateDemo() {
    Screen s;
    UiContext& ui = s.ui;
    const int W = 1200, H = 800, cw = W / 4, chh = H / 3;
    DrawList dl;
    const auto tf = fns(ui);
    const ch::SeriesType kinds[] = {ch::SeriesType::Spline, ch::SeriesType::StackedArea, ch::SeriesType::StackedBar, ch::SeriesType::HorizontalBar,
                                    ch::SeriesType::Donut, ch::SeriesType::Candlestick, ch::SeriesType::BoxPlot, ch::SeriesType::Histogram,
                                    ch::SeriesType::Heatmap, ch::SeriesType::Bubble, ch::SeriesType::Scatter, ch::SeriesType::Line};
    const char* names[] = {"Spline", "Stacked area", "Stacked bar", "Horizontal bar", "Donut", "Candlestick", "Box plot", "Histogram", "Heatmap", "Bubble", "Scatter", "Polar"};
    for (int i = 0; i < 12; ++i) {
        ch::Chart c = sampleChart(kinds[i]);
        c.title = names[i];
        if (i == 11) c.polar = true;
        if (i == 10) { c.axes[1].type = ch::AxisType::Log; for (f32& y : c.series[0].y) y = std::exp(y); }
        ch::render(c, dl, {f32(i % 4 * cw), f32(i / 4 * chh), f32(cw), f32(chh)}, &tf);
    }
    Image img;
    img.resize(W, H, 0xFF000000u);
    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    r.render(dl, img);
    const char* out = std::getenv("FUSE_CHARTS_DEMO");
    if (out) CHECK(ch::writePng(img, out));
    CHECK(colours(img, {0, 0, f32(W), f32(H)}) > 30);
}
void gateBeyondQt() {
    Screen sc;
    const auto tf = fns(sc.ui);
    const Rect B{0, 0, 480, 360};
    auto hitAll = [&](const ch::Chart& c, u32 si, std::size_t expect) { // every index is reachable by some pixel
        std::vector<u8> seen(expect, 0);
        for (f32 y = 0; y < B.h; y += 2)
            for (f32 x = 0; x < B.w; x += 2) {
                const ch::Hit h = ch::hitTest(c, B, {x, y}, 6.f, &tf);
                if (h.series == i32(si) && h.index >= 0 && std::size_t(h.index) < expect) seen[std::size_t(h.index)] = 1;
            }
        std::size_t n = 0;
        for (u8 v : seen) n += v;
        if (n != expect) std::fprintf(stderr, "hit %zu / %zu\n", n, expect);
        return n == expect;
    };
    // waterfall: floating bars on the running total; totals restart from 0; range covers the peak (165)
    {
        ch::Chart c = sampleChart(ch::SeriesType::Waterfall);
        const auto rg = ch::axisRanges(c);
        CHECK(rg[1].first <= 0.f && rg[1].second >= 175.f);
        CHECK(ch::tooltip(c, 0, 4).find("total 115") != std::string::npos);
        CHECK(ch::tooltip(c, 0, 1).find("-30") != std::string::npos);
        CHECK(ch::tooltip(c, 0, 7).find("total 160") != std::string::npos);
        CHECK(hitAll(c, 0, 8));
    }
    // contour: isolines cross where expected (a radial bowl gives closed rings, one per level)
    {
        ch::Chart c;
        c.animate = false;
        ch::Series& s = c.add(ch::SeriesType::Contour, "bowl");
        s.rows = s.cols = 41, s.levels = 4;
        for (u32 i = 0; i < 41 * 41; ++i) { const f32 x = f32(i % 41) - 20, y = f32(i / 41) - 20; s.y.push_back(std::sqrt(x * x + y * y)); }
        DrawList dl;
        ch::RenderStats st;
        ch::render(c, dl, B, &tf, &st);
        CHECK(st.drawnPoints > 4 * 40 && st.drawnPoints < 4 * 400);
        Image img;
        img.resize(480, 360, 0xFF000000u);
        CpuRasterizer r;
        r.render(dl, img);
        const ch::Layout L = ch::layout(c, B, &tf);
        // walking right from the centre crosses 4 isolines (colour changes from the plot fill)
        const int yc = int(L.plot.y + L.plot.h * 0.5f);
        int crossings = 0;
        bool on = false;
        const u32 bg = img.at(int(L.plot.x + L.plot.w * 0.5f) + 2, yc + 3);
        for (int x = int(L.plot.x + L.plot.w * 0.5f) + 2; x < int(L.plot.right()) - 2; ++x) {
            const bool line = img.at(x, yc) != bg && img.at(x, yc) != img.at(int(L.plot.right()) - 3, yc);
            if (line && !on) ++crossings;
            on = line;
        }
        if (crossings != 4) std::fprintf(stderr, "contour crossings %d\n", crossings);
        CHECK(crossings >= 4 && crossings <= 5);
        CHECK(ch::hitTest(c, B, {L.plot.x + L.plot.w * 0.5f, L.plot.y + L.plot.h * 0.5f}, 4.f, &tf).index == 20 * 41 + 20);
    }
    // radar: both series share one web; every spoke of each is hit-testable
    {
        ch::Chart c = sampleChart(ch::SeriesType::Radar);
        CHECK(hitAll(c, 0, 6) && hitAll(c, 1, 6));
        CHECK(ch::describe(c).find("radar") != std::string::npos);
    }
    // treemap: cell areas proportional to the values (squarified, no overlap)
    {
        ch::Chart c = sampleChart(ch::SeriesType::Treemap);
        c.title.clear();
        c.legend = ch::LegendPos::None;
        std::vector<int> px(7, 0);
        for (f32 y = 0.5f; y < B.h; y += 1)
            for (f32 x = 0.5f; x < B.w; x += 1) {
                const ch::Hit h = ch::hitTest(c, B, {x, y}, 0.f, &tf);
                if (h.series == 0 && h.index >= 0) px[std::size_t(h.index)]++;
            }
        const f32 total = 24.f;
        int all = 0;
        for (int v : px) all += v;
        for (u32 i = 0; i < 7; ++i) {
            const f32 share = f32(px[i]) / f32(all), want = c.series[0].y[i] / total;
            if (std::abs(share - want) > 0.03f) std::fprintf(stderr, "treemap %u share %.3f want %.3f\n", i, share, want);
            CHECK(std::abs(share - want) <= 0.03f);
        }
    }
    // sankey: node columns by depth, flows conserve (power node in = out = 60), every link and node hit-testable
    {
        ch::Chart c = sampleChart(ch::SeriesType::Sankey);
        CHECK(hitAll(c, 0, 8 + 7));
        CHECK(ch::tooltip(c, 0, 8 + 3) == "power: in 60, out 60");
        CHECK(ch::tooltip(c, 0, 0) == "coal -> power: 30");
        CHECK(ch::describe(c).find("8 flows") != std::string::npos);
    }
    // sankey node order: crossing flows untangle (barycentric), explicit nodeOrder wins, autoOrder off = index order
    {
        ch::Chart c;
        c.title.clear();
        c.legend = ch::LegendPos::None;
        ch::Series& s = c.add(ch::SeriesType::Sankey, "x");
        s.labels = {"a", "b", "x", "y"};
        s.links = {{0, 3, 10}, {1, 2, 10}};
        const ch::Layout L = ch::layout(c, B, &tf);
        auto topNode = [&] { // first node hit scanning down the right column
            for (f32 y = L.plot.y; y < L.plot.bottom(); y += 1) {
                const ch::Hit h = ch::hitTest(c, B, {L.plot.right() - 4.f, y}, 0.f, &tf);
                if (h.series == 0 && h.index >= 2) return h.index - 2;
            }
            return -1;
        };
        CHECK(topNode() == 3); // y beside a: no crossing
        c.series[0].sankeyAutoOrder = false;
        CHECK(topNode() == 2);
        c.series[0].sankeyAutoOrder = true;
        c.series[0].nodeOrder = {1, 0, 2, 3};
        CHECK(topNode() == 2);
    }
    // nested treemap: groups weigh their leaves, children tile inside their group, a header hit reports the group
    {
        ch::Chart c;
        c.title.clear();
        c.legend = ch::LegendPos::None;
        c.animate = false;
        ch::Series& s = c.add(ch::SeriesType::Treemap, "t");
        s.labels = {"A", "B", "a1", "a2", "b1", "b2", "b3"};
        s.y = {0, 99, 6, 2, 3, 3, 2};
        s.parents = {-1, -1, 0, 0, 1, 1, 1};
        std::vector<int> px(7, 0);
        std::vector<Rect> box(7, Rect{1e9f, 1e9f, -1e9f, -1e9f}); // as x0, y0, x1, y1
        for (f32 y = 0.5f; y < B.h; y += 1)
            for (f32 x = 0.5f; x < B.w; x += 1) {
                const ch::Hit h = ch::hitTest(c, B, {x, y}, 0.f, &tf);
                if (h.series != 0 || h.index < 0) continue;
                const std::size_t i = std::size_t(h.index);
                px[i]++;
                box[i] = {std::min(box[i].x, x), std::min(box[i].y, y), std::max(box[i].w, x), std::max(box[i].h, y)};
            }
        for (int i = 0; i < 7; ++i) CHECK(px[std::size_t(i)] > 0);
        auto inside = [&](int k, int g) { return box[k].x >= box[g].x && box[k].y >= box[g].y && box[k].w <= box[g].w && box[k].h <= box[g].h; };
        CHECK(inside(2, 0) && inside(3, 0) && inside(4, 1) && inside(5, 1) && inside(6, 1));
        const f32 areaA = (box[0].w - box[0].x) * (box[0].h - box[0].y), areaB = (box[1].w - box[1].x) * (box[1].h - box[1].y);
        CHECK(std::abs(areaA / (areaA + areaB) - 0.5f) < 0.05f); // A = 8, B = 8 (B's own 99 ignored)
        CHECK(std::abs(f32(px[2]) / f32(px[2] + px[3]) - 0.75f) < 0.06f);
    }
    // gauge: value clamps to the dial, tooltip reports range
    {
        ch::Chart c = sampleChart(ch::SeriesType::Gauge);
        CHECK(ch::tooltip(c, 0, 0) == "a: 72 (0 to 100)");
        CHECK(hitAll(c, 0, 1));
        c.series[0].y = {250};
        DrawList dl;
        ch::render(c, dl, B, &tf);
        CHECK(dl.size() > 10);
    }
    // funnel: stage widths shrink with the values; every stage hit-testable
    {
        ch::Chart c = sampleChart(ch::SeriesType::Funnel);
        CHECK(hitAll(c, 0, 5));
        const ch::Layout L = ch::layout(c, B, &tf);
        auto width = [&](u32 st) {
            const f32 y = L.plot.y + (f32(st) + 0.05f) * L.plot.h / 5.f;
            int n = 0;
            for (f32 x = L.plot.x; x < L.plot.right(); x += 1) n += ch::hitTest(c, B, {x, y}, 0.f, &tf).index == i32(st);
            return n;
        };
        CHECK(width(0) > width(1) && width(1) > width(2) && width(3) > width(4));
        CHECK(std::abs(f32(width(1)) / f32(width(0)) - 0.62f) < 0.04f);
    }
}

void gateTransition() {
    // blend(): values move from the old to the new data; new points grow from the last old one (0 for bars)
    ch::Chart a = sampleChart(ch::SeriesType::Bar), b = a;
    for (f32& y : b.series[0].y) y += 10.f;
    b.series[0].x.push_back(30), b.series[0].y.push_back(50);
    ch::Chart m;
    ch::blend(a, b, 0.5f, m);
    CHECK(std::abs(m.series[0].y[3] - (a.series[0].y[3] + 5.f)) < 1e-4f);
    CHECK(std::abs(m.series[0].y[30] - 25.f) < 1e-4f);
    ch::blend(a, b, 1.f, m);
    CHECK(m.series[0].y == b.series[0].y);
    ch::Chart sa = sampleChart(ch::SeriesType::Sankey), sb = sa;
    sb.series[0].links[0].value = 50;
    ch::blend(sa, sb, 0.25f, m);
    CHECK(std::abs(m.series[0].links[0].value - 35.f) < 1e-4f);
    // widget: chartChanged(view, true) tweens to the new values; axes follow; hover still works mid-tween
    Screen s;
    UiContext& ui = s.ui;
    const NodeHandle v = ch::chartView(ui, ui.root(), "tween");
    ui.setCss(v, "width", "600px");
    ui.setCss(v, "height", "400px");
    ch::Chart& c = *ch::chartOf(ui, v);
    c = sampleChart(ch::SeriesType::Bar);
    c.animate = true;
    ch::chartChanged(ui, v, false);
    step(ui), step(ui);
    const f32 y0 = c.series[0].y[5];
    const f32 hi0 = ch::axisRanges(c)[1].second;
    for (f32& y : c.series[0].y) y *= 4.f;
    ch::chartChanged(ui, v, true);
    step(ui);
    const ch::Chart* shown = ch::chartShown(ui, v);
    CHECK(shown != &c && c.progress == 1.f); // a tween, not the entry animation
    const f32 mid = shown->series[0].y[5];
    CHECK(mid > y0 && mid < y0 * 4.f);
    const f32 hiMid = ch::axisRanges(*shown)[1].second;
    CHECK(hiMid >= hi0 && hiMid <= ch::axisRanges(c)[1].second);
    for (int i = 0; i < 30; ++i) step(ui);
    CHECK(ch::chartShown(ui, v) == &c);
    // a change during a tween continues from the on-screen values
    for (f32& y : c.series[0].y) y *= 0.25f;
    ch::chartChanged(ui, v, true);
    for (int i = 0; i < 5; ++i) step(ui);
    const f32 m1 = ch::chartShown(ui, v)->series[0].y[5];
    for (f32& y : c.series[0].y) y *= 2.f;
    ch::chartChanged(ui, v, true);
    step(ui);
    const f32 m2 = ch::chartShown(ui, v)->series[0].y[5];
    CHECK(std::abs(m2 - m1) < std::abs(m1) * 0.3f + 0.5f); // no jump back to an old snapshot
    for (int i = 0; i < 30; ++i) step(ui);
    CHECK(ch::chartShown(ui, v) == &c);
    // a structural change (new series) replays the entry animation instead
    c.add(ch::SeriesType::Line, "extra").y = {1, 2, 3};
    ch::chartChanged(ui, v, true);
    step(ui);
    CHECK(c.progress < 1.f && ch::chartShown(ui, v) == &c);
    for (int i = 0; i < 40; ++i) step(ui);
    // gauge needle tween
    c = sampleChart(ch::SeriesType::Gauge);
    c.animate = true;
    ch::chartChanged(ui, v, false);
    c.series[0].y = {20};
    ch::chartChanged(ui, v, true);
    for (int i = 0; i < 6; ++i) step(ui);
    const f32 g = ch::chartShown(ui, v)->series[0].y[0];
    CHECK(g < 72.f && g > 20.f);
    Image img;
    ch::renderImage(ui, *ch::chartShown(ui, v), 300, 300, img);
    CHECK(colours(img, {0, 0, 300, 300}) > 6);
}

void gateDemoBeyond() {
    Screen s;
    UiContext& ui = s.ui;
    const int W = 1200, H = 600, cw = W / 4, chh = H / 2;
    DrawList dl;
    const auto tf = fns(ui);
    const ch::SeriesType kinds[] = {ch::SeriesType::Radar, ch::SeriesType::Treemap, ch::SeriesType::Sankey, ch::SeriesType::Gauge,
                                    ch::SeriesType::Waterfall, ch::SeriesType::Funnel, ch::SeriesType::Contour, ch::SeriesType::Contour};
    const char* names[] = {"Radar", "Treemap", "Sankey", "Gauge", "Waterfall", "Funnel", "Contour", "Filled contour"};
    for (int i = 0; i < 8; ++i) {
        ch::Chart c = sampleChart(kinds[i]);
        c.title = names[i];
        if (kinds[i] == ch::SeriesType::Contour) {
            ch::Series& q = c.series[0];
            q.rows = q.cols = 60, q.y.clear(), q.levels = 9, q.contourFill = i == 7;
            for (u32 k = 0; k < 3600; ++k) { const f32 x = f32(k % 60) / 9.f, y = f32(k / 60) / 9.f; q.y.push_back(std::sin(x) * std::cos(y) + 0.3f * std::sin(x * y * 0.5f)); }
        }
        ch::render(c, dl, {f32(i % 4 * cw), f32(i / 4 * chh), f32(cw), f32(chh)}, &tf);
    }
    Image img;
    img.resize(W, H, 0xFF000000u);
    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    r.render(dl, img);
    if (const char* out = std::getenv("FUSE_CHARTS_DEMO_BEYOND")) CHECK(ch::writePng(img, out));
    CHECK(colours(img, {0, 0, f32(W), f32(H)}) > 30);
}

ch::Chart3D scene3d(ch::Kind3D k) {
    ch::Chart3D c;
    c.kind = k;
    if (k == ch::Kind3D::Scatter || k == ch::Kind3D::Line)
        for (int i = 0; i < 120; ++i) c.points.push_back({std::cos(f32(i) * 0.15f), f32(i) / 120.f, std::sin(f32(i) * 0.15f)});
    else {
        c.rows = k == ch::Kind3D::Area ? 4 : 12, c.cols = 12;
        for (u32 r = 0; r < c.rows; ++r)
            for (u32 q = 0; q < c.cols; ++q) c.values.push_back(1.2f + std::sin(f32(q) * 0.5f) * std::cos(f32(r) * 0.4f));
    }
    return c;
}

void gate3d() {
    Screen s;
    UiContext& ui = s.ui;
    // every kind renders through the charts API, themed
    for (u32 k = 0; k <= u32(ch::Kind3D::Area); ++k) {
        ch::Chart3D c = scene3d(ch::Kind3D(k));
        ch::applyTheme(c, ch::Theme::light());
        CHECK(c.background.a > 0 && !c.colormap.empty());
        Image img;
        ch::renderImage(ui, c, ch::Theme::light(), 320, 240, img);
        const int n = colours(img, {0, 0, 320, 240});
        if (n < 6) std::fprintf(stderr, "3d kind %u drew %d colours\n", k, n);
        CHECK(n >= 6);
        CHECK(rgb(img, 2, 2)[0] > 200); // light theme plot background
        CHECK(!ch::describe(c).empty());
    }
    // 3D axes: nice ticks over the resolved range
    ch::Chart3D sc = scene3d(ch::Kind3D::Surface);
    const auto t = dv3d_ticks(sc);
    CHECK(t.size() >= 3);
    // widget: hover, click-select, Ctrl adds, Escape clears, a11y summary
    const NodeHandle v = ch::chart3dView(ui, ui.root(), ch::Kind3D::Line, "l3");
    ui.setCss(v, "width", "400px");
    ui.setCss(v, "height", "300px");
    *ch::chart3dOf(ui, v) = scene3d(ch::Kind3D::Line);
    ch::chart3dChanged(ui, v);
    for (int i = 0; i < 2; ++i) step(ui);
    CHECK(a11yName(ui, v).rfind("3D line, 120 points", 0) == 0);
    const ch::Chart3D& lc = *ch::chart3dOf(ui, v);
    const Vec2 p60 = fuse::ui::dv3d::project(lc, ui.rect(v), lc.points[60]);
    ui.pointerMove(p60);
    step(ui);
    const i32 hv = ch::chart3dHovered(ui, v);
    CHECK(hv >= 55 && hv <= 65);
    CHECK(ch::chart3dTooltip(ui, v).size() == 3);
    int toggles = 0;
    ui.connect(v, SignalKind::Toggled, [&](UiContext&, NodeHandle, const SignalArgs&) { ++toggles; });
    ui.pointerDown(p60); ui.pointerUp(p60); step(ui);
    CHECK(ch::chart3dSelection(ui, v).size() == 1 && toggles == 1);
    const Vec2 p10 = fuse::ui::dv3d::project(lc, ui.rect(v), lc.points[10]);
    ui.setModifiers(kModCtrl);
    ui.pointerMove(p10); ui.pointerDown(p10); ui.pointerUp(p10); step(ui);
    ui.setModifiers(0);
    CHECK(ch::chart3dSelection(ui, v).size() == 2);
    ch::chart3dChanged(ui, v);
    CHECK(a11yName(ui, v).find("2 selected") != std::string::npos);
    // a drag orbits and does not select
    const f32 yaw0 = lc.camera.yaw;
    ui.pointerDown(p10); ui.pointerMove({p10.x + 50, p10.y}); ui.pointerUp({p10.x + 50, p10.y}); step(ui);
    CHECK(std::abs(lc.camera.yaw - (yaw0 + 25.f)) < 0.01f && ch::chart3dSelection(ui, v).size() == 2);
    ui.setFocus(v);
    ui.keyDown(Key::Escape); step(ui);
    CHECK(ch::chart3dSelection(ui, v).empty());
    // the selected point is drawn highlighted: rendering changes
    ch::Chart3D a = scene3d(ch::Kind3D::Scatter), b = a;
    b.selection = {40};
    Image ia, ib;
    ch::renderImage(ui, a, ch::Theme::dark(), 300, 200, ia);
    ch::renderImage(ui, b, ch::Theme::dark(), 300, 200, ib);
    CHECK(diffPixels(ia, ib) > 20);
    // export
    const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/fuse_chart3d.png";
    CHECK(ch::exportImage3d(ui, v, path, 200, 150));
    std::remove(path.c_str());
}

void gate3dLarge() {
    Screen s;
    // 1M-point scatter, 1M-point line and a 1000 x 1000 surface stay bounded in draw commands
    ch::Chart3D sc;
    sc.kind = ch::Kind3D::Scatter;
    std::mt19937 g(5);
    std::normal_distribution<f32> d(0, 1);
    for (int i = 0; i < 1'000'000; ++i) sc.points.push_back({d(g), d(g), d(g)});
    DrawList dl;
    ch::Stats3D st;
    auto t0 = std::chrono::steady_clock::now();
    ch::render(sc, dl, {0, 0, 800, 600}, ch::Theme::dark(), nullptr, &st);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("bench3d: 1M scatter -> %zu drawn, %zu cmds, %.1f ms\n", st.drawn, dl.size(), ms);
    CHECK(st.input == 1'000'000 && st.drawn < 60000 && st.drawn > 1000);
    ch::Chart3D ln;
    ln.kind = ch::Kind3D::Line;
    for (int i = 0; i < 1'000'000; ++i) { const f32 u = f32(i) * 1e-5f; ln.points.push_back({std::cos(u * 3), u / 10, std::sin(u * 3)}); }
    dl.clear();
    t0 = std::chrono::steady_clock::now();
    ch::render(ln, dl, {0, 0, 800, 600}, ch::Theme::dark(), nullptr, &st);
    const double lms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("bench3d: 1M line -> %zu drawn, %zu cmds, %.1f ms\n", st.drawn, dl.size(), lms);
    CHECK(st.drawn < 20000);
    ch::Chart3D sf;
    sf.kind = ch::Kind3D::Surface;
    sf.rows = sf.cols = 1000;
    for (u32 i = 0; i < 1'000'000; ++i) sf.values.push_back(std::sin(f32(i % 1000) * 0.01f) * std::cos(f32(i / 1000) * 0.01f));
    dl.clear();
    t0 = std::chrono::steady_clock::now();
    std::vector<fuse::ui::dv3d::ProjectedItem> items;
    ch::render(sf, dl, {0, 0, 800, 600}, ch::Theme::dark(), &items, &st);
    const double sms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("bench3d: 1000x1000 surface -> %zu drawn vertices, %zu cmds, %.1f ms\n", st.drawn, dl.size(), sms);
    CHECK(st.drawn <= 170 * 170 && st.drawn > 100 * 100);
    // picking reports original grid indices: the far corner is index rows * cols - 1
    CHECK(std::any_of(items.begin(), items.end(), [](const auto& it) { return it.index == 999'999u; }));
}

void gateDemo3d() {
    Screen s;
    UiContext& ui = s.ui;
    const int W = 1200, H = 400;
    DrawList dl;
    const ch::Theme th = ch::Theme::dark();
    dl.rect({0, 0, f32(W), f32(H)}, th.background);
    for (u32 k = 0; k <= u32(ch::Kind3D::Area); ++k) {
        ch::Chart3D c = scene3d(ch::Kind3D(k));
        if (k == u32(ch::Kind3D::Area) || k == u32(ch::Kind3D::Line)) ch::applyTheme(c, th);
        ch::render(c, dl, {f32(k) * W / 5.f, 0, W / 5.f, f32(H)}, th);
    }
    Image img;
    img.resize(W, H, 0xFF000000u);
    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    r.render(dl, img);
    if (const char* out = std::getenv("FUSE_CHARTS_DEMO3D")) CHECK(ch::writePng(img, out));
}
void gateMesh3d() {
    Screen s;
    UiContext& ui = s.ui;
    // every kind builds a lit mesh; the CPU reference rasterises it with a depth buffer
    for (u32 k = 0; k <= u32(ch::Kind3D::Volume); ++k) {
        ch::Chart3D c = k == u32(ch::Kind3D::Volume) ? ch::Chart3D{} : scene3d(ch::Kind3D(k));
        if (k == u32(ch::Kind3D::Volume)) {
            c.kind = ch::Kind3D::Volume, c.vx = 10, c.vy = 8, c.vz = 6;
            for (u32 q = 0; q < 480; ++q) c.volume.push_back(f32(q % 10) + f32(q / 80));
        }
        c.gpuMesh = true;
        Mesh3D m;
        ch::Stats3D st;
        std::vector<fuse::ui::dv3d::ProjectedItem> items;
        fuse::ui::dv3d::buildMesh(c, {0, 0, 320, 240}, m, &items, &st);
        CHECK(!m.vertices.empty() && m.indices.size() % 3 == 0 && !items.empty() && st.drawn > 0);
        CHECK(m.frame.width == 320 && m.frame.height == 240);
        for (u32 i : m.indices) CHECK(i < m.vertices.size());
        Image img;
        ch::renderImage(ui, c, ch::Theme::dark(), 320, 240, img);
        const int n = colours(img, {0, 0, 320, 240});
        if (n < 12) std::fprintf(stderr, "mesh kind %u drew %d colours\n", k, n);
        CHECK(n >= 12);
        // the painter path draws the same kinds (volume slices included)
        c.gpuMesh = false;
        Image img2;
        ch::renderImage(ui, c, ch::Theme::dark(), 320, 240, img2);
        CHECK(colours(img2, {0, 0, 320, 240}) >= 6);
    }
    // the mesh projection matches the painter's: a scatter point's octahedron centre lands on project()
    {
        ch::Chart3D c = scene3d(ch::Kind3D::Scatter);
        c.gpuMesh = true;
        Mesh3D m;
        fuse::ui::dv3d::buildMesh(c, {10, 20, 300, 200}, m);
        const f32* M = m.frame.mvp;
        for (std::size_t p = 0; p < c.points.size(); p += 17) {
            f32 sx = 0, sy = 0;
            for (int j = 0; j < 6; ++j) { // octahedron vertices average to the centre
                const MeshVertex& v = m.vertices[p * 6 + std::size_t(j)];
                sx += v.x, sy += v.y;
            }
            (void)sx, (void)sy;
            const MeshVertex& a = m.vertices[p * 6], &b = m.vertices[p * 6 + 1];
            const f32 cx = (a.x + b.x) * 0.5f, cy = (a.y + b.y) * 0.5f, cz = (a.z + b.z) * 0.5f;
            const f32 X = M[0] * cx + M[4] * cy + M[8] * cz + M[12], Y = M[1] * cx + M[5] * cy + M[9] * cz + M[13];
            const f32 Wc = M[3] * cx + M[7] * cy + M[11] * cz + M[15];
            const Rect mr = fuse::ui::dv3d::meshRect({10, 20, 300, 200});
            const Vec2 got{mr.x + (X / Wc * 0.5f + 0.5f) * mr.w, mr.y + (Y / Wc * 0.5f + 0.5f) * mr.h};
            const Vec2 want = fuse::ui::dv3d::project(c, {10, 20, 300, 200}, c.points[p]);
            CHECK(std::abs(got.x - want.x) < 0.05f && std::abs(got.y - want.y) < 0.05f);
        }
    }
    // depth buffer: a near triangle hides a far one regardless of submission order
    {
        Mesh3D m;
        m.frame.width = m.frame.height = 64;
        auto add = [&](f32 z, u32 rgba) {
            const u32 o = m.add({-0.8f, -0.8f, z, 0, 0, 1, rgba});
            m.add({0.8f, -0.8f, z, 0, 0, 1, rgba});
            m.add({0.f, 0.8f, z, 0, 0, 1, rgba});
            m.tri(o, o + 1, o + 2);
        };
        m.frame.light[3] = 1.f; // unlit
        add(0.2f, 0xFF0000FFu); // near: red
        add(0.7f, 0xFF00FF00u); // far: green, drawn later
        Image img;
        rasterMesh(m, img);
        CHECK(img.pixels[32 * 64 + 32] == 0xFF0000FFu);
        CHECK(img.pixels[2] == 0u); // outside: clear
        // shared edge: two triangles of a quad cover each pixel exactly once (top-left rule, no gaps / doubles)
        Mesh3D q;
        q.frame.width = q.frame.height = 16, q.frame.light[3] = 1.f;
        const u32 o = q.add({-1, -1, 0.5f, 0, 0, 1, 0xFFFFFFFFu});
        q.add({1, -1, 0.5f, 0, 0, 1, 0xFFFFFFFFu});
        q.add({1, 1, 0.5f, 0, 0, 1, 0xFFFFFFFFu});
        q.add({-1, 1, 0.5f, 0, 0, 1, 0xFFFFFFFFu});
        q.quad(o, o + 1, o + 2, o + 3);
        rasterMesh(q, img);
        for (u32 px : img.pixels) CHECK(px == 0xFFFFFFFFu);
        // equal depths: the first-drawn triangle keeps the pixel (LESS), and the id buffer names it
        Mesh3D t;
        t.frame.width = t.frame.height = 32, t.frame.light[3] = 1.f;
        for (u32 k = 0; k < 2; ++k) {
            MeshVertex a{-0.9f, -0.9f, 0.4f, 0, 0, 1, k ? 0xFF00FF00u : 0xFF0000FFu}, b = a, cc = a;
            b.x = 0.9f, cc.x = 0.f, cc.y = 0.9f;
            a.id = b.id = cc.id = 10 + k;
            const u32 o = t.add(a);
            t.add(b), t.add(cc);
            t.tri(o, o + 1, o + 2);
        }
        std::vector<u32> ids;
        rasterMesh(t, img, nullptr, &ids);
        CHECK(img.pixels[16 * 32 + 16] == 0xFF0000FFu && ids[16 * 32 + 16] == 10u && ids[0] == 0u);
    }
    // widget on the mesh path: attached image, picking and hover still work
    const NodeHandle v = ch::chart3dView(ui, ui.root(), ch::Kind3D::Surface, "m3");
    ui.setCss(v, "width", "400px");
    ui.setCss(v, "height", "300px");
    *ch::chart3dOf(ui, v) = scene3d(ch::Kind3D::Surface);
    CHECK(fuse::ui::widgets::dataViz3dMeshImage(ui, v) != nullptr);
    fuse::ui::widgets::dataViz3dSetMeshImage(ui, v, 0);
    ch::chart3dChanged(ui, v);
    for (int i = 0; i < 2; ++i) step(ui);
    const Mesh3D* wm = fuse::ui::widgets::dataViz3dMesh(ui, v);
    CHECK(wm && wm->vertices.size() == 144);
    const Image* wi = fuse::ui::widgets::dataViz3dMeshImage(ui, v);
    CHECK(wi->width == 400 && wi->height == 300 && colours(*wi, {0, 0, 400, 300}) >= 12);
    const ch::Chart3D& sc = *ch::chart3dOf(ui, v);
    ui.pointerMove(fuse::ui::dv3d::project(sc, ui.rect(v), {5.f, sc.values[5 * 12 + 5], 5.f}));
    step(ui);
    CHECK(ch::chart3dHovered(ui, v) >= 0);
    // GPU-style picking: the id buffer under the pointer (CPU twin of UiVkMesh3D::requestPick) drives hover
    {
        std::vector<u32> ids;
        Image tmp;
        rasterMesh(*wm, tmp, nullptr, &ids);
        u32 px = 0, py = 0;
        CHECK(fuse::ui::widgets::dataViz3dMeshPickPixel(ui, v, px, py));
        const u32 id = ids[std::size_t(py) * wm->frame.width + px];
        CHECK(id > 0 && id <= 144);
        fuse::ui::widgets::dataViz3dSetMeshPick(ui, v, id);
        step(ui);
        CHECK(ch::chart3dHovered(ui, v) == i32(id - 1));
        fuse::ui::widgets::dataViz3dSetMeshPick(ui, v, 0);
        step(ui);
        CHECK(ch::chart3dHovered(ui, v) == -1);
    }
    // 3D transitions: a data change tweens from the shown values; a kind change snaps
    {
        ch::Chart3D& c3 = *ch::chart3dOf(ui, v);
        const f32 before = c3.values[40];
        for (f32& y : c3.values) y = y * 3.f + 1.f;
        ch::chart3dChanged(ui, v, true);
        step(ui);
        const ch::Chart3D* sh = ch::chart3dShown(ui, v);
        CHECK(sh != &c3);
        CHECK(sh->values[40] > before && sh->values[40] < c3.values[40]);
        for (int i = 0; i < 30; ++i) step(ui);
        CHECK(ch::chart3dShown(ui, v) == &c3);
        ch::Chart3D a3 = c3, m3;
        a3.values.assign(a3.values.size(), 0.f);
        ch::blend(a3, c3, 0.5f, m3);
        CHECK(std::abs(m3.values[7] - c3.values[7] * 0.5f) < 1e-5f);
        c3.kind = ch::Kind3D::Bars;
        ch::chart3dChanged(ui, v, true);
        step(ui);
        CHECK(ch::chart3dShown(ui, v) == &c3);
    }
    // volume summary: voxel grid, the field's value range and the visible slices (no spatial y "values")
    {
        ch::Chart3D vol;
        vol.kind = ch::Kind3D::Volume;
        vol.vx = vol.vy = vol.vz = 4;
        for (u32 q = 0; q < 64; ++q) vol.volume.push_back(f32(q) * 0.5f);
        vol.slice[1] = -1, vol.slice[2] = 3;
        const std::string d = ch::describe(vol);
        CHECK(d == "3D volume slices, 4 x 4 x 4 voxels, values 0 to 31.5, x slice 2, z slice 3");
        if (d != "3D volume slices, 4 x 4 x 4 voxels, values 0 to 31.5, x slice 2, z slice 3") std::fprintf(stderr, "%s\n", d.c_str());
    }
}

void gateMesh3dLarge() {
    // 1000 x 1000 surface and 1M-point scatter on the mesh path: build + CPU raster time, bounded mesh size
    ch::Chart3D sf;
    sf.kind = ch::Kind3D::Surface, sf.gpuMesh = true;
    sf.rows = sf.cols = 1000;
    for (u32 i = 0; i < 1'000'000; ++i) sf.values.push_back(std::sin(f32(i % 1000) * 0.01f) * std::cos(f32(i / 1000) * 0.01f));
    Mesh3D m;
    ch::Stats3D st;
    auto t0 = std::chrono::steady_clock::now();
    fuse::ui::dv3d::buildMesh(sf, {0, 0, 800, 600}, m, nullptr, &st);
    const double bms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Image img;
    t0 = std::chrono::steady_clock::now();
    rasterMesh(m, img);
    const double rms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("bench mesh3d: 1000x1000 surface -> %zu vertices, %zu triangles, build %.1f ms, CPU raster %.1f ms\n",
                m.vertices.size(), m.indices.size() / 3, bms, rms);
    // screen-space LOD: no finer than ~2 px per grid step on the 800 px side
    CHECK(m.vertices.size() >= 300u * 300u && m.vertices.size() <= 402u * 402u && st.input == 1'000'000);
    ch::Chart3D sc;
    sc.kind = ch::Kind3D::Scatter, sc.gpuMesh = true, sc.pointSize = 2.f;
    std::mt19937 g(9);
    std::normal_distribution<f32> d(0, 1);
    for (int i = 0; i < 1'000'000; ++i) sc.points.push_back({d(g), d(g), d(g)});
    sc.selection = {7};
    t0 = std::chrono::steady_clock::now();
    fuse::ui::dv3d::buildMesh(sc, {0, 0, 800, 600}, m, nullptr, &st);
    const double sms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    t0 = std::chrono::steady_clock::now();
    rasterMesh(m, img);
    const double srm = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("bench mesh3d: 1M scatter -> %zu points meshed, %zu triangles, build %.1f ms, CPU raster %.1f ms\n",
                st.drawn, m.indices.size() / 3, sms, srm);
    // one point per marker-sized screen cell (1.5 px), the selection always kept
    CHECK(st.drawn >= 10000 && st.drawn <= 800u * 600u && m.vertices.size() <= (1u << 21));
    bool kept = false;
    for (const MeshVertex& mv : m.vertices) kept |= mv.id == 8;
    CHECK(kept);
}
} // namespace

int main(int argc, char** argv) {
    std::string gate = argc > 1 && std::strncmp(argv[1], "--gate=", 7) == 0 ? argv[1] + 7 : "all";
    struct G { const char* n; void (*f)(); } gates[] = {{"series", gateSeries}, {"axes", gateAxes}, {"decimation", gateDecimation}, {"ring", gateRing},
        {"hit", gateHit}, {"widget", gateWidget}, {"model", gateModel}, {"export", gateExport}, {"bench", gateBench}, {"demo", gateDemo},
        {"3d", gate3d}, {"3d_large", gate3dLarge}, {"demo3d", gateDemo3d},
        {"mesh3d", gateMesh3d}, {"mesh3d_large", gateMesh3dLarge},
        {"beyond", gateBeyondQt}, {"transition", gateTransition}, {"demo_beyond", gateDemoBeyond}};
    for (const G& g : gates) if (gate == "all" || gate == g.n) g.f();
    return fuse::ui::test::finish(("charts " + gate).c_str());
}
