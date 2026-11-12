// fuse_ui_render_scale: UiContext::setRenderScale (HiDPI). The UI stays in logical units, the painted output is in
// device pixels (logical x scale) and text is rasterised at the device resolution.
//   * scale 2: layout rects, hit testing and clicks are the logical ones; the frame is 2x the size; a filled rect
//     covers exactly the 2x pixel area
//   * text at scale 2 is crisp: it is not an upscale (nearest / bilinear) of the scale-1 frame, and it matches the same
//     scene built at double size (font-size, lengths) in a scale-1 context
//   * scale 1 (default, explicit, and after a round trip through 2) is the unscaled draw list, byte for byte
//   * scale 1.5: fills land on whole device pixels, text is there, input is unchanged
//   * deviceDamage(): an incremental raster of the damaged device rect equals a full repaint
//   * custom widget paint callbacks and OS popup surfaces are scaled without the widget knowing
// Optional --out=<png> (scale 1 upscaled | scale 2).
#include "test_util.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_write_impl.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
constexpr u32 kBg = 0xFF282020u, kBox = 0xFF2040E0u; // packed premultiplied 0xAABBGGRR of #202028 / #E04020
constexpr f32 kVw = 320.f, kVh = 200.f;
constexpr const char* kText = "Sharp glyphs: Hamburgefonstiv 0123";

int ch(u32 p, int k) { return int((p >> (8 * k)) & 0xFFu); }
std::string px(f32 v) { return std::to_string(int(v)) + "px"; }

struct Custom {
    static constexpr std::string_view kName = "scale-custom";
    static void paint(const PaintContext& pc, DrawList& dl) {
        dl.rect(pc.rect, Color::rgb(0x20C040));
        dl.line({pc.rect.x + 2.f, pc.rect.y + 8.f}, {pc.rect.right() - 2.f, pc.rect.y + 8.f}, Color::rgb(0xFFFFFF), 2.f);
        pc.ui.paintString(dl, "custom", pc.rect, Color::rgb(0x000000), 12.f, TextAlignment::Start, pc.node.index);
    }
};

/// The scene with every length multiplied by `k` (k = 2 at scale 1 is the reference for k = 1 at scale 2).
struct Scene {
    NodeHandle box, text, button, custom;
    int clicks = 0;
};
void build(UiContext& ui, Scene& s, f32 k, bool extras) {
    w::install(ui);
    ui.registry().registerWidget<Custom>();
    const NodeHandle root = ui.root();
    ui.setCss(root, "background", "#202028");
    ui.setCss(root, "padding", px(31.f * k));
    ui.setCss(root, "flex-direction", "column");
    ui.setCss(root, "align-items", "flex-start");
    s.box = w::panel(ui, root, "box");
    ui.setCss(s.box, "width", px(101.f * k));
    ui.setCss(s.box, "height", px(37.f * k));
    ui.setCss(s.box, "background", "#E04020");
    s.text = w::label(ui, root, kText, "text");
    ui.setCss(s.text, "font-size", px(16.f * k));
    ui.setCss(s.text, "color", "#FFFFFF");
    ui.setCss(s.text, "margin-top", px(9.f * k));
    if (!extras) return;
    s.button = w::button(ui, root, "Press", "press");
    ui.setCss(s.button, "margin-top", "8px");
    ui.connect(s.button, SignalKind::Clicked, [&s](UiContext&, NodeHandle, const SignalArgs&) { ++s.clicks; });
    s.custom = *ui.create("scale-custom", root, "custom");
    ui.setCss(s.custom, "width", "60px");
    ui.setCss(s.custom, "height", "20px");
    ui.setCss(s.custom, "margin-top", "6px");
}

