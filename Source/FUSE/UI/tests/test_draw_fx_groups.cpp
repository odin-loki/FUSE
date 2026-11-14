// Gate fuse_ui_draw_fx_groups: the effect groups of the draw list (DrawList::pushTransform / pushFilter / pushMask)
// on the CPU reference rasteriser, against references computed here.
//   * transform: a pure translation is the same picture moved by whole pixels (bit exact); rotation / scale / skew
//     put every pixel where the inverse matrix says (against the analytic shape), nested groups compose, glyphs and
//     clips inside a group follow it, a singular matrix draws nothing
//   * filter: blur against a brute-force Gaussian convolution of the unfiltered picture; the colour functions against
//     the Filter Effects matrices on arbitrary colours with alpha; drop-shadow; filterReach bounds what is touched
//   * mask: alpha and luminance of linear / radial gradients multiply the group
//   * the list operations keep the picture: translate(d) moves it, scale(2) doubles it, a scissored
//     render equals the same region of a full render (what damage repaints rely on), groups nest in any order
//   * robustness: bad side-record indices, unbalanced groups, empty and enormous bounds, NaN matrices
//   * rendering a recorded list again allocates nothing
#include "fuse/ui/painter.hpp"
#include "test_alloc_hooks.hpp"
#include "test_util.hpp"

#include <cmath>
#include <cstdlib>
#include <new>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;

static long g_allocs = 0;
static bool g_count = false;
// Every global allocation form is replaced and counted (test_alloc_hooks.hpp).
void fuse::ui::test::countAlloc(std::size_t) noexcept {
    if (g_count) ++g_allocs;
}

namespace {
constexpr int W = 240, H = 180;
constexpr f32 kPi = 3.14159265358979f;
int ch(u32 p, int c) { return int((p >> (8 * c)) & 0xFF); }
Image render(const DrawList& dl, int w = W, int h = H, Rect scissor = {}, u32 clear = 0) {
    Image img;
    img.resize(w, h, clear);
    CpuRasterizer().render(dl, img, scissor);
    return img;
}
struct Diff {
    int maxErr = 0;
    int differing = 0;
};
Diff compare(const Image& a, const Image& b, int x0 = 0, int y0 = 0, int x1 = W, int y1 = H, int dx = 0, int dy = 0) {
    Diff d;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            if (x + dx < 0 || y + dy < 0 || x + dx >= b.width || y + dy >= b.height) continue;
            int m = 0;
            for (int c = 0; c < 4; ++c) m = std::max(m, std::abs(ch(a.at(x, y), c) - ch(b.at(x + dx, y + dy), c)));
            d.maxErr = std::max(d.maxErr, m);
            d.differing += m ? 1 : 0;
        }
    return d;
}
/// A small scene with every kind of content: rects, a rounded border, a gradient, a path, a line, a clip.
void content(DrawList& dl, Vec2 o) {
    dl.rect({o.x, o.y, 80, 50}, Color::rgb(0xD03030));
    dl.roundedRect({o.x + 10, o.y + 8, 40, 24}, Color::rgb(0x30D060, 0.8f), {8, 2, 8, 2}, Color::rgb(0xFFFFFF), 2.f);
    dl.gradientRect({o.x + 52, o.y + 6, 24, 38}, Color::rgb(0x2040FF), Color::rgb(0xFFE040), true);
    static Path p; // reused: recording allocates nothing once warm
    p.clear();
    p.moveTo({o.x + 6, o.y + 44}).lineTo({o.x + 40, o.y + 36}).lineTo({o.x + 30, o.y + 48}).close();
    dl.fillPath(p, Brush(Color::rgb(0xFFFFFF, 0.7f)));
    dl.line({o.x + 2, o.y + 2}, {o.x + 78, o.y + 48}, Color::rgb(0x000000, 0.6f), 1.5f);
    dl.pushClip({o.x + 60, o.y + 30, 30, 30});
    dl.rect({o.x + 50, o.y + 25, 60, 40}, Color::rgb(0x8020C0, 0.9f), 6.f);
    dl.popClip();
}
bool inRect(const Rect& r, Vec2 p, f32 margin) { return p.x >= r.x + margin && p.y >= r.y + margin && p.x < r.right() - margin && p.y < r.bottom() - margin; }
} // namespace

