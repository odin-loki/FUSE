// fuse_ui_vector_golden: golden images of the vector drawing layer (behaviour gates: test_paths / test_painter /
// test_svg). One scene per ctest gate (`--scene=<name>`, ctest fuse_ui_vector_golden_<name>) against
// tests/golden/vector_<name>.png with the tolerance of fuse_ui_cpu_raster_golden; FUSE_UI_UPDATE_GOLDEN=1 rewrites the
// PNG, `--out=<dir>` also writes the rendered scene there. The scenes use no text, so one golden serves both text
// profiles. Scenes:
//   paths — fills (non-zero / even-odd star, curves, holes), strokes (caps, joins, miter limit, dashes, widths),
//           linear / radial / conic gradients with pad / repeat / reflect, gradient strokes, per-corner gradient rects,
//           clip to path and to a per-corner rounded rect, an opacity layer next to per-primitive opacity, rotated and
//           skewed Painter transforms, a 9-slice frame at three sizes
//   icons — the built-in icon set at 12 / 16 / 24 / 32 / 48 px, tinted, plus a row drawn at render scale 2 and SVG
//           documents with gradients / fills
#include "fuse/ui/painter.hpp"
#include "test_util.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
constexpr f32 kPi = 3.14159265358979f;
u8 ch(u32 p, int c) { return u8((p >> (8 * c)) & 0xFF); }
const Color kWhite = Color::rgb(0xFFFFFF);
const Color kBg = Color::rgb(0x1B1D23);
const Color kPanel = Color::rgb(0x262A33);
const Color kAccent = Color::rgb(0x3D8BFD);

Path star(Vec2 c, f32 r) {
    Path p;
    for (int i = 0; i < 5; ++i) {
        const f32 a = -kPi / 2.f + f32(i * 2) * 2.f * kPi / 5.f;
        const Vec2 v{c.x + r * std::cos(a), c.y + r * std::sin(a)};
        if (i == 0) p.moveTo(v);
        else p.lineTo(v);
    }
    return p.close();
}