void renderPx(UiContext& ui, Image& img) {
    const f32 s = ui.renderScale();
    img.resize(int(std::lround(ui.viewport().w * s)), int(std::lround(ui.viewport().h * s)), 0xFF000000u);
    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    r.render(ui.drawList(), img);
}
bool sameRect(Rect a, Rect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

/// Mean absolute difference (0..255, max over channels per pixel) of two same-size regions.
double meanDiff(const Image& a, const Image& b, int x0, int y0, int x1, int y1) {
    double sum = 0.0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            int d = 0;
            for (int k = 0; k < 3; ++k) d = std::max(d, std::abs(ch(a.at(x, y), k) - ch(b.at(x, y), k)));
            sum += d;
        }
    return sum / double(std::max(1, (x1 - x0) * (y1 - y0)));
}
Image upscale2(const Image& src, bool bilinear) {
    Image out;
    out.resize(src.width * 2, src.height * 2);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            if (!bilinear) { out.pixels[std::size_t(y) * std::size_t(out.width) + std::size_t(x)] = src.at(x / 2, y / 2); continue; }
            const f32 fx = std::clamp((f32(x) + 0.5f) * 0.5f - 0.5f, 0.f, f32(src.width - 1));
            const f32 fy = std::clamp((f32(y) + 0.5f) * 0.5f - 0.5f, 0.f, f32(src.height - 1));
            const int ix = std::min(int(fx), src.width - 2), iy = std::min(int(fy), src.height - 2);
            const f32 tx = fx - f32(ix), ty = fy - f32(iy);
            u32 o = 0;
            for (int k = 0; k < 4; ++k) {
                const f32 v = (f32(ch(src.at(ix, iy), k)) * (1.f - tx) + f32(ch(src.at(ix + 1, iy), k)) * tx) * (1.f - ty) +
                              (f32(ch(src.at(ix, iy + 1), k)) * (1.f - tx) + f32(ch(src.at(ix + 1, iy + 1), k)) * tx) * ty;
                o |= u32(std::clamp(std::lround(v), 0l, 255l)) << (8 * k);
            }
            out.pixels[std::size_t(y) * std::size_t(out.width) + std::size_t(x)] = o;
        }
    return out;
}
/// Sum of horizontal luminance steps: an edge-sharpness measure (a blurred upscale scores lower).
double edgeEnergy(const Image& a, int x0, int y0, int x1, int y1) {
    double e = 0.0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x + 1 < x1; ++x) e += std::abs(ch(a.at(x + 1, y), 1) - ch(a.at(x, y), 1));
    return e;
}
} // namespace