int main() {
    // ---- transform: translation ----
    {
        DrawList plain, moved;
        content(plain, {30, 20});
        moved.pushTransform(Affine::translation(57.f, 41.f), {30, 20, 90, 60});
        content(moved, {30, 20});
        moved.popClip();
        const Image a = render(plain), b = render(moved);
        const Diff d = compare(a, b, 0, 0, W - 57, H - 41, 57, 41);
        UI_CHECK_EQ(d.maxErr, 0); // the same pixels, 57 x 41 further
        UI_CHECK(compare(b, render(DrawList{}), 0, 0, 80, 60).differing == 0); // and nothing where it was
        DrawList frac; // a fractional translation snaps to the pixel grid: still bit exact
        frac.pushTransform(Affine::translation(57.4f, 40.6f), {30, 20, 90, 60});
        content(frac, {30, 20});
        frac.popClip();
        UI_CHECK_EQ(compare(b, render(frac)).maxErr, 0);
        // inside a rect clip the moved content is cut by the clip, not by where it came from
        DrawList clipped;
        clipped.pushClip({100, 70, 40, 30});
        clipped.pushTransform(Affine::translation(57.f, 41.f), {30, 20, 90, 60});
        content(clipped, {30, 20});
        clipped.popClip();
        clipped.popClip();
        const Image c = render(clipped);
        UI_CHECK_EQ(compare(b, c, 100, 70, 140, 100).maxErr, 0);
        UI_CHECK(compare(c, render(DrawList{}), 0, 0, W, 70).differing == 0 && compare(c, render(DrawList{}), 140, 0, W, H).differing == 0);
    }

    // ---- transform: rotation / scale / skew against the analytic shape ----
    {
        const Rect src{40, 30, 100, 60};
        struct Case { const char* name; Affine xf; };
        const Vec2 c{90, 60};
        auto about = [&](const Affine& m) { return Affine::translation(c.x + 30, c.y + 30) * m * Affine::translation(-c.x, -c.y); };
        const Case cases[] = {{"rotate 30", about(Affine::rotation(kPi / 6))}, {"rotate 90", about(Affine::rotation(kPi / 2))},
                              {"scale 1.5 x 0.75", about(Affine::scaling(1.5f, 0.75f))}, {"mirror", about(Affine::scaling(-1.f, 1.f))},
                              {"skew", about(Affine{1.f, 0.2f, 0.5f, 1.f, 0.f, 0.f})},
                              {"rotate + scale", about(Affine::rotation(-1.f) * Affine::scaling(0.6f, 1.2f))}};
        for (const Case& cs : cases) {
            DrawList dl;
            dl.pushTransform(cs.xf, src);
            dl.rect(src, Color::rgb(0xFFFFFF));
            dl.rect({src.x, src.y, 50, 30}, Color{1.f, 0.f, 0.f, 1.f}); // the top left quarter is red
            dl.popClip();
            const Image img = render(dl);
            Affine inv;
            UI_CHECK(cs.xf.invert(inv));
            int wrong = 0, inside = 0;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const Vec2 p = inv.apply({f32(x) + 0.5f, f32(y) + 0.5f});
                    const u32 px = img.at(x, y);
                    // pixels clearly inside / outside (1.5 source px from an edge, scaled) must be exact; edges may blend
                    const f32 m = 1.5f / std::max(0.05f, std::min(1.f, cs.xf.meanScale()));
                    if (inRect(src, p, m)) {
                        ++inside;
                        const bool red = inRect({src.x, src.y, 50, 30}, p, m), white = !inRect({src.x, src.y, 50, 30}, p, -m);
                        if (red && px != 0xFF0000FFu) ++wrong;
                        if (white && px != 0xFFFFFFFFu) ++wrong;
                    } else if (!inRect(src, p, -m) && px != 0u) ++wrong;
                }
            if (wrong) std::fprintf(stderr, "  transform case `%s`: %d wrong pixels (%d inside)\n", cs.name, wrong, inside);
            UI_CHECK(wrong == 0 && inside > 2000);
        }
        // edges are antialiased: a 45 degree edge has intermediate coverage, and the covered area is the shape's area
        DrawList dl;
        dl.pushTransform(about(Affine::rotation(kPi / 4)), src);
        dl.rect(src, Color::rgb(0xFFFFFF));
        dl.popClip();
        const Image img = render(dl);
        double area = 0.0;
        int soft = 0;
        for (u32 p : img.pixels) {
            area += f32(p >> 24) / 255.f;
            soft += (p >> 24) > 0 && (p >> 24) < 255;
        }
        std::printf("rotated 100 x 60 rect: covered area %.1f (ideal 6000), %d edge pixels\n", area, soft);
        UI_CHECK(std::fabs(area - 6000.0) < 60.0 && soft > 200);
        // nested groups compose: T1 (T2 content) == (T1 * T2) content
        const Affine t1 = about(Affine::rotation(0.4f)), t2 = Affine::translation(20, -10) * Affine::scaling(0.8f, 0.8f);
        DrawList nested, single;
        nested.pushTransform(t1, {0, 0, 200, 150});
        nested.pushTransform(t2, {30, 20, 90, 60});
        content(nested, {30, 20});
        nested.popClip();
        nested.popClip();
        single.pushTransform(t1 * t2, {30, 20, 90, 60});
        content(single, {30, 20});
        single.popClip();
        const Diff d = compare(render(nested), render(single));
        std::printf("nested vs composed transform: max err %d, %d differing\n", d.maxErr, d.differing);
        UI_CHECK(d.maxErr <= 96 && d.differing < 2000); // resampled twice vs once: only edge pixels differ
        // a singular matrix draws nothing; an empty source too
        DrawList none;
        none.pushTransform(Affine::scaling(0.f, 1.f), src);
        none.rect(src, Color::rgb(0xFFFFFF));
        none.popClip();
        none.pushTransform(Affine::rotation(1.f), {});
        none.rect(src, Color::rgb(0xFFFFFF));
        none.popClip();
        UI_CHECK(compare(render(none), render(DrawList{})).differing == 0);
    }

    // ---- filter: blur against a brute-force Gaussian ----
    for (const f32 sigma : {1.f, 2.5f, 4.f, 9.f}) {
        DrawList plain, blurred;
        content(plain, {80, 60});
        const FilterOp op{FilterKind::Blur, sigma, {}, {}};
        blurred.pushFilter({&op, 1}, {80, 60, 90, 60});
        content(blurred, {80, 60});
        blurred.popClip();
        const Image src = render(plain), got = render(blurred);
        const int r = int(std::ceil(sigma * 4.f));
        std::vector<f32> kernel(std::size_t(2 * r + 1));
        f32 sum = 0.f;
        for (int k = -r; k <= r; ++k) sum += kernel[std::size_t(k + r)] = std::exp(-f32(k * k) / (2.f * sigma * sigma));
        for (f32& k : kernel) k /= sum;
        std::vector<f32> tmp(std::size_t(W) * H * 4, 0.f), ref(std::size_t(W) * H * 4, 0.f);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int k = -r; k <= r; ++k) {
                    if (x + k < 0 || x + k >= W) continue;
                    for (int c = 0; c < 4; ++c) tmp[(std::size_t(y) * W + x) * 4 + c] += kernel[std::size_t(k + r)] * f32(ch(src.at(x + k, y), c));
                }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int k = -r; k <= r; ++k) {
                    if (y + k < 0 || y + k >= H) continue;
                    for (int c = 0; c < 4; ++c) ref[(std::size_t(y) * W + x) * 4 + c] += kernel[std::size_t(k + r)] * tmp[(std::size_t(y + k) * W + x) * 4 + c];
                }
        f32 worst = 0.f;
        double total = 0.0, wantTotal = 0.0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int c = 0; c < 4; ++c) {
                    const f32 want = ref[(std::size_t(y) * W + x) * 4 + c];
                    worst = std::max(worst, std::fabs(f32(ch(got.at(x, y), c)) - want));
                    total += ch(got.at(x, y), c);
                    wantTotal += want;
                }
        std::printf("blur sigma %.1f: worst channel error vs Gaussian %.1f / 255, energy %.4f\n", sigma, worst, total / wantTotal);
        UI_CHECK(worst <= 6.f);                                 // within 2.5 % of the Gaussian (three box blurs; the kernel itself below 2 px)
        UI_CHECK(std::fabs(total / wantTotal - 1.0) < 0.004);   // and it keeps the energy
        // nothing beyond filterReach of the content bounds
        const f32 reach = filterReach({&op, 1});
        UI_CHECK(reach >= sigma * 2.f && reach <= sigma * 3.f + 2.f);
        int outside = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (!inRect({80, 60, 90, 60}, {f32(x) + 0.5f, f32(y) + 0.5f}, -reach - 1.f) && got.at(x, y)) ++outside;
        UI_CHECK_EQ(outside, 0);
    }

    // ---- filter: colour functions on arbitrary colours with alpha ----
    {
        struct Fn { FilterKind kind; f32 amount; };
        const Fn fns[] = {{FilterKind::Brightness, 1.6f}, {FilterKind::Brightness, 0.3f}, {FilterKind::Contrast, 1.8f}, {FilterKind::Contrast, 0.4f},
                          {FilterKind::Grayscale, 1.f}, {FilterKind::Grayscale, 0.5f}, {FilterKind::Sepia, 1.f}, {FilterKind::Saturate, 2.f},
                          {FilterKind::Saturate, 0.f}, {FilterKind::HueRotate, kPi / 2}, {FilterKind::HueRotate, kPi}, {FilterKind::Invert, 1.f},
                          {FilterKind::Invert, 0.25f}, {FilterKind::Opacity, 0.4f}};
        const Color colours[] = {Color{0.9f, 0.2f, 0.1f, 1.f}, Color{0.1f, 0.6f, 0.9f, 0.6f}, Color{0.5f, 0.5f, 0.5f, 1.f}, Color{1.f, 1.f, 0.f, 0.3f}};
        auto matrix = [](const Fn& f, const f32 in[3], f32 out[3], f32& alpha) {
            const f32 a = f.amount;
            auto lin = [&](const f32 m[9], f32 off) {
                for (int k = 0; k < 3; ++k) out[k] = m[k * 3] * in[0] + m[k * 3 + 1] * in[1] + m[k * 3 + 2] * in[2] + off;
            };
            switch (f.kind) {
            case FilterKind::Brightness: { const f32 m[9] = {a, 0, 0, 0, a, 0, 0, 0, a}; lin(m, 0); break; }
            case FilterKind::Contrast: { const f32 m[9] = {a, 0, 0, 0, a, 0, 0, 0, a}; lin(m, 0.5f - 0.5f * a); break; }
            case FilterKind::Invert: { const f32 k = 1.f - 2.f * a; const f32 m[9] = {k, 0, 0, 0, k, 0, 0, 0, k}; lin(m, a); break; }
            case FilterKind::Opacity: { const f32 m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; lin(m, 0); alpha *= a; break; }
            case FilterKind::Grayscale:
            case FilterKind::Saturate: { // Filter Effects 1: grayscale(a) is the saturate matrix at 1 - a
                const f32 s = f.kind == FilterKind::Grayscale ? 1.f - a : a;
                const f32 m[9] = {0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s, 0.213f - 0.213f * s, 0.715f + 0.285f * s,
                                  0.072f - 0.072f * s, 0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s};
                lin(m, 0);
                break;
            }
            case FilterKind::Sepia: { const f32 m[9] = {0.393f, 0.769f, 0.189f, 0.349f, 0.686f, 0.168f, 0.272f, 0.534f, 0.131f}; lin(m, 0); break; }
            case FilterKind::HueRotate: {
                const f32 c = std::cos(a), s = std::sin(a);
                const f32 m[9] = {0.213f + c * 0.787f - s * 0.213f, 0.715f - c * 0.715f - s * 0.715f, 0.072f - c * 0.072f + s * 0.928f,
                                  0.213f - c * 0.213f + s * 0.143f, 0.715f + c * 0.285f + s * 0.140f, 0.072f - c * 0.072f - s * 0.283f,
                                  0.213f - c * 0.213f - s * 0.787f, 0.715f - c * 0.715f + s * 0.715f, 0.072f + c * 0.928f + s * 0.072f};
                lin(m, 0);
                break;
            }
            default: break;
            }
        };
        int worst = 0;
        for (const Fn& f : fns)
            for (const Color& col : colours) {
                DrawList dl;
                const FilterOp op{f.kind, f.amount, {}, {}};
                dl.pushFilter({&op, 1}, {10, 10, 40, 40});
                dl.rect({10, 10, 40, 40}, col);
                dl.popClip();
                const u32 got = render(dl, 64, 64).at(30, 30);
                // the layer holds the colour as premultiplied RGBA8: start from what an unfiltered render stores
                DrawList base;
                base.rect({10, 10, 40, 40}, col);
                const u32 stored = render(base, 64, 64).at(30, 30);
                const f32 a0 = f32(stored >> 24) / 255.f;
                const f32 in[3] = {f32(ch(stored, 0)) / 255.f / a0, f32(ch(stored, 1)) / 255.f / a0, f32(ch(stored, 2)) / 255.f / a0};
                f32 out[3], alpha = a0;
                matrix(f, in, out, alpha);
                for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(ch(got, k) - int(std::lround(std::clamp(out[k], 0.f, 1.f) * alpha * 255.f))));
                worst = std::max(worst, std::abs(int(got >> 24) - int(std::lround(alpha * 255.f))));
            }
        std::printf("colour filters: worst channel error %d / 255\n", worst);
        UI_CHECK(worst <= 1);
        // a chain applies in order (invert then brightness is not brightness then invert)
        const FilterOp ab[2] = {{FilterKind::Invert, 1.f, {}, {}}, {FilterKind::Brightness, 0.5f, {}, {}}};
        const FilterOp ba[2] = {{FilterKind::Brightness, 0.5f, {}, {}}, {FilterKind::Invert, 1.f, {}, {}}};
        DrawList one, two;
        one.pushFilter(ab, {0, 0, 20, 20});
        one.rect({0, 0, 20, 20}, Color::rgb(0xFF0000));
        one.popClip();
        two.pushFilter(ba, {0, 0, 20, 20});
        two.rect({0, 0, 20, 20}, Color::rgb(0xFF0000));
        two.popClip();
        UI_CHECK_EQ(render(one, 32, 32).at(10, 10), 0xFF808000u); // (0, 1, 1) * 0.5
        UI_CHECK_EQ(render(two, 32, 32).at(10, 10), 0xFFFFFF7Fu); // 1 - (128 / 255, 0, 0): each function works on the stored 8 bits
        // no functions: a plain group
        DrawList empty, plain;
        empty.pushFilter({}, {30, 20, 90, 60});
        content(empty, {30, 20});
        empty.popClip();
        content(plain, {30, 20});
        UI_CHECK_EQ(compare(render(empty), render(plain)).maxErr, 0);
    }

    // ---- filter: drop-shadow ----
    {
        DrawList dl;
        const FilterOp hard{FilterKind::DropShadow, 0.f, {12, 7}, Color{0.f, 1.f, 0.f, 0.5f}};
        dl.pushFilter({&hard, 1}, {40, 40, 60, 40});
        dl.rect({40, 40, 60, 40}, Color::rgb(0xFF0000));
        dl.popClip();
        const Image img = render(dl);
        UI_CHECK_EQ(img.at(60, 60), 0xFF0000FFu);  // the source on top of its shadow
        UI_CHECK_EQ(img.at(105, 60), 0x80008000u); // the shadow alone: green at half alpha (premultiplied)
        UI_CHECK_EQ(img.at(60, 84), 0x80008000u);
        UI_CHECK(img.at(45, 84) == 0u && img.at(113, 60) == 0u && img.at(60, 88) == 0u);
        // a blurred shadow keeps the source's alpha mass (times the shadow alpha), centred on the offset shape
        DrawList soft;
        const FilterOp blur{FilterKind::DropShadow, 4.f, {0, 60}, Color{1.f, 1.f, 1.f, 1.f}};
        soft.pushFilter({&blur, 1}, {40, 20, 60, 40});
        soft.rect({40, 20, 60, 40}, Color::rgb(0xFF0000));
        soft.popClip();
        const Image s = render(soft);
        double mass = 0.0, cx = 0.0, cy = 0.0;
        for (int y = 62; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const f32 a = f32(s.at(x, y) >> 24) / 255.f;
                mass += a;
                cx += a * (f32(x) + 0.5f);
                cy += a * (f32(y) + 0.5f);
            }
        std::printf("soft drop-shadow: mass %.1f (ideal 2400), centre %.2f %.2f (ideal 70 100)\n", mass, cx / mass, cy / mass);
        UI_CHECK(std::fabs(mass - 2400.0) < 12.0 && std::fabs(cx / mass - 70.0) < 0.1 && std::fabs(cy / mass - 100.0) < 0.1);
    }

    // ---- mask ----
    {
        const GradientStop fade[2] = {{0.f, Color{0, 0, 0, 1}}, {1.f, Color{0, 0, 0, 0}}};
        DrawList dl;
        dl.pushMask(Gradient::linear({20, 0}, {220, 0}, fade), {20, 20, 200, 100});
        dl.rect({20, 20, 200, 100}, Color::rgb(0xFFFFFF));
        dl.popClip();
        const Image img = render(dl);
        int worst = 0;
        for (int x = 20; x < 220; ++x) worst = std::max(worst, std::abs(int(img.at(x, 60) >> 24) - int(std::lround(255.f * (1.f - (f32(x) + 0.5f - 20.f) / 200.f)))));
        UI_CHECK(worst <= 1);
        // luminance: white keeps, black removes, a colour by its luminance; the gradient's alpha counts too
        const GradientStop lum[3] = {{0.f, Color{1, 1, 1, 1}}, {0.5f, Color{0, 1, 0, 1}}, {1.f, Color{1, 1, 1, 0}}};
        DrawList dl2;
        dl2.pushMask(Gradient::linear({20, 0}, {220, 0}, lum), {20, 20, 200, 100}, MaskMode::Luminance);
        dl2.rect({20, 20, 200, 100}, Color::rgb(0xFFFFFF));
        dl2.popClip();
        const Image img2 = render(dl2);
        UI_CHECK(std::abs(int(img2.at(20, 60) >> 24) - 254) <= 2 && std::abs(int(img2.at(120, 60) >> 24) - int(0.7152f * 255)) <= 3 && (img2.at(219, 60) >> 24) <= 2);
        // radial; and a mask around mixed content keeps its colours (premultiplied scaling)
        const GradientStop spot[2] = {{0.5f, Color{0, 0, 0, 1}}, {0.5f, Color{0, 0, 0, 0}}};
        DrawList dl3;
        dl3.pushMask(Gradient::radial({75, 45}, 40, spot), {30, 20, 90, 60});
        content(dl3, {30, 20});
        dl3.popClip();
        DrawList plain;
        content(plain, {30, 20});
        const Image m = render(dl3), p = render(plain);
        UI_CHECK(m.at(75, 45) == p.at(75, 45) && m.at(60, 40) == p.at(60, 40) && m.at(100, 45) == 0u && m.at(40, 25) == 0u);
        // without a usable gradient everything is masked out
        DrawList dl4;
        dl4.pushMask(Gradient{}, {0, 0, 100, 100});
        dl4.rect({0, 0, 100, 100}, Color::rgb(0xFFFFFF));
        dl4.popClip();
        UI_CHECK(compare(render(dl4), render(DrawList{})).differing == 0);
    }

    // ---- list operations and scissored renders ----
    {
        const GradientStop fade[2] = {{0.f, Color{0, 0, 0, 1}}, {1.f, Color{0, 0, 0, 0.2f}}};
        const FilterOp ops[2] = {{FilterKind::Blur, 2.f, {}, {}}, {FilterKind::DropShadow, 1.5f, {5, 4}, Color{0.f, 0.f, 0.f, 0.6f}}};
        auto scene = [&](DrawList& dl) {
            dl.rect({0, 0, 240, 180}, Color::rgb(0x203040));
            dl.pushTransform(Affine::translation(120, 90) * Affine::rotation(0.5f) * Affine::translation(-75, -50), {30, 20, 90, 60});
            dl.pushMask(Gradient::linear({30, 0}, {120, 0}, fade), {30, 20, 90, 60});
            dl.pushFilter(ops, {30, 20, 90, 60});
            content(dl, {30, 20});
            dl.popClip();
            dl.popClip();
            dl.popClip();
            dl.pushFilter({&ops[0], 1}, {150, 110, 80, 60}); // the other nesting order: a transform inside a filter
            dl.pushTransform(Affine::translation(150, 110) * Affine::scaling(0.7f, 0.7f) * Affine::translation(-30, -20), {30, 20, 90, 60});
            content(dl, {30, 20});
            dl.popClip();
            dl.popClip();
        };
        DrawList dl;
        scene(dl);
        const Image full = render(dl);
        UI_CHECK(compare(full, render(DrawList{})).differing > W * H / 2);
        // scissored: bit exact inside, untouched outside - for windows through the middle of every group
        for (const Rect sc : {Rect{70, 50, 90, 70}, Rect{0, 0, 120, 90}, Rect{130, 100, 110, 80}, Rect{100, 60, 1, 1}, Rect{95.5f, 71.25f, 40.5f, 33.f}}) {
            const Image part = render(dl, W, H, sc, 0xFF112233u);
            const int x0 = int(std::floor(sc.x)), y0 = int(std::floor(sc.y)), x1 = int(std::ceil(sc.right())), y1 = int(std::ceil(sc.bottom()));
            Image over = full; // the full picture drawn over the same clear colour
            {
                Image bg;
                bg.resize(W, H, 0xFF112233u);
                CpuRasterizer().render(dl, bg);
                over = bg;
            }
            int bad = 0;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const bool in = x >= x0 && x < x1 && y >= y0 && y < y1;
                    if (in ? part.at(x, y) != over.at(x, y) : part.at(x, y) != 0xFF112233u) ++bad;
                }
            if (bad) std::fprintf(stderr, "  scissor %g %g %g %g: %d pixels differ\n", sc.x, sc.y, sc.w, sc.h, bad);
            UI_CHECK_EQ(bad, 0);
        }
        // translate: the same picture, moved (bit exact for whole pixels)
        DrawList movedList;
        scene(movedList);
        movedList.translate({-17.f, 9.f});
        const Image moved = render(movedList);
        UI_CHECK(compare(full, moved, 20, 0, W, H - 10, -17, 9).maxErr <= 1); // (the resampled group: the matrix is rebased)
        // scale: twice as big; every 2 x 2 block of the big picture is close to the pixel it came from
        DrawList bigList;
        scene(bigList);
        bigList.scale(2.f);
        const Image big = render(bigList, W * 2, H * 2);
        long err = 0;
        int worst = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int c = 0; c < 4; ++c) {
                    const int avg = (ch(big.at(x * 2, y * 2), c) + ch(big.at(x * 2 + 1, y * 2), c) + ch(big.at(x * 2, y * 2 + 1), c) + ch(big.at(x * 2 + 1, y * 2 + 1), c) + 2) / 4;
                    const int e = std::abs(avg - ch(full.at(x, y), c));
                    err += e;
                    worst = std::max(worst, e);
                }
        std::printf("scale(2): mean channel error %.3f, worst %d\n", double(err) / (W * H * 4.0), worst);
        UI_CHECK(double(err) / (W * H * 4.0) < 0.4);
        // rendering it again allocates nothing
        Image target;
        target.resize(W, H, 0);
        CpuRasterizer raster;
        raster.render(dl, target);
        g_allocs = 0;
        g_count = true;
        for (int k = 0; k < 5; ++k) raster.render(dl, target);
        g_count = false;
        UI_CHECK_EQ(g_allocs, 0L);
        // recording it again into a warm list allocates nothing either
        dl.clear();
        scene(dl);
        g_allocs = 0;
        g_count = true;
        dl.clear();
        scene(dl);
        g_count = false;
        UI_CHECK_EQ(g_allocs, 0L);
    }

    // ---- robustness ----
    {
        DrawList dl;
        dl.rect({0, 0, 50, 50}, Color::rgb(0xFFFFFF));
        const FilterOp huge{FilterKind::Blur, 1.0e9f, {}, {}};
        const FilterOp nan{FilterKind::DropShadow, std::nanf(""), {std::nanf(""), 1.0e30f}, Color{1, 1, 1, 1}};
        dl.pushFilter({&huge, 1}, {0, 0, 50, 50});
        dl.pushFilter({&nan, 1}, {0, 0, 1.0e9f, 1.0e9f});
        dl.rect({10, 10, 20, 20}, Color::rgb(0xFF0000));
        dl.popClip();
        dl.popClip();
        dl.pushTransform(Affine{std::nanf(""), 0, 0, 1, 0, 0}, {0, 0, 50, 50});
        dl.rect({10, 10, 20, 20}, Color::rgb(0xFF0000));
        dl.popClip();
        dl.pushTransform(Affine::scaling(1.0e-9f, 1.0e9f), {0, 0, 1.0e7f, 1.0e7f});
        dl.rect({10, 10, 20, 20}, Color::rgb(0xFF0000));
        dl.popClip();
        dl.pushTransform(Affine::rotation(0.3f), {-1.0e8f, -1.0e8f, 2.0e8f, 2.0e8f}); // enormous bounds: only the visible part is rendered
        dl.rect({-1.0e8f, -1.0e8f, 2.0e8f, 2.0e8f}, Color::rgb(0x00FF00));
        dl.popClip();
        dl.pushMask(Gradient::linear({0, 0}, {0, 0}, {}), {0, 0, 50, 50});
        dl.pushTransform(Affine::rotation(1.f), {0, 0, 50, 50}); // never closed
        dl.rect({10, 10, 20, 20}, Color::rgb(0xFF0000));
        Image img = render(dl, 96, 96);
        UI_CHECK(img.at(90, 90) == 0xFF00FF00u); // the enormous group still painted
        // side-record indices that point nowhere
        u32 seed = 7;
        for (int k = 0; k < 300; ++k) {
            DrawList bad;
            const FilterOp op{FilterKind::Blur, 2.f, {}, {}};
            bad.pushTransform(Affine::rotation(0.2f), {0, 0, 60, 60});
            bad.pushFilter({&op, 1}, {0, 0, 60, 60});
            bad.pushMask(Gradient::linear({0, 0}, {60, 0}, std::span<const GradientStop>{}), {0, 0, 60, 60});
            content(bad, {5, 5});
            bad.popClip();
            bad.popClip();
            bad.popClip();
            for (int e = 0; e < 3; ++e) {
                seed = seed * 1664525u + 1013904223u;
                DrawCmd& c = bad.cmds()[(seed >> 8) % bad.cmds().size()];
                switch ((seed >> 4) & 7) {
                case 0: c.data = seed; break;
                case 1: c.u = u16(seed >> 3); break;
                case 2: c.op = DrawOp((seed >> 12) % 16); break;
                case 3: c.rect = {f32(i32(seed >> 6) % 4000 - 2000), f32(i32(seed >> 9) % 4000 - 2000), f32((seed >> 2) % 5000), f32((seed >> 5) % 5000)}; break;
                case 4: c.gradient = u8(seed >> 7); break;
                case 5: c.image = seed; break;
                default: c.op = DrawOp::PopClip; break;
                }
            }
            bad.scale(1.5f);
            bad.translate({3.f, -2.f});
            (void)bad.hash();
            img = render(bad, 96, 96);
            Image out;
            rasterizeCommands(bad, 0, bad.size(), {0, 0, 40, 40}, 96, RasterSources{}, out);
        }
    }
    return finish("fuse_ui_draw_fx_groups");
}