void scenePaths(Image& img) {
    constexpr int W = 720, H = 520;
    Image frame; // 9-slice source: a 16 x 16 bevelled frame with distinct corners
    frame.resize(16, 16, 0);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const int d = std::min(std::min(x, y), std::min(15 - x, 15 - y));
            const bool corner = (x < 5 || x > 10) && (y < 5 || y > 10);
            u32 c = d == 0 ? 0xFF3D8BFDu : d < 3 ? 0xFF7A4A20u : d < 5 ? 0xFF50382Au : 0xFF2A2623u;
            if (corner && d >= 1 && d < 4 && ((x < 5) == (y < 5))) c = 0xFF40D0FFu;
            frame.pixels[std::size_t(y) * 16 + std::size_t(x)] = c;
        }
    const GradientStop warm[3]{{0.f, Color::rgb(0xFFB000)}, {0.5f, Color::rgb(0xFF3D6E)}, {1.f, Color::rgb(0x6A3DFF)}};
    const GradientStop fade[3]{{0.f, Color::rgb(0x3D8BFD)}, {0.7f, Color::rgb(0x3DFDB0, 0.8f)}, {1.f, Color::rgb(0x3DFDB0, 0.f)}};
    const GradientStop wheel[7]{{0.f, Color::rgb(0xFF0000)},    {1.f / 6, Color::rgb(0xFFFF00)}, {2.f / 6, Color::rgb(0x00FF00)},
                                {3.f / 6, Color::rgb(0x00FFFF)}, {4.f / 6, Color::rgb(0x0000FF)}, {5.f / 6, Color::rgb(0xFF00FF)},
                                {1.f, Color::rgb(0xFF0000)}};
    const GradientStop bw[2]{{0.f, Color::rgb(0x101010)}, {1.f, Color::rgb(0xF0F0F0)}};
    DrawList dl;
    dl.rect({0, 0, f32(W), f32(H)}, kBg);
    Painter p(dl);
    auto cell = [&](f32 x, f32 y, f32 w, f32 h) { p.fillRect({x, y, w, h}, kPanel); };

    // row 1: fills
    cell(10, 10, 700, 110);
    p.setBrush(kAccent);
    p.drawPath(star({65, 65}, 48));
    p.setFillRule(FillRule::EvenOdd);
    p.drawPath(star({175, 65}, 48));
    p.setFillRule(FillRule::NonZero);
    Path blob;
    blob.moveTo({240, 40}).cubicTo({280, 0}, {340, 30}, {330, 70}).quadTo({320, 115}, {270, 105}).cubicTo({235, 100}, {225, 60}, {240, 40}).close();
    p.setBrush(Gradient::linear({235, 30}, {335, 105}, warm));
    p.setPen(kWhite, 1.5f);
    p.drawPath(blob);
    p.setNoPen();
    Path ring;
    ring.addCircle({395, 65}, 44);
    ring.addCircle({395, 65}, 26); // same direction: a hole under even-odd
    ring.addRect({383, 53, 24, 24});
    p.setFillRule(FillRule::EvenOdd);
    p.setBrush(Color::rgb(0x3DFDB0));
    p.drawPath(ring);
    p.setFillRule(FillRule::NonZero);
    p.setBrush(Color::rgb(0xFFB000));
    p.drawEllipse({500, 65}, 46, 22);
    p.setBrush(Color::rgb(0xFF3D6E, 0.7f));
    p.drawEllipse({500, 65}, 22, 46);
    if (auto heart = Path::fromSvg("M40 78 C-20 30 10 -5 40 22 C70 -5 100 30 40 78 Z")) {
        p.save();
        p.translate(580, 15);
        p.scale(1.15f);
        p.setBrush(Gradient::radial({32, 28}, 52, warm));
        p.drawPath(*heart);
        p.restore();
    }

    // row 2: strokes
    cell(10, 130, 700, 120);
    p.setNoBrush();
    const LineCap caps[3]{LineCap::Butt, LineCap::Square, LineCap::Round};
    for (int i = 0; i < 3; ++i) {
        StrokeStyle st;
        st.width = 12.f;
        st.cap = caps[i];
        p.setPen(kAccent, st);
        Path l;
        l.moveTo({34, 150.f + 32.f * f32(i)}).lineTo({104, 150.f + 32.f * f32(i)});
        p.drawPath(l);
        p.setPen(kWhite, 1.f); // the centre line: shows where the caps extend
        p.drawPath(l);
    }
    const LineJoin joins[3]{LineJoin::Miter, LineJoin::Round, LineJoin::Bevel};
    for (int i = 0; i < 3; ++i) {
        StrokeStyle st;
        st.width = 12.f;
        st.join = joins[i];
        p.setPen(Color::rgb(0x3DFDB0), st);
        Path l;
        const f32 x = 140.f + 80.f * f32(i);
        l.moveTo({x, 230}).lineTo({x + 30, 160}).lineTo({x + 60, 230});
        p.drawPath(l);
    }
    for (int i = 0; i < 2; ++i) { // miter limit: 4 keeps the spike, 2 bevels it
        StrokeStyle st;
        st.width = 8.f;
        st.miterLimit = i == 0 ? 4.f : 2.f;
        p.setPen(Color::rgb(0xFFB000), st);
        Path l;
        const f32 y = 165.f + 50.f * f32(i);
        l.moveTo({390, y - 18}).lineTo({450, y}).lineTo({390, y + 18}); // 33 degrees: a miter ratio of 3.5
        p.drawPath(l);
    }
    {
        StrokeStyle st;
        st.width = 3.f;
        const f32 d1[2]{10.f, 6.f};
        st.setDashes(d1);
        p.setPen(kWhite, st);
        Path wave;
        wave.moveTo({480, 160});
        for (int i = 0; i < 4; ++i) wave.quadTo({495.f + 30.f * f32(i), i % 2 ? 190.f : 130.f}, {510.f + 30.f * f32(i), 160.f});
        p.drawPath(wave);
        st.cap = LineCap::Round;
        const f32 d2[2]{0.f, 8.f}; // dots
        st.setDashes(d2);
        st.width = 4.f;
        p.setPen(Color::rgb(0xFF3D6E), st);
        p.drawEllipse({520, 215}, 36, 18);
        const f32 d3[4]{14.f, 4.f, 2.f, 4.f};
        st.setDashes(d3, 3.f);
        st.cap = LineCap::Butt;
        st.width = 2.f;
        p.setPen(Color::rgb(0x3DFDB0), st);
        p.drawRoundedRect({580, 196, 110, 40}, 10.f);
        for (int i = 0; i < 5; ++i) { // hairline to heavy
            p.setPen(kWhite, 0.5f + 1.5f * f32(i));
            p.drawLine({612.f + 18.f * f32(i), 140}, {624.f + 18.f * f32(i), 184});
        }
    }
    p.setNoPen();

    // row 3: gradients
    cell(10, 260, 700, 120);
    dl.gradientRect({20, 270, 150, 28}, Gradient::linear({20, 0}, {170, 0}, warm));
    dl.gradientRect({20, 304, 150, 28}, Gradient::linear({20, 0}, {60, 0}, bw, GradientSpread::Repeat), {14, 14, 14, 14});
    dl.gradientRect({20, 338, 150, 28}, Gradient::linear({20, 0}, {60, 0}, bw, GradientSpread::Reflect), {0, 14, 0, 14});
    dl.gradientRect({185, 270, 100, 100}, Gradient::radial({235, 320}, 50, warm), {50, 50, 50, 50});
    dl.gradientRect({300, 270, 100, 100}, Gradient::radial({325, 295}, 22, fade, GradientSpread::Reflect), {8, 30, 8, 30});
    dl.gradientRect({415, 270, 100, 100}, Gradient::conic({465, 320}, 0.f, wheel), {50, 50, 50, 50});
    dl.gradientRect({530, 270, 80, 100}, Gradient::conic({570, 320}, kPi / 4.f, bw));
    {
        StrokeStyle st;
        st.width = 14.f;
        st.cap = LineCap::Round;
        Path arc;
        arc.addArc({665, 325}, 34, 34, kPi * 0.75f, kPi * 1.5f);
        p.strokePath(arc, st, Color::rgb(0x000000, 0.35f));
        p.strokePath(arc, st, Brush(Gradient::linear({631, 0}, {699, 0}, warm)));
    }

    // row 4: clips, layers, transforms, 9-slice
    cell(10, 390, 700, 120);
    p.save();
    p.clipPath(star({70, 450}, 56));
    p.fillRect({10, 390, 120, 120}, Gradient::conic({70, 450}, 0.f, wheel));
    for (int i = 0; i < 7; ++i) p.fillRect({14.f + 16.f * f32(i), 390, 6, 120}, Color::rgb(0x000000, 0.35f));
    p.restore();
    p.save();
    p.clipRoundedRect({140, 400, 100, 100}, {40, 4, 40, 4});
    p.fillRect({140, 400, 100, 100}, Gradient::linear({140, 400}, {240, 500}, warm));
    p.setBrush(kWhite);
    p.drawCircle({240, 400}, 44);
    p.restore();
    p.fillRect({256, 400, 108, 46}, kWhite); // group opacity (top) vs per-primitive opacity (bottom)
    p.fillRect({256, 454, 108, 46}, kWhite);
    p.beginLayer(0.5f, {256, 400, 108, 46});
    p.setBrush(Color::rgb(0xFF0000));
    p.drawCircle({292, 423}, 20);
    p.setBrush(Color::rgb(0x0000FF));
    p.drawCircle({328, 423}, 28);
    p.endLayer();
    p.save();
    p.setOpacity(0.5f);
    p.setBrush(Color::rgb(0xFF0000));
    p.drawCircle({292, 477}, 20);
    p.setBrush(Color::rgb(0x0000FF));
    p.drawCircle({328, 477}, 28);
    p.restore();
    for (int i = 0; i < 6; ++i) { // rotation about a pivot
        p.save();
        p.translate(430, 450);
        p.rotate(f32(i) * kPi / 6.f);
        p.setBrush(Color::rgb(0x3D8BFD, 0.55f));
        p.setPen(kWhite, 1.f);
        p.drawRoundedRect({-44, -9, 88, 18}, 6.f);
        p.restore();
    }
    p.save(); // skew + non-uniform scale: the pen is stretched with the shape
    p.transform({1.6f, 0.f, -0.5f, 0.8f, 530.f, 418.f});
    p.setBrush(Gradient::linear({0, 0}, {50, 50}, warm));
    p.setPen(kWhite, 3.f);
    p.drawRect({0, 0, 50, 50});
    p.setNoBrush();
    p.drawCircle({25, 25}, 14);
    p.restore();
    p.drawNinePatch(0, {604, 398, 26, 26}, Edges{5, 5, 5, 5});
    p.drawNinePatch(0, {636, 398, 68, 40}, Edges{5, 5, 5, 5});
    p.save();
    p.translate(604, 444);
    p.scale(2.f);
    p.drawNinePatch(0, {0, 0, 50, 29}, Edges{5, 5, 5, 5}); // borders scale with the painter
    p.restore();

    img.resize(W, H, 0xFF000000u);
    CpuRasterizer r;
    r.addImage(&frame);
    r.render(dl, img);
}

