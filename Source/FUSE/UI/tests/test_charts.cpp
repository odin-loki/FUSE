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
    for (u32 t = 0; t <= u32(ch::SeriesType::Bubble); ++t) {
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
} // namespace

int main(int argc, char** argv) {
    std::string gate = argc > 1 && std::strncmp(argv[1], "--gate=", 7) == 0 ? argv[1] + 7 : "all";
    struct G { const char* n; void (*f)(); } gates[] = {{"series", gateSeries}, {"axes", gateAxes}, {"decimation", gateDecimation}, {"ring", gateRing},
        {"hit", gateHit}, {"widget", gateWidget}, {"model", gateModel}, {"export", gateExport}, {"bench", gateBench}, {"demo", gateDemo}};
    for (const G& g : gates) if (gate == "all" || gate == g.n) g.f();
    return fuse::ui::test::finish(("charts " + gate).c_str());
}
