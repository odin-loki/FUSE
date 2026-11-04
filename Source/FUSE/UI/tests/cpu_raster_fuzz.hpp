// Shared by fuse_ui_cpu_raster_bench (and cross-platform runs of the same check): a random draw list of every
// primitive, and an exact image comparison.
#pragma once

#include "fuse/ui/draw_list.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <random>

namespace fuse::ui::test {

inline bool same(const Image& a, const Image& b, const char* what) {
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
inline DrawList fuzzList(u32 seed, int w, int h, int atlasSize, u32 images, bool effects) {
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

} // namespace fuse::ui::test