void sceneIcons(Image& img) {
    constexpr int W = 720, H = 420;
    DrawList dl;
    dl.rect({0, 0, f32(W), f32(H)}, kBg);
    {
        Painter p(dl);
        const f32 sizes[5]{12, 16, 24, 32, 48};
        f32 y = 12.f;
        for (int s = 0; s < 5; ++s) {
            const f32 size = sizes[s];
            for (u32 i = 0; i < kIconCount; ++i) {
                const f32 pitch = 36.f;
                const f32 x = 14.f + f32(i) * pitch + (pitch - 4.f - std::min(size, 32.f)) * 0.5f;
                if (size <= 32.f) drawIcon(p, Icon(i), {x, y, size, size}, s == 1 ? kAccent : kWhite);
            }
            if (size > 32.f) // the largest size: fewer, wider apart
                for (u32 i = 0; i < kIconCount; i += 2) drawIcon(p, Icon(i), {14.f + f32(i / 2) * 68.f, y, size, size}, Color::rgb(0x3DFDB0));
            y += size + 12.f;
        }
        // SVG documents with fills and gradients, at two sizes each
        auto logo = SvgImage::parse(R"svg(<svg viewBox="0 0 64 64">
  <defs>
    <linearGradient id="a" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#ffb000"/><stop offset="1" stop-color="#ff3d6e"/></linearGradient>
    <radialGradient id="b" cx="0.35" cy="0.35" r="0.8"><stop offset="0" stop-color="#fff"/><stop offset="1" stop-color="#3d8bfd"/></radialGradient>
  </defs>
  <rect x="2" y="2" width="60" height="60" rx="14" fill="url(#a)"/>
  <circle cx="32" cy="32" r="18" fill="url(#b)" stroke="#1b1d23" stroke-width="3"/>
  <path d="M22 34l7 7 14-17" fill="none" stroke="#1b1d23" stroke-width="5" stroke-linecap="round" stroke-linejoin="round"/>
</svg>)svg");
        auto chart = SvgImage::parse(R"svg(<svg viewBox="0 0 100 60" fill="none" stroke-linejoin="round">
  <g stroke="#3a3f4b" stroke-width="1"><line x1="5" y1="15" x2="95" y2="15"/><line x1="5" y1="30" x2="95" y2="30"/><line x1="5" y1="45" x2="95" y2="45"/></g>
  <polygon points="5,55 5,40 25,28 45,36 65,14 95,22 95,55" fill="#3d8bfd" fill-opacity="0.25" stroke="none"/>
  <polyline points="5,40 25,28 45,36 65,14 95,22" stroke="#3d8bfd" stroke-width="2.5"/>
  <polyline points="5,50 25,46 45,48 65,38 95,42" stroke="#3dfdb0" stroke-width="2" stroke-dasharray="5 3"/>
  <g fill="#fff" stroke="#3d8bfd" stroke-width="1.5"><circle cx="25" cy="28" r="3"/><circle cx="45" cy="36" r="3"/><circle cx="65" cy="14" r="3"/></g>
  <ellipse cx="82" cy="10" rx="12" ry="5" fill="currentColor" opacity="0.6" transform="rotate(-12 82 10)"/>
</svg>)svg");
        if (logo && chart) {
            logo->render(p, {14, y, 96, 96});
            logo->render(p, {120, y, 32, 32});
            logo->render(p, {120, y + 40, 16, 16});
            chart->render(p, {170, y, 200, 120}, Color::rgb(0xFFB000));
            chart->render(p, {380, y, 100, 60}, Color::rgb(0xFFB000));
            chart->render(p, {380, y + 66, 160, 40}, Color::rgb(0xFF3D6E), true); // stretched
        } else {
            ++failures();
        }
    }
    img.resize(W, H, 0xFF000000u);
    CpuRasterizer r;
    r.render(dl, img);
    // the same icons through the render scale: a 1x logical row recorded once, scaled to device pixels x2 by the list
    DrawList row;
    {
        Painter p(row);
        for (u32 i = 0; i < 7; ++i) drawIcon(p, Icon(i * 2 + 4), {4.f + f32(i) * 22.f, 4.f, 16.f, 16.f}, kWhite);
    }
    row.scale(2.f);
    row.translate({14.f - 8.f, 340.f - 8.f}); // device pixels: whole-pixel placement in the sheet
    r.render(row, img);
    DrawList rowFrac;
    {
        Painter p(rowFrac);
        for (u32 i = 0; i < 7; ++i) drawIcon(p, Icon(i * 2 + 4), {4.f + f32(i) * 22.f, 4.f, 16.f, 16.f}, kAccent);
    }
    rowFrac.scale(1.25f);
    rowFrac.translate({360.f - 5.f, 346.f - 5.f});
    r.render(rowFrac, img);
}
} // namespace