int main(int argc, char** argv) {
    // ---- scale 1 is the unscaled output --------------------------------------------------------------------------
    UiContext ui1({.viewport = {kVw, kVh}, .font = &font()});
    Scene s1;
    build(ui1, s1, 1.f, true);
    ui1.update();
    UI_CHECK(ui1.renderScale() == 1.f);
    const u64 hash1 = ui1.drawList().hash();
    Image img1;
    renderPx(ui1, img1);
    UI_CHECK_EQ(img1.width, 320);
    {
        UiContext u({.viewport = {kVw, kVh}, .font = &font()});
        Scene s;
        build(u, s, 1.f, true);
        u.setRenderScale(1.f);
        u.update();
        UI_CHECK(u.drawList().hash() == hash1);
        UI_CHECK(sameRect(u.deviceDamage(), u.damage()));
        u.setRenderScale(2.f); // there and back again
        u.update();
        UI_CHECK(u.drawList().hash() != hash1);
        u.setRenderScale(1.f);
        u.update();
        UI_CHECK(u.drawList().hash() == hash1);
        UI_CHECK(u.drawList().size() == ui1.drawList().size());
        Image i;
        renderPx(u, i);
        UI_CHECK(i.hash() == img1.hash());
        for (const DrawCmd& c : u.drawList().cmds()) UI_CHECK(c.devicePx == 0);
    }
    { // the Tier A sample tree too
        UiContext a({.viewport = {800, 600}, .font = &font()}), b({.viewport = {800, 600}, .font = &font()});
        w::install(a);
        w::install(b);
        buildSample(a);
        buildSample(b);
        a.update();
        b.setRenderScale(3.f);
        b.update();
        b.setRenderScale(1.f);
        b.update();
        UI_CHECK(a.drawList().size() == b.drawList().size());
        Image ia, ib; // same pixels (the lists differ only in where the glyphs sit in the atlas)
        renderPx(a, ia);
        renderPx(b, ib);
        UI_CHECK(ia.hash() == ib.hash());
    }

    // ---- scale 2: logical layout and input, 2x frame ---------------------------------------------------------------
    UiContext ui2({.viewport = {kVw, kVh}, .font = &font()});
    Scene s2;
    build(ui2, s2, 1.f, true);
    ui2.setRenderScale(2.f);
    ui2.update();
    UI_CHECK(ui2.renderScale() == 2.f);
    UI_CHECK(ui2.viewport().w == kVw && ui2.viewport().h == kVh);
    UI_CHECK(sameRect(ui2.rect(ui2.root()), ui1.rect(ui1.root())));
    UI_CHECK(sameRect(ui2.rect(s2.box), ui1.rect(s1.box)));
    UI_CHECK(sameRect(ui2.rect(s2.text), ui1.rect(s1.text)));
    UI_CHECK(sameRect(ui2.rect(s2.button), ui1.rect(s1.button)));
    UI_CHECK(sameRect(ui2.contentRect(s2.button), ui1.contentRect(s1.button)));
    UI_CHECK_RECT(ui2.rect(s2.box), 31, 31, 101, 37);
    UI_CHECK(ui2.drawList().size() == ui1.drawList().size());
    for (const DrawCmd& c : ui2.drawList().cmds()) UI_CHECK(c.devicePx == 0); // the flag never leaves the paint pass
    {
        const Rect b = ui2.rect(s2.button);
        const Vec2 c{b.x + b.w * 0.5f, b.y + b.h * 0.5f};
        UI_CHECK(ui2.hitTest(c) == s2.button);
        UI_CHECK(ui1.hitTest(c) == s1.button);
        UI_CHECK(ui2.hitTest({c.x * 2.f, c.y * 2.f}) != s2.button); // device coordinates are not input coordinates
        UI_CHECK(ui2.hitTest({32.f, 32.f}) == s2.box);
        UiTestDriver d(ui2);
        d.click("press");
        UI_CHECK_EQ(s2.clicks, 1);
        ui2.pointerMove({-1.f, -1.f});
        ui2.update();
    }
    Image img2;
    renderPx(ui2, img2);
    UI_CHECK_EQ(img2.width, 640);
    UI_CHECK_EQ(img2.height, 400);
    { // the box: exactly the 2x area, hard edges
        int inside = 0, wrong = 0, halo = 0;
        for (int y = 56; y < 142; ++y)
            for (int x = 56; x < 270; ++x) {
                const bool in = x >= 62 && x < 264 && y >= 62 && y < 136;
                const u32 p = img2.at(x, y);
                if (in) { ++inside; wrong += p != kBox; }
                else halo += p != kBg;
            }
        UI_CHECK_EQ(inside, 202 * 74);
        UI_CHECK_EQ(wrong, 0);
        UI_CHECK_EQ(halo, 0);
        UI_CHECK(img1.at(31, 31) == kBox && img1.at(30, 31) == kBg);
    }
    { // the custom widget's paint callback worked in logical units; its output is scaled
        const Rect c = ui2.rect(s2.custom);
        UI_CHECK(c.w == 60.f && c.h == 20.f);
        const int x0 = int(c.x * 2.f), y0 = int(c.y * 2.f);
        UI_CHECK(img2.at(x0 + 1, y0 + 1) == packPremultiplied(Color::rgb(0x20C040)));
        UI_CHECK(img2.at(x0 + 119, y0 + 39) == packPremultiplied(Color::rgb(0x20C040)));
        UI_CHECK(img2.at(x0 + 120, y0 + 39) == kBg);
        // the 2 px line at y + 8 is 4 device px thick (rows 14..17), white
        UI_CHECK(img2.at(x0 + 60, y0 + 15) == 0xFFFFFFFFu && img2.at(x0 + 60, y0 + 16) == 0xFFFFFFFFu);
        UI_CHECK(img2.at(x0 + 60, y0 + 12) != 0xFFFFFFFFu && img2.at(x0 + 60, y0 + 19) != 0xFFFFFFFFu);
        int dark = 0; // paintString text (black on green)
        for (int y = y0; y < y0 + 40; ++y)
            for (int x = x0; x < x0 + 120; ++x) dark += ch(img2.at(x, y), 1) < 60;
        UI_CHECK(dark > 40);
    }

    // ---- text: rasterised at the device size, not upscaled ---------------------------------------------------------
    {
        const Rect t = ui2.rect(s2.text);
        const int x0 = int(t.x) * 2, y0 = int(t.y) * 2;
        const int x1 = std::min(640, int(std::ceil(t.right())) * 2), y1 = int(std::ceil(t.bottom())) * 2;
        // glyph commands of the label: same count, texels about twice the size
        u32 n1 = 0, n2 = 0;
        double h1 = 0.0, h2 = 0.0;
        for (const DrawCmd& c : ui1.drawList().cmds())
            if (c.op == DrawOp::Glyph && c.node == s1.text.index) { ++n1; h1 += c.th; }
        for (const DrawCmd& c : ui2.drawList().cmds())
            if (c.op == DrawOp::Glyph && c.node == s2.text.index) {
                ++n2;
                h2 += c.th;
                UI_CHECK(c.rect.x == std::floor(c.rect.x) && c.rect.y == std::floor(c.rect.y)); // whole device pixels
                UI_CHECK(c.rect.w == f32(c.tw) && c.rect.h == f32(c.th));
            }
        UI_CHECK(n1 > 20 && n1 == n2);
        std::printf("  label glyphs: %u, mean height %.2f px at scale 1, %.2f px at scale 2\n", n1, h1 / n1, h2 / n2);
        UI_CHECK(h2 / n2 > 1.8 * (h1 / n1) && h2 / n2 < 2.2 * (h1 / n1));

        // reference: the same scene laid out at double size in a scale-1 context
        UiContext ref({.viewport = {kVw * 2.f, kVh * 2.f}, .font = &font()});
        Scene sr;
        build(ref, sr, 2.f, false);
        ref.update();
        Image imgRef;
        renderPx(ref, imgRef);
        UI_CHECK_RECT(ref.rect(sr.box), 62, 62, 202, 74);
        const Image nearest = upscale2(img1, false), linear = upscale2(img1, true);
        const double dRef = meanDiff(img2, imgRef, x0, y0, x1, y1);
        const double dNearest = meanDiff(img2, nearest, x0, y0, x1, y1), dLinear = meanDiff(img2, linear, x0, y0, x1, y1);
        const double e2 = edgeEnergy(img2, x0, y0, x1, y1), eRef = edgeEnergy(imgRef, x0, y0, x1, y1);
        const double eLinear = edgeEnergy(linear, x0, y0, x1, y1);
        std::printf("  text region %dx%d: mean diff vs 2x-size reference %.3f, vs nearest upscale %.3f, vs bilinear upscale %.3f\n",
                    x1 - x0, y1 - y0, dRef, dNearest, dLinear);
        std::printf("  edge energy: scale 2 %.0f, reference %.0f, bilinear upscale %.0f\n", e2, eRef, eLinear);
        UI_CHECK(dRef < 2.0);                    // matches text rasterised at double size
        UI_CHECK(dNearest > 6.0 && dLinear > 6.0); // and is not an upscale of the scale-1 frame
        UI_CHECK(dNearest > 4.0 * dRef + 1.0 && dLinear > 4.0 * dRef + 1.0);
        UI_CHECK(e2 > 1.3 * eLinear && e2 > 0.9 * eRef && e2 < 1.1 * eRef);
        int lit = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) lit += ch(img2.at(x, y), 1) > 200;
        UI_CHECK(lit > 400);

        const std::string out = argc > 1 && std::string(argv[1]).rfind("--out=", 0) == 0 ? std::string(argv[1]).substr(6) : "";
        if (!out.empty()) {
            Image both;
            both.resize(640, 800, 0xFF000000u);
            for (int y = 0; y < 400; ++y)
                for (int x = 0; x < 640; ++x) {
                    both.pixels[std::size_t(y) * 640 + std::size_t(x)] = linear.at(x, y);
                    both.pixels[std::size_t(y + 400) * 640 + std::size_t(x)] = img2.at(x, y);
                }
            stbi_write_png(out.c_str(), 640, 800, 4, both.pixels.data(), 640 * 4);
        }
    }

    // ---- damage: logical, with a device-pixel accessor; incremental raster == full repaint -------------------------
    {
        Image inc = img2;
        const Rect b = ui2.rect(s2.box);
        ui2.setCss(s2.box, "background", "#40E020"); // repaints the box only
        ui2.update();
        const Rect dmg = ui2.damage(), dev = ui2.deviceDamage();
        std::printf("  damage (%g %g %g %g) logical -> (%g %g %g %g) device px\n", double(dmg.x), double(dmg.y), double(dmg.w),
                    double(dmg.h), double(dev.x), double(dev.y), double(dev.w), double(dev.h));
        UI_CHECK(!dmg.empty() && sameRect(dmg, b)); // logical units
        UI_CHECK(dev.x <= dmg.x * 2.f && dev.y <= dmg.y * 2.f && dev.right() >= dmg.right() * 2.f && dev.bottom() >= dmg.bottom() * 2.f);
        UI_CHECK(dev.w <= dmg.w * 2.f + 4.f && dev.h <= dmg.h * 2.f + 4.f);
        const Rect sc = dev.intersect({0.f, 0.f, 640.f, 400.f});
        for (int y = int(sc.y); y < int(sc.bottom()); ++y)
            std::fill_n(inc.pixels.begin() + std::ptrdiff_t(y) * 640 + int(sc.x), int(sc.w), 0xFF000000u);
        CpuRasterizer r;
        r.setGlyphAtlas(ui2.atlas().pixels(), ui2.atlas().size());
        r.render(ui2.drawList(), inc, sc);
        Image full;
        renderPx(ui2, full);
        UI_CHECK(inc.hash() == full.hash());
        UI_CHECK(full.hash() != img2.hash());
    }

    // ---- fractional scale ------------------------------------------------------------------------------------------
    {
        UiContext u({.viewport = {kVw, kVh}, .font = &font()});
        Scene s;
        build(u, s, 1.f, true);
        u.setRenderScale(1.5f);
        u.update();
        UI_CHECK(sameRect(u.rect(s.box), ui1.rect(s1.box)) && sameRect(u.rect(s.text), ui1.rect(s1.text)));
        Image img;
        renderPx(u, img);
        UI_CHECK_EQ(img.width, 480);
        UI_CHECK_EQ(img.height, 300);
        // box 31..132 x 31..68 -> 46.5..198 x 46.5..102: edges rounded to whole device pixels (47..198, 47..102)
        int wrong = 0, halo = 0;
        for (int y = 40; y < 108; ++y)
            for (int x = 40; x < 204; ++x) {
                const bool in = x >= 47 && x < 198 && y >= 47 && y < 102;
                if (in) wrong += img.at(x, y) != kBox;
                else halo += img.at(x, y) != kBg;
            }
        UI_CHECK_EQ(wrong, 0);
        UI_CHECK_EQ(halo, 0);
        u32 glyphs = 0;
        double hh = 0.0, h1 = 0.0;
        u32 n1 = 0;
        for (const DrawCmd& c : u.drawList().cmds())
            if (c.op == DrawOp::Glyph && c.node == s.text.index) { ++glyphs; hh += c.th; }
        for (const DrawCmd& c : ui1.drawList().cmds())
            if (c.op == DrawOp::Glyph && c.node == s1.text.index) { ++n1; h1 += c.th; }
        UI_CHECK(glyphs == n1 && hh / glyphs > 1.3 * (h1 / n1) && hh / glyphs < 1.7 * (h1 / n1));
        const Rect t = u.rect(s.text);
        int lit = 0, outside = 0;
        for (int y = 0; y < img.height; ++y)
            for (int x = 0; x < img.width; ++x) {
                if (ch(img.at(x, y), 0) < 200 || ch(img.at(x, y), 1) < 200 || ch(img.at(x, y), 2) < 200) continue;
                const bool in = f32(x) >= t.x * 1.5f - 2.f && f32(x) < t.right() * 1.5f + 2.f && f32(y) >= t.y * 1.5f - 2.f &&
                                f32(y) < t.bottom() * 1.5f + 2.f;
                const Rect c = u.rect(s.custom), b = u.rect(s.button);
                const bool other = f32(y) >= std::min(c.y, b.y) * 1.5f - 2.f; // button label / custom line: also white
                if (in) ++lit;
                else if (!other) ++outside;
            }
        UI_CHECK(lit > 200);
        UI_CHECK_EQ(outside, 0);
        const Rect b = u.rect(s.button);
        UI_CHECK(u.hitTest({b.x + 2.f, b.y + 2.f}) == s.button);
        UiTestDriver d(u);
        d.click("press");
        UI_CHECK_EQ(s.clicks, 1);
        u.setRenderScale(0.f); // nonsense scales fall back to 1
        UI_CHECK(u.renderScale() == 1.f);
    }

    // ---- OS popup surfaces: rect logical, list in device pixels at the popup's origin ------------------------------
    {
        UiContext u({.viewport = {kVw, kVh}, .font = &font()});
        w::install(u);
        u.setCss(u.root(), "padding", "20px");
        u.setCss(u.root(), "align-items", "flex-start");
        const NodeHandle pop = w::panel(u, u.root(), "pop");
        u.addClass(pop, "clamp");
        u.setCss(pop, "width", "90px");
        u.setCss(pop, "height", "50px");
        u.setCss(pop, "background", "#E04020");
        const NodeHandle l = w::label(u, pop, "Item", "item");
        u.setCss(l, "color", "#FFFFFF");
        u.setOsPopups(true);
        u.setRenderScale(2.f);
        u.update();
        UI_CHECK_EQ(u.popupSurfaces().size(), std::size_t(1));
        if (u.popupSurfaces().size() == 1) {
            const auto& ps = u.popupSurfaces()[0];
            UI_CHECK_RECT(ps.rect, 20, 20, 90, 50);
            Image img;
            img.resize(180, 100, 0xFF000000u);
            CpuRasterizer r;
            r.setGlyphAtlas(u.atlas().pixels(), u.atlas().size());
            r.render(ps.draw, img);
            UI_CHECK(img.at(0, 0) == kBox && img.at(179, 99) == kBox);
            int lit = 0;
            for (u32 p : img.pixels) lit += p == 0xFFFFFFFFu;
            UI_CHECK(lit > 20);
            bool glyph = false;
            for (const DrawCmd& c : ps.draw.cmds())
                if (c.op == DrawOp::Glyph) { glyph = true; UI_CHECK(c.rect.x >= 0.f && c.rect.right() <= 180.f && c.devicePx == 0); }
            UI_CHECK(glyph);
        }
        for (const DrawCmd& c : u.drawList().cmds()) UI_CHECK(c.node != pop.index); // not in the main list
    }

    // ---- DrawList::scale on its own --------------------------------------------------------------------------------
    {
        DrawList dl;
        dl.roundedRect({10.f, 20.f, 30.f, 40.f}, Color::rgb(0xFFFFFF), {4.f, 4.f, 4.f, 4.f}, Color::rgb(0), 1.f);
        dl.shadow({1.f, 2.f, 3.f, 4.f}, Color::rgb(0), 6.f, 2.f);
        dl.line({1.f, 2.f}, {3.f, 4.f}, Color::rgb(0), 1.5f);
        dl.pushClip({5.f, 5.f, 10.f, 10.f}, 3.f);
        dl.popClip();
        const u64 h = dl.hash();
        dl.scale(1.f);
        UI_CHECK(dl.hash() == h);
        dl.scale(2.f);
        const auto& c = dl.cmds();
        UI_CHECK(sameRect(c[0].rect, {20.f, 40.f, 60.f, 80.f}) && c[0].radius[0] == 8.f && c[0].borderWidth == 2.f);
        UI_CHECK(sameRect(c[1].rect, {2.f, 4.f, 6.f, 8.f}) && c[1].blur == 12.f && c[1].radius[2] == 4.f);
        UI_CHECK(sameRect(c[2].rect, {2.f, 4.f, 6.f, 8.f}) && c[2].borderWidth == 3.f);
        UI_CHECK(sameRect(c[3].rect, {10.f, 10.f, 20.f, 20.f}) && c[3].radius[0] == 6.f);
        DrawList f;
        f.roundedRect({1.f, 1.f, 0.5f, 3.f}, Color::rgb(0xFFFFFF), {}, Color::rgb(0), 1.f);
        f.scale(1.25f); // 1.25..1.875 would vanish between pixel centres: kept 1 px wide; border stays a whole pixel
        UI_CHECK(sameRect(f.cmds()[0].rect, {1.f, 1.f, 1.f, 4.f}) && f.cmds()[0].borderWidth == 1.f);
    }
    return finish("fuse_ui_render_scale");
}
