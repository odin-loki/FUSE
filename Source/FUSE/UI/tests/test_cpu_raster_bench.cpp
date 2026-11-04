// Gate fuse_ui_cpu_raster_bench: speed and exactness of the CPU reference rasteriser.
//   * exact: every scene, a fuzzed mix of all primitives (rounded clips, gradients, borders, shadows, lines, glyphs in
//     all modes, images, backdrops, effects) and scissored (incremental) renders give the same bytes as the frozen
//     pre-optimisation copy (cpu_raster_reference.hpp), single-threaded and banded over threads
//   * speed (median of 10 after a warm-up): 1080p "8 cards" full frame <= 20 ms, a small-change incremental frame
//     (one button's damage) <= 2 ms; the sample and the 1440p HUD are reported
// --report (or FUSE_UI_PERF_REPORT_ONLY=1, for slow / software CI): print the times, do not fail on them.
// --ops: also time each op kind alone (where the frame time goes).
#include "fuse/ui/effects.hpp"
#include "fuse/ui/game/game_ui.hpp"
#include "cpu_raster_reference.hpp"
#include "test_util.hpp"

#include "../Game/samples/hud_scene.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
void appendShifted(DrawList& out, const DrawList& src, f32 dx, f32 dy) {
    for (DrawCmd c : src.cmds()) {
        c.rect.x += dx;
        c.rect.y += dy;
        if (c.op == DrawOp::Line) {
            c.rect.w += dx;
            c.rect.h += dy;
        }
        out.push(c);
    }
}

struct Scene {
    const char* name;
    int w, h;
    const DrawList* list;
    const u8* atlas;
    int atlasSize;
};