int main(int argc, char** argv) {
    std::string scene = "paths", out;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--scene=", 0) == 0) scene = a.substr(8);
        else if (a.rfind("--out=", 0) == 0) out = a.substr(6);
    }
    Image img;
    if (scene == "paths") scenePaths(img);
    else if (scene == "icons") sceneIcons(img);
    else {
        std::fprintf(stderr, "unknown scene %s\n", scene.c_str());
        return 2;
    }
    const std::string golden = "golden/vector_" + scene + ".png";
    if (!out.empty()) stbi_write_png((out + "/vector_" + scene + ".png").c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
    if (std::getenv("FUSE_UI_UPDATE_GOLDEN")) {
        stbi_write_png(golden.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("golden written: %s\n", golden.c_str());
        return finish("fuse_ui_vector_golden");
    }
    int w = 0, h = 0, n = 0;
    unsigned char* ref = stbi_load(golden.c_str(), &w, &h, &n, 4);
    UI_CHECK(ref != nullptr);
    if (ref) {
        UI_CHECK_EQ(w, img.width);
        UI_CHECK_EQ(h, img.height);
        if (w == img.width && h == img.height) {
            int bad = 0, maxd = 0;
            for (int i = 0; i < w * h; ++i)
                for (int c = 0; c < 4; ++c) {
                    const int d = std::abs(int(ref[i * 4 + c]) - int(ch(img.pixels[std::size_t(i)], c)));
                    maxd = std::max(maxd, d);
                    if (d > 3) { ++bad; break; }
                }
            std::printf("golden diff (%s): %d pixels > 3, max channel diff %d\n", scene.c_str(), bad, maxd);
            UI_CHECK(bad <= w * h / 1000); // <= 0.1 % of pixels may differ (compiler / libm rounding)
        }
        stbi_image_free(ref);
    }
    return finish("fuse_ui_vector_golden");
}
