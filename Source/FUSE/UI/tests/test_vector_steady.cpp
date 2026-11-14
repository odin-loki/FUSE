// Gate fuse_ui_vector_steady: the vector ops in steady state.
//   * zero heap allocations: a tree whose widgets paint with a Painter (paths, gradients, dashed strokes, a path clip,
//     an opacity layer, icons, an SVG image, buttons with icons) repaints and is rasterised by the CPU reference
//     rasteriser without a single allocation once warm (global operator new is counted)
//   * the classic path is untouched: a tree without vector ops records none (DrawList::hasVectorOps() stays false)
//   * rasterisation budget (optimised builds): a typical icon (a few hundred flattened segments) at 24 px costs well
//     under 0.1 ms from the coverage cache and stays inside a sane bound when rasterised from scratch, so icons do
//     not put the 10k-node frame budget at risk
#include "fuse/ui/painter.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <new>
#include <vector>

namespace {
std::atomic<long> g_allocs{0};
std::atomic<bool> g_counting{false};
} // namespace

void* operator new(std::size_t n) {
    if (g_counting) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
constexpr f32 kPi = 3.14159265358979f;

// A widget that uses most of the Painter each time it paints; its value animates the geometry.
struct Canvas {
    static constexpr std::string_view kName = "test-canvas";
    static Size measure(UiContext&, NodeHandle, Size) { return {160.f, 100.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const f32 v = pc.ui.value(pc.node);
        static const GradientStop stops[3]{{0.f, Color::rgb(0xFF4400)}, {0.5f, Color::rgb(0x22CC88)}, {1.f, Color::rgb(0x2244FF)}};
        Painter p(pc, dl);
        p.translate(pc.content.x, pc.content.y);
        p.setBrush(Gradient::linear({0, 0}, {160, 0}, stops));
        p.drawRoundedRect({0, 0, 160, 100}, {12, 4, 12, 4});
        p.save();
        p.translate(50, 50);
        p.rotate(v * kPi);
        StrokeStyle st;
        st.width = 3.f;
        st.join = LineJoin::Round;
        const f32 dash[2]{6.f, 3.f};
        st.setDashes(dash, v * 9.f);
        p.setPen(Color::rgb(0xFFFFFF), st);
        p.setBrush(Gradient::radial({0, 0}, 30.f, stops));
        p.drawEllipse({0, 0}, 30.f, 18.f + 6.f * v);
        p.restore();
        p.save();
        thread_local Path clip; // widgets keep their own scratch paths
        clip.clear();
        clip.addCircle({120, 50}, 24.f);
        p.clipPath(clip);
        p.setNoPen();
        p.setBrush(Color::rgb(0x101010));
        p.drawRect({90, 20, 60, 60});
        p.beginLayer(0.7f);
        p.setPen(Color::rgb(0xFFFF00), 2.f);
        const Vec2 zig[5]{{96, 70}, {106, 30 + 20 * v}, {120, 62}, {134, 34}, {146, 66}};
        p.drawPolyline(zig);
        p.restore();
        drawIcon(p, Icon::Settings, {4, 4, 24, 24}, Color::rgb(0xFFFFFF));
        drawIcon(p, Icon::Folder, {4, 72, 24, 24}, Color::rgb(0xFFFFFF));
        p.drawText("vector", {60, 80, 60, 16}, Color::rgb(0xFFFFFF), 12.f, TextAlignment::Center);
    }
};

double medianMs(std::vector<double>& t) {
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}
} // namespace

int main() {
    // ---- zero allocations in steady state ----
    {
        UiContext ui({.viewport = {360, 260}, .font = &font()});
        w::install(ui);
        ui.registry().registerWidget<Canvas>();
        ui.setCss(ui.root(), "padding", "8px");
        ui.setCss(ui.root(), "gap", "6px");
        ui.setCss(ui.root(), "align-items", "start");
        const NodeHandle canvas = *ui.create("test-canvas", ui.root(), "canvas");
        ui.setRange(canvas, 0.f, 1.f);
        const NodeHandle row = w::panel(ui, ui.root(), "row");
        ui.addClass(row, "row");
        const NodeHandle save = w::button(ui, row, "Save", "save");
        w::setIcon(ui, save, Icon::Save);
        w::iconButton(ui, row, Icon::Undo, {}, "undo");
        w::iconButton(ui, row, Icon::Redo, "Redo", "redo");
        w::svgImage(ui, row, w::iconHandle(Icon::Warning), 32.f, 32.f, "warn");
        const NodeHandle label = w::label(ui, ui.root(), "frame 0", "label");
        Image target;
        target.resize(360, 260, 0xFF000000u);
        CpuRasterizer raster;
        raster.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        auto frame = [&](int f) {
            ui.setValue(canvas, f32(f % 20) / 20.f);
            ui.pointerMove({f32(20 + f % 7), f32(130 + f % 3)}); // hover changes on the icon buttons
            ui.update(1.f / 60.f);
            raster.render(ui.drawList(), target, ui.damage());
        };
        for (int f = 0; f < 60; ++f) frame(f); // warm-up: buffers, glyph atlas, coverage cache
        UI_CHECK(ui.drawList().hasVectorOps());
        UI_CHECK(ui.drawList().paths().size() >= 15);
        g_allocs = 0;
        g_counting = true;
        for (int f = 60; f < 110; ++f) frame(f);
        g_counting = false;
        std::printf("allocations in 50 steady frames with vector ops (update + CPU raster): %ld; paths per frame %zu\n",
                    g_allocs.load(), ui.drawList().paths().size());
        UI_CHECK_NO_ALLOC(g_allocs.load());
        (void)label;
        (void)save;
    }
    // ---- a tree without vector ops stays on the classic path ----
    {
        UiContext ui({.viewport = {488, 400}, .font = &font()});
        w::install(ui);
        buildSample(ui);
        ui.update();
        UI_CHECK(!ui.drawList().hasVectorOps());
        UI_CHECK(ui.drawList().paths().empty() && ui.drawList().patches().empty() && ui.drawList().pathPoints().empty());
    }
    // ---- icon rasterisation budget ----
    {
        // the busiest built-in icon, and a synthetic one with a few hundred flattened segments
        const SvgImage& gear = iconImage(Icon::Settings);
        Path flower;
        for (int i = 0; i < 12; ++i) {
            const f32 a0 = f32(i) * kPi / 6.f, a1 = a0 + kPi / 12.f, a2 = a0 + kPi / 6.f;
            const Vec2 p0{12 + 5 * std::cos(a0), 12 + 5 * std::sin(a0)}, c{12 + 14 * std::cos(a1), 12 + 14 * std::sin(a1)},
                p1{12 + 5 * std::cos(a2), 12 + 5 * std::sin(a2)}; // petals reach r = 9.5: inside the 24 px box
            if (i == 0) flower.moveTo(p0);
            flower.quadTo(c, p1);
        }
        flower.close();
        flower.addCircle({12, 12}, 3.f);
        Polyline flat;
        flattenPath(flower, 0.1f, flat);
        StrokeStyle st;
        st.width = 1.5f;
        st.join = LineJoin::Round;
        Polyline outline;
        strokePolyline(flat, st, 0.1f, outline);
        std::printf("synthetic icon: %zu flattened segments, %zu stroke outline segments; gear: %u path segments\n",
                    flat.segmentCount(), outline.segmentCount(), gear.segmentCount());
        UI_CHECK(outline.segmentCount() >= 300);

        DrawList gearList, flowerList;
        {
            Painter p(gearList);
            gear.render(p, {8, 8, 24, 24});
            Painter q(flowerList);
            q.translate(8, 8);
            q.setBrush(Color::rgb(0x3D8BFD));
            q.setPen(Color::rgb(0xFFFFFF), st);
            q.drawPath(flower);
        }
        Image target;
        target.resize(40, 40, 0); // the icon at (8, 8), as inside a window
        CpuRasterizer raster;
        auto bench = [&](const DrawList& dl, bool cached) {
            std::vector<double> t;
            raster.render(dl, target);
            for (int i = 0; i < 400; ++i) {
                if (!cached) clearPathCache();
                const auto t0 = std::chrono::steady_clock::now();
                raster.render(dl, target);
                const auto t1 = std::chrono::steady_clock::now();
                t.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            return medianMs(t);
        };
        const double gearCold = bench(gearList, false), flowerCold = bench(flowerList, false);
        clearPathCache();
        const double gearWarm = bench(gearList, true), flowerWarm = bench(flowerList, true);
        const PathCacheStats cs = pathCacheStats();
        UI_CHECK_EQ(cs.uncached, u64(0)); // both icons fit the cache
        std::printf("24 px icon raster, median: gear %.4f ms uncached / %.4f ms cached; synthetic %.4f ms uncached / %.4f ms cached\n",
                    gearCold, gearWarm, flowerCold, flowerWarm);
        UI_CHECK(cs.hits >= 400);
        UI_CHECK(gearWarm < gearCold && flowerWarm < flowerCold); // the cache pays
#ifdef NDEBUG
        UI_CHECK(gearWarm < 0.02);   // "well under 0.1 ms"
        UI_CHECK(flowerWarm < 0.02);
        UI_CHECK(gearCold < 0.5);    // from scratch: flatten + stroke + coverage
        UI_CHECK(flowerCold < 0.5);
#else
        std::printf("(debug build: budget not enforced)\n");
#endif
        // 1000 icon instances in one frame (a large tool palette / tree of items): all but the first are cache hits
        DrawList many;
        {
            Painter p(many);
            for (int i = 0; i < 1000; ++i) drawIcon(p, Icon(i % int(kIconCount)), {f32((i % 40) * 26), f32((i / 40) * 26), 24, 24}, Color::rgb(0xFFFFFF));
        }
        Image big;
        big.resize(1040, 650, 0);
        raster.render(many, big);
        std::vector<double> t;
        for (int i = 0; i < 20; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            raster.render(many, big);
            const auto t1 = std::chrono::steady_clock::now();
            t.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        const double perIcon = medianMs(t) / 1000.0;
        std::printf("1000 icons: %.4f ms per icon (%zu path commands)\n", perIcon, many.paths().size());
#ifdef NDEBUG
        UI_CHECK(perIcon < 0.02);
#endif
    }
    // ---- large paths: cost follows the covered spans, not the bounding box ----
    {
        Image target;
        target.resize(1920, 1080, 0xFF000000u);
        CpuRasterizer raster;
        DrawList diag, chart;
        {
            Painter p(diag);
            p.setPen(Color::rgb(0xFFFFFF), 1.5f);
            p.drawLine({4, 4}, {1916, 1076}); // bounding box = the whole frame
            Painter q(chart);
            std::vector<Vec2> pts;
            for (int i = 0; i < 2000; ++i) pts.push_back({f32(i) * 0.96f, 540.f + 400.f * std::sin(f32(i) * 0.021f) * std::cos(f32(i) * 0.0037f)});
            StrokeStyle st;
            st.width = 2.f;
            st.join = LineJoin::Round;
            q.setPen(Color::rgb(0x3D8BFD), st);
            q.drawPolyline(pts); // a 2000-point series as ONE stroked path (smooth joins, no overlap blobs)
        }
        auto time = [&](const DrawList& dl) {
            std::vector<double> t;
            raster.render(dl, target);
            for (int i = 0; i < 15; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                raster.render(dl, target);
                const auto t1 = std::chrono::steady_clock::now();
                t.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            return medianMs(t);
        };
        const double tDiag = time(diag), tChart = time(chart);
        std::printf("1920x1080: full-frame diagonal %.3f ms, 2000-point series %.3f ms\n", tDiag, tChart);
#ifdef NDEBUG
        UI_CHECK(tDiag < 3.0);
        UI_CHECK(tChart < 8.0);
#endif
    }
    return finish("fuse_ui_vector_steady");
}