struct Times {
    double min = 0.0, median = 0.0;
};
template <class F> Times timeIt(F&& f, int runs = 10) {
    f(); // warm-up (thread pool, scratch buffers)
    std::vector<double> ms;
    for (int i = 0; i < runs; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    return {ms.front(), ms[ms.size() / 2]};
}

bool same(const Image& a, const Image& b, const char* what) {
    if (a.width != b.width || a.height != b.height) {
        std::fprintf(stderr, "  %s: size differs\n", what);
        return false;
    }
    for (std::size_t i = 0; i < a.pixels.size(); ++i)
        if (a.pixels[i] != b.pixels[i]) {
            std::fprintf(stderr, "  %s: first difference at (%d, %d): %08x vs reference %08x\n", what,
                         int(i % std::size_t(a.width)), int(i / std::size_t(a.width)), a.pixels[i], b.pixels[i]);
            return false;
        }
    return true;
}

// a random mix of every primitive, with nested (rounded) clips; `atlasSize` > 0 adds glyphs of all modes
DrawList fuzzList(u32 seed, int w, int h, int atlasSize, u32 images, bool effects) {
    std::mt19937 rng(seed);
    auto uf = [&](f32 a, f32 b) { return std::uniform_real_distribution<f32>(a, b)(rng); };
    auto ui = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); };
    auto col = [&] {
        const f32 a = ui(0, 3) == 0 ? 1.f : uf(0.f, 1.f);
        return Color{uf(0.f, 1.f), uf(0.f, 1.f), uf(0.f, 1.f), a};
    };
    auto rect = [&] {
        const f32 x = uf(-40.f, f32(w)), y = uf(-40.f, f32(h));
        // integer, half and arbitrary edges
        const int snap = ui(0, 2);
        Rect r{x, y, uf(1.f, f32(w) * 0.6f), uf(1.f, f32(h) * 0.6f)};
        if (snap == 0) r = {std::floor(r.x), std::floor(r.y), std::floor(r.w) + 1.f, std::floor(r.h) + 1.f};
        if (snap == 1) r = {std::floor(r.x) + 0.5f, std::floor(r.y) + 0.5f, std::floor(r.w) + 1.f, std::floor(r.h)};
        return r;
    };
    DrawList dl;
    dl.gradientRect({0, 0, f32(w), f32(h)}, Color::rgb(0x203040), Color::rgb(0x101018), true);
    int depth = 0;
    for (int i = 0; i < 260; ++i) {
        const int k = ui(0, 21);
        if (k <= 5) {
            const f32 rr = ui(0, 2) == 0 ? 0.f : uf(0.f, 30.f);
            std::array<f32, 4> radii{rr, rr, rr, rr};
            if (ui(0, 3) == 0) radii = {uf(0.f, 20.f), uf(0.f, 20.f), uf(0.f, 20.f), uf(0.f, 20.f)};
            if (ui(0, 4) == 0) radii = {uf(0.f, 300.f), uf(0.f, 300.f), uf(0.f, 300.f), uf(0.f, 300.f)}; // pills, circles
            const bool border = ui(0, 2) == 0;
            dl.roundedRect(rect(), col(), radii, border ? col() : Color{}, border ? uf(0.5f, 6.f) : 0.f);
        } else if (k <= 7) {
            const f32 rr = ui(0, 1) ? uf(0.f, 16.f) : 0.f;
            dl.gradientRect(rect(), col(), col(), ui(0, 1) == 0, {rr, rr, rr, rr});
        } else if (k <= 9) {
            dl.shadow(rect(), Color{0.f, 0.f, 0.f, uf(0.1f, 0.8f)}, uf(0.f, 24.f), ui(0, 3) ? uf(0.f, 16.f) : uf(0.f, 200.f));
        } else if (k == 10) {
            dl.line({uf(0.f, f32(w)), uf(0.f, f32(h))}, {uf(0.f, f32(w)), uf(0.f, f32(h))}, col(), uf(0.5f, 5.f));
        } else if (k <= 13 && atlasSize > 0) {
            DrawCmd c;
            c.op = DrawOp::Glyph;
            c.gradient = u8(ui(0, 4) == 0 ? ui(1, 2) : 0);
            const int page = c.gradient ? atlasSize / 2 : atlasSize;
            c.tw = u16(ui(1, 40));
            c.th = u16(ui(1, 40));
            c.u = u16(ui(0, page - c.tw));
            c.v = u16(ui(0, page - c.th));
            const f32 s = c.gradient == kGlyphMsdf ? uf(0.5f, 3.f) : 1.f;
            c.rect = {std::floor(uf(-10.f, f32(w))) + (ui(0, 3) == 0 ? 0.3f : 0.f), std::floor(uf(-10.f, f32(h))),
                      f32(c.tw) * s, f32(c.th) * s};
            c.fill = col();
            c.blur = uf(1.f, 6.f);
            dl.push(c);
        } else if (k <= 15 && images > 0) {
            DrawCmd c;
            c.op = DrawOp::Image;
            c.image = u32(ui(0, int(images) - 1));
            c.rect = rect();
            c.fill = ui(0, 1) ? Color{} : Color{1.f, 1.f, 1.f, uf(0.f, 1.f)};
            dl.push(c);
        } else if (k == 16 && depth < 6) {
            dl.pushClip(rect(), ui(0, 1) ? uf(0.f, ui(0, 2) ? 20.f : 150.f) : 0.f);
            ++depth;
        } else if (k == 17 && depth > 0) {
            dl.popClip();
            --depth;
        } else if (k == 18 && ui(0, 2) == 0) {
            const f32 rr = uf(0.f, 16.f);
            dl.backdrop(rect(), uf(1.f, 14.f), {rr, rr, rr, rr}, ui(0, 1) ? Color{1.f, 1.f, 1.f, uf(0.f, 0.4f)} : Color{});
        } else if (k == 19 && effects) {
            const u32 id = u32(ui(0, 2)); // desaturate / ripple / grain
            const f32 p[3] = {uf(0.f, 1.f), uf(4.f, 30.f), uf(1.f, 40.f)};
            dl.effect(rect(), id, p, uf(0.f, 3.f), id == 1 ? 8.f : 0.f);
        } else {
            dl.rect(rect(), col(), ui(0, 1) ? uf(0.f, 8.f) : 0.f);
        }
    }
    while (depth-- > 0) dl.popClip();
    return dl;
}

bool envOn(const char* n) {
    const char* v = std::getenv(n);
    return v && *v && std::strcmp(v, "0") != 0;
}
} // namespace

int main(int argc, char** argv) {
    bool report = envOn("FUSE_UI_PERF_REPORT_ONLY"), ops = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--report") == 0) report = true;
        if (std::strcmp(argv[i], "--ops") == 0) ops = true;
    }

    // ---- scenes (the fuse_ui_baselines frames) ----
    UiContext sample({.viewport = {488.f, 400.f}, .font = &font()});
    widgets::install(sample);
    buildSample(sample);
    sample.update();
    UiContext card({.viewport = {470.f, 520.f}, .font = &font()});
    widgets::install(card);
    buildSample(card);
    card.update();
    DrawList cards;
    cards.gradientRect({0, 0, 1920.f, 1080.f}, Color::rgb(0x202838), Color::rgb(0x101018), true);
    for (int i = 0; i < 8; ++i) appendShifted(cards, card.drawList(), f32(i % 4) * 480.f, f32(i / 4) * 540.f);
    UiContext hud({.viewport = game::demo::kHudViewport, .font = &font()});
    widgets::install(hud);
    game::install(hud);
    auto hudScene = game::demo::buildHudScene(hud);
    hud.setScale(2560.f / game::demo::kHudViewport.w);
    hud.update();
    const Scene scenes[] = {
        {"sample", 488, 400, &sample.drawList(), sample.atlas().pixels(), sample.atlas().size()},
        {"cards-1080p", 1920, 1080, &cards, card.atlas().pixels(), card.atlas().size()},
        {"hud-1440p", 2560, 1440, &hud.drawList(), hud.atlas().pixels(), hud.atlas().size()},
    };

    // ---- exactness against the frozen reference ----
    EffectRegistry fx;
    fx.addBuiltins();
    Image imgs[2];
    imgs[0].resize(37, 23);
    imgs[1].resize(300, 200);
    {
        std::mt19937 rng(7);
        for (Image& im : imgs)
            for (u32& p : im.pixels) {
                const u32 a = (rng() & 1) ? 255u : rng() & 0xFFu;
                auto c = [&] { return u32(rng() % (a + 1)); };
                p = c() | (c() << 8) | (c() << 16) | (a << 24);
            }
    }
    auto check = [&](const char* what, const DrawList& dl, int w, int h, const u8* atlas, int atlasSize, Rect scissor,
                     bool withImages) {
        Image base;
        base.resize(w, h, 0xFF000000u);
        if (!scissor.empty()) { // incremental: render over a previous frame
            ref::Rasterizer r0;
            r0.m_atlas = atlas;
            r0.m_atlasSize = atlasSize;
            r0.m_effects = &fx;
            if (withImages) r0.m_images = {&imgs[0], &imgs[1]};
            r0.render(dl, base);
        }
        Image want = base;
        ref::Rasterizer rr;
        rr.m_atlas = atlas;
        rr.m_atlasSize = atlasSize;
        rr.m_effects = &fx;
        if (withImages) rr.m_images = {&imgs[0], &imgs[1]};
        rr.render(dl, want, scissor);
        for (int threads : {1, 0, 3}) {
            CpuRasterizer r;
            r.setGlyphAtlas(atlas, atlasSize);
            r.setEffects(&fx);
            r.setThreads(threads);
            if (withImages) {
                r.addImage(&imgs[0]);
                r.addImage(&imgs[1]);
            }
            Image got = base;
            r.render(dl, got, scissor);
            char label[160];
            std::snprintf(label, sizeof label, "%s (threads %d)", what, threads);
            UI_CHECK(same(got, want, label));
        }
    };
    for (const Scene& s : scenes) {
        check(s.name, *s.list, s.w, s.h, s.atlas, s.atlasSize, {}, false);
        check(s.name, *s.list, s.w, s.h, s.atlas, s.atlasSize, {f32(s.w) * 0.3f, f32(s.h) * 0.2f, 157.f, 61.f}, false);
    }
    for (u32 seed = 1; seed <= 48; ++seed) {
        const int w = seed % 3 == 0 ? 701 : 512, h = seed % 3 == 0 ? 333 : 384;
        const DrawList dl = fuzzList(seed, w, h, sample.atlas().size(), 2, true);
        char what[64];
        std::snprintf(what, sizeof what, "fuzz %u", seed);
        check(what, dl, w, h, sample.atlas().pixels(), sample.atlas().size(), {}, true);
        const Rect sc{f32(seed * 7 % 200), f32(seed * 13 % 150), f32(40 + seed * 11 % 300), f32(30 + seed * 5 % 200)};
        check(what, dl, w, h, sample.atlas().pixels(), sample.atlas().size(), sc, true);
    }

    // ---- speed ----
    std::printf("fuse_ui_cpu_raster_bench (%u hardware threads)%s\n", std::thread::hardware_concurrency(),
                report ? " [report only]" : "");
    double cardsMs = 0.0, incrMs = 0.0;
    for (const Scene& s : scenes) {
        CpuRasterizer r;
        r.setGlyphAtlas(s.atlas, s.atlasSize);
        Image img;
        img.resize(s.w, s.h, 0xFF000000u);
        const Times full = timeIt([&] { r.render(*s.list, img); });
        r.setThreads(1);
        const Times one = timeIt([&] { r.render(*s.list, img); });
        std::printf("  %-12s %4dx%-4d %5zu cmds  full %7.2f ms (min %6.2f), one thread %7.2f ms (min %6.2f)\n", s.name,
                    s.w, s.h, s.list->size(), full.median, full.min, one.median, one.min);
        if (std::strcmp(s.name, "cards-1080p") == 0) cardsMs = full.median;
        if (ops) {
            for (DrawOp op : {DrawOp::Rect, DrawOp::Shadow, DrawOp::Glyph, DrawOp::Image, DrawOp::Line}) {
                DrawList only;
                for (const DrawCmd& c : s.list->cmds())
                    if (c.op == op || c.op == DrawOp::PushClip || c.op == DrawOp::PopClip) only.push(c);
                const Times t = timeIt([&] { r.render(only, img); }, 5);
                std::printf("      op %d alone (one thread): %7.2f ms\n", int(op), t.median);
            }
        }
    }
    {
        // small change: one button of the second card re-rendered (its damage + the redraw region rule)
        const Rect btn = card.rect(card.find("apply"));
        const Rect damage{btn.x + 480.f - 2.f, btn.y - 2.f, btn.w + 4.f, btn.h + 4.f};
        const Rect region = cards.redrawRegion(damage, {0.f, 0.f, 1920.f, 1080.f});
        CpuRasterizer r;
        r.setGlyphAtlas(card.atlas().pixels(), card.atlas().size());
        Image img;
        img.resize(1920, 1080, 0xFF000000u);
        r.render(cards, img);
        const Times t = timeIt([&] { r.render(cards, img, region); }, 20);
        incrMs = t.median;
        std::printf("  incremental  %.0fx%.0f px region of cards-1080p: %.3f ms (min %.3f)\n", region.w, region.h,
                    t.median, t.min);
    }
    if (!report) {
        UI_CHECK(cardsMs <= 20.0);
        UI_CHECK(incrMs <= 2.0);
    }
    (void)hudScene;
    return finish("fuse_ui_cpu_raster_bench");
}
