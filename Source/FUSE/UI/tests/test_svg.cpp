// Gate fuse_ui_svg: the SVG subset (fuse/ui/svg.hpp), the built-in icons and the SVG widgets.
//   * documents: viewBox / size, groups and transforms, every shape element, paint (names, #hex, rgb(), none,
//     currentColor, style=""), inheritance, opacity, fill-rule, stroke properties, linear / radial gradients
//     (objectBoundingBox and userSpaceOnUse, href), unsupported elements counted and not drawn
//   * rendering through a Painter: fitted (aspect kept, centred) or stretched, at any size
//   * malformed input fails cleanly: a list of broken documents, every truncation of a valid one, random mutations
//   * the built-in icons: all parse, are named, and render inside their box at 16 / 24 / 48 px
//   * widgets: the SVG store (handles), `svgimage`, setIcon on buttons (measure + paint), render scale
#include "test_util.hpp"

#include <cmath>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
u8 ch(u32 p, int c) { return u8((p >> (8 * c)) & 0xFF); }

Image draw(const SvgImage& svg, int w, int h, Rect dst = {}, Color cc = Color::rgb(0xFFFFFF), bool stretch = false) {
    DrawList dl;
    Painter p(dl);
    svg.render(p, dst.empty() ? Rect{0, 0, f32(w), f32(h)} : dst, cc, stretch);
    Image img;
    img.resize(w, h, 0);
    CpuRasterizer r;
    r.render(dl, img);
    return img;
}
double area(const Image& img) {
    double a = 0.0;
    for (u32 p : img.pixels) a += double(p >> 24) / 255.0;
    return a;
}
bool nearRel(double v, double want, double rel) { return std::abs(v - want) <= rel * std::abs(want); }
SvgImage must(std::string_view xml) {
    auto r = SvgImage::parse(xml);
    if (!r) {
        std::fprintf(stderr, "parse failed at %u: %s\n", r.error().offset, r.error().message.c_str());
        ++failures();
        return {};
    }
    return std::move(*r);
}
int countOp(const DrawList& dl, DrawOp op, u32 node = 0xFFFFFFFFu) {
    int n = 0;
    for (const DrawCmd& c : dl.cmds()) n += c.op == op && (node == 0xFFFFFFFFu || c.node == node);
    return n;
}
} // namespace

int main() {
    // ---- a document with every shape ----
    {
        const SvgImage svg = must(R"svg(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">
<!-- every shape element -->
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
  <title>shapes &amp; more</title>
  <rect x="0" y="0" width="20" height="20" fill="red"/>
  <rect x="30" y="0" width="20" height="20" rx="6" fill="#00ff00"/>
  <circle cx="70" cy="10" r="10" fill="rgb(0, 0, 255)"/>
  <ellipse cx="20" cy="40" rx="18" ry="8" fill="#ff0"/>
  <line x1="50" y1="40" x2="90" y2="40" stroke="white" stroke-width="4"/>
  <polyline points="5,60 15,70 25,60" fill="none" stroke="#FF00FF" stroke-width="2"/>
  <polygon points="40,60 60,60 50,80" style="fill: cyan; stroke: none"/>
  <path d="M70 60h20v20h-20z" fill="rgba(255,255,255,0.5)"/>
  <g transform="translate(10 85) scale(2)">
    <rect width="5" height="5" fill="orange"/>
  </g>
</svg>)svg");
        UI_CHECK_EQ(svg.shapeCount(), 9u);
        UI_CHECK(svg.size() == (Size{100, 100}));
        UI_CHECK_RECT(svg.viewBox(), 0, 0, 100, 100);
        UI_CHECK_EQ(svg.unsupportedElements(), 0u);
        const Image img = draw(svg, 100, 100);
        UI_CHECK_EQ(img.at(10, 10), 0xFF0000FFu);
        UI_CHECK_EQ(img.at(40, 10), 0xFF00FF00u);
        UI_CHECK_EQ(img.at(30, 0), 0u); // rounded corner
        UI_CHECK_EQ(img.at(70, 10), 0xFFFF0000u);
        UI_CHECK_EQ(img.at(20, 40), 0xFF00FFFFu);
        UI_CHECK_EQ(img.at(70, 40), 0xFFFFFFFFu);
        UI_CHECK_EQ(img.at(70, 37), 0u);
        UI_CHECK(ch(img.at(15, 69), 0) > 200 && ch(img.at(15, 69), 2) > 200 && ch(img.at(15, 69), 1) < 40);
        UI_CHECK_EQ(img.at(15, 62), 0u); // the polyline is not filled
        UI_CHECK_EQ(img.at(50, 65), 0xFFFFFF00u);
        UI_CHECK(ch(img.at(80, 70), 3) == 128 && ch(img.at(80, 70), 0) == 128);
        UI_CHECK_EQ(img.at(15, 90), 0xFF00A5FFu); // the group's transform: 10x10 at (10, 85)
        UI_CHECK_EQ(img.at(21, 90), 0u);
        // the same image at 3x: every shape scales (vector, not pixels)
        const Image big = draw(svg, 300, 300);
        UI_CHECK_EQ(big.at(30, 30), 0xFF0000FFu);
        UI_CHECK_EQ(big.at(210, 30), 0xFFFF0000u);
        UI_CHECK(nearRel(area(big), area(img) * 9.0, 0.01));
    }
    // ---- inheritance, opacity, currentColor, fill rule, stroke properties ----
    {
        const SvgImage svg = must(R"svg(<svg viewBox="0 0 60 40" fill="blue" stroke-width="4">
  <g fill="red" opacity="0.5">
    <rect width="10" height="10"/>
    <rect x="10" width="10" height="10" fill="lime"/>
    <rect x="20" width="10" height="10" fill-opacity="0.5"/>
    <rect x="30" width="10" height="10" fill="none"/>
  </g>
  <rect x="40" width="10" height="10"/>
  <rect x="50" width="10" height="10" fill="currentColor"/>
  <g color="#ff0000"><rect y="10" width="10" height="10" fill="currentColor"/></g>
  <path fill-rule="evenodd" d="M10 20h20v20h-20z M15 25h10v10h-10z" fill="white"/>
  <path d="M35 20h20v20h-20z M40 25h10v10h-10z" fill="white"/>
  <line x1="2" y1="30" x2="8" y2="30" stroke="white" stroke-linecap="round"/>
  <rect x="100" width="10" height="10" display="none"/>
  <rect x="0" y="0" width="0" height="10"/>
</svg>)svg");
        UI_CHECK_EQ(svg.shapeCount(), 9u); // fill="none" without a stroke, display="none" and the empty rect draw nothing
        const Image img = draw(svg, 60, 40, {}, Color::rgb(0x00FFFF));
        UI_CHECK(ch(img.at(5, 5), 0) == 128 && ch(img.at(5, 5), 3) == 128);   // red at the group's opacity
        UI_CHECK(ch(img.at(15, 5), 1) == 128 && ch(img.at(15, 5), 0) == 0);    // overridden fill, still half
        UI_CHECK(ch(img.at(25, 5), 3) == 64);                                  // opacity x fill-opacity
        UI_CHECK_EQ(img.at(35, 5), 0u);
        UI_CHECK_EQ(img.at(45, 5), 0xFFFF0000u); // the root's fill is inherited
        UI_CHECK_EQ(img.at(55, 5), 0xFFFFFF00u); // currentColor = the render colour
        UI_CHECK_EQ(img.at(5, 15), 0xFF0000FFu); // currentColor = the `color` property when the document sets it
        UI_CHECK_EQ(img.at(20, 30), 0u);         // even-odd hole
        UI_CHECK_EQ(img.at(45, 30), 0xFFFFFFFFu);
        UI_CHECK(ch(img.at(1, 30), 3) > 150);    // round cap beyond the line's end (inherited stroke-width 4)
    }
    // ---- gradients ----
    {
        const SvgImage svg = must(R"svg(<svg viewBox="0 0 100 60">
  <defs>
    <linearGradient id="lg"><stop offset="0" stop-color="#000"/><stop offset="100%" stop-color="#fff"/></linearGradient>
    <linearGradient id="vert" href="#lg" x1="0" y1="0" x2="0" y2="1"/>
    <radialGradient id="rg" gradientUnits="userSpaceOnUse" cx="75" cy="45" r="10">
      <stop offset="0" stop-color="red"/><stop offset="1" style="stop-color: blue; stop-opacity: 0.5"/>
    </radialGradient>
    <rect id="hidden" width="100" height="60" fill="white"/>
  </defs>
  <rect x="10" y="5" width="80" height="10" fill="url(#lg)"/>
  <rect x="10" y="20" width="20" height="40" fill="url(#vert)"/>
  <circle cx="75" cy="45" r="10" fill="url(#rg)"/>
  <rect x="40" y="25" width="10" height="10" fill="url(#missing)"/>
</svg>)svg");
        UI_CHECK_EQ(svg.gradientCount(), 3u);
        UI_CHECK_EQ(svg.shapeCount(), 4u); // the rect in <defs> is not drawn
        const Image img = draw(svg, 100, 60);
        UI_CHECK(ch(img.at(10, 10), 0) <= 4 && ch(img.at(89, 10), 0) >= 250); // across the shape's bounding box
        UI_CHECK(std::abs(int(ch(img.at(50, 10), 0)) - 129) <= 3);
        UI_CHECK(ch(img.at(20, 21), 0) <= 12 && ch(img.at(20, 58), 0) >= 240); // href'd stops, vertical
        UI_CHECK(std::abs(int(ch(img.at(12, 40), 0)) - int(ch(img.at(27, 40), 0))) <= 1);
        UI_CHECK(ch(img.at(75, 45), 0) > 235 && ch(img.at(75, 45), 2) < 20 && ch(img.at(75, 45), 3) > 240);
        UI_CHECK(ch(img.at(83, 45), 2) > ch(img.at(83, 45), 0)); // towards blue at half opacity near the rim
        UI_CHECK(ch(img.at(83, 45), 3) < 200);
        UI_CHECK_EQ(img.at(45, 30), 0u); // an unresolved reference paints nothing
        UI_CHECK_EQ(img.at(5, 50), 0u);
    }
    // ---- fitting ----
    {
        const SvgImage svg = must(R"svg(<svg viewBox="10 10 20 20"><rect x="10" y="10" width="20" height="20" fill="white"/></svg>)svg");
        UI_CHECK(svg.size() == (Size{20, 20}));
        Image img = draw(svg, 100, 40); // meet: a 40 px square centred horizontally
        UI_CHECK_EQ(img.at(30, 0), 0xFFFFFFFFu);
        UI_CHECK_EQ(img.at(69, 39), 0xFFFFFFFFu);
        UI_CHECK_EQ(img.at(29, 20), 0u);
        UI_CHECK_EQ(img.at(70, 20), 0u);
        img = draw(svg, 100, 40, {}, Color::rgb(0xFFFFFF), true); // stretch
        UI_CHECK(area(img) == 4000.0);
        img = draw(svg, 100, 40, {50, 10, 10, 10});
        UI_CHECK(area(img) == 100.0 && img.at(50, 10) == 0xFFFFFFFFu && img.at(59, 19) == 0xFFFFFFFFu);
        // no viewBox: width / height; neither: the bounds of the shapes
        const SvgImage wh = must(R"svg(<svg width="40" height="20"><rect width="40" height="20" fill="white"/></svg>)svg");
        UI_CHECK_RECT(wh.viewBox(), 0, 0, 40, 20);
        const SvgImage bare = must(R"svg(<svg><circle cx="50" cy="50" r="10" fill="white"/></svg>)svg");
        UI_CHECK(std::abs(bare.viewBox().x - 40.f) < 0.01f && std::abs(bare.viewBox().w - 20.f) < 0.01f);
        const SvgImage pct = must(R"svg(<svg width="100%" height="100%" viewBox="0 0 8 4"><rect width="8" height="4"/></svg>)svg");
        UI_CHECK(pct.size() == (Size{8, 4}));
        // transforms: rotate about a point, matrix, nested
        const SvgImage rot = must(R"svg(<svg viewBox="0 0 40 40">
  <g transform="rotate(90 20 20)"><rect x="20" y="18" width="18" height="4" fill="white"/></g>
  <rect transform="matrix(0 1 -1 0 10 0)" x="2" y="2" width="6" height="2" fill="white"/>
</svg>)svg");
        img = draw(rot, 40, 40);
        UI_CHECK_EQ(img.at(20, 30), 0xFFFFFFFFu); // the bar now points down from the centre
        UI_CHECK_EQ(img.at(30, 20), 0u);
        UI_CHECK_EQ(img.at(7, 5), 0xFFFFFFFFu); // matrix: (x, y) -> (10 - y, x)
        UI_CHECK(nearRel(area(img), 72.0 + 12.0, 0.002));
    }
    // ---- unsupported content is counted, never drawn, never an error ----
    {
        const SvgImage svg = must(R"svg(<svg viewBox="0 0 10 10">
  <style>rect { fill: red }</style>
  <text x="1" y="5">hi <tspan>there</tspan></text>
  <use href="#a"/><image href="x.png" width="10" height="10"/>
  <clipPath id="c"><rect width="5" height="5"/></clipPath>
  <symbol id="s"><rect width="10" height="10"/></symbol>
  <unknownElement><rect x="8" y="8" width="2" height="2" fill="white"/></unknownElement>
  <![CDATA[ <rect width="10" height="10"/> ]]>
</svg>)svg");
        UI_CHECK_EQ(svg.unsupportedElements(), 6u);
        UI_CHECK_EQ(svg.shapeCount(), 1u); // only the rect inside the unknown (pass-through) element
        UI_CHECK(area(draw(svg, 10, 10)) == 4.0);
    }
    // ---- malformed input ----
    {
        struct Bad {
            const char* xml;
            SvgErrorKind kind;
        };
        const Bad bad[] = {
            {"", SvgErrorKind::NoSvgRoot},
            {"   \n ", SvgErrorKind::NoSvgRoot},
            {"just text", SvgErrorKind::NoSvgRoot},
            {"<html><svg/></html>", SvgErrorKind::NoSvgRoot},
            {"<svg>", SvgErrorKind::Syntax},
            {"<svg><g></svg>", SvgErrorKind::Syntax},
            {"<svg></g></svg>", SvgErrorKind::Syntax},
            {"</svg>", SvgErrorKind::Syntax},
            {"<svg", SvgErrorKind::Syntax},
            {"<svg viewBox=\"0 0 1 1></svg>", SvgErrorKind::Syntax},
            {"<svg viewBox=0></svg>", SvgErrorKind::Syntax},
            {"<svg><rect width=\"1\" height></svg>", SvgErrorKind::Syntax},
            {"<svg><!-- never closed </svg>", SvgErrorKind::Syntax},
            {"<svg><![CDATA[ never closed </svg>", SvgErrorKind::Syntax},
            {"<svg><?pi never closed </svg>", SvgErrorKind::Syntax},
            {"<svg>< rect/></svg>", SvgErrorKind::Syntax},
            {"<svg><rect /x></svg>", SvgErrorKind::Syntax},
            {"<svg><path d=\"M0 0 L\"/></svg>", SvgErrorKind::BadValue},
            {"<svg><path d=\"hello\"/></svg>", SvgErrorKind::BadValue},
            {"<svg><polygon points=\"1,2 x,4\"/></svg>", SvgErrorKind::BadValue},
        };
        for (const Bad& b : bad) {
            auto r = SvgImage::parse(b.xml);
            if (r || r.error().kind != b.kind) std::fprintf(stderr, "malformed case not rejected as expected: %s\n", b.xml);
            UI_CHECK(!r.has_value() && r.error().kind == b.kind && !r.error().message.empty());
            if (!r) UI_CHECK(r.error().offset <= std::string_view(b.xml).size());
        }
        // nesting limit
        std::string deep = "<svg>";
        for (int i = 0; i < 100; ++i) deep += "<g>";
        for (int i = 0; i < 100; ++i) deep += "</g>";
        deep += "</svg>";
        auto dr = SvgImage::parse(deep);
        UI_CHECK(!dr && dr.error().kind == SvgErrorKind::Limit);
        // lenient where SVG is: bad colours, numbers and transforms fall back, the document still loads
        const SvgImage soft = must(R"svg(<svg viewBox="0 0 10 10"><rect width="10" height="10" fill="notacolor" stroke-width="abc"
            transform="rotate(" opacity="2" stroke-dasharray="1,-2"/><circle r="-5" fill="red"/><rect width="1e99" height="5"/></svg>)svg");
        UI_CHECK_EQ(soft.shapeCount(), 1u);
        UI_CHECK_EQ(draw(soft, 10, 10).at(5, 5), 0xFF000000u); // the default black fill
        // every truncation of a valid document and random mutations: an error or an image, never a crash or a hang
        const std::string_view valid = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor">
<defs><linearGradient id="g" x1="0" x2="1"><stop offset="0" stop-color="red"/><stop offset="1" stop-color="#00f"/></linearGradient></defs>
<g transform="translate(1,1) rotate(15 12 12)" stroke-width="2" stroke-dasharray="3 1">
<path d="M3 7a2 2 0 0 1 2-2h4l2 2.5h8a2 2 0 0 1 2 2V17a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" fill="url(#g)"/>
<circle cx="12" cy="12" r="3" style="fill:rgb(10,20,30);opacity:.5"/><polyline points="1,2 3,4 5,6"/></g></svg>)svg";
        UI_CHECK(SvgImage::parse(valid).has_value());
        int ok = 0, rejected = 0;
        for (std::size_t n = 0; n < valid.size(); ++n) {
            auto r = SvgImage::parse(valid.substr(0, n));
            (r ? ok : rejected)++;
            if (r) (void)draw(*r, 24, 24);
        }
        UI_CHECK_EQ(ok, 0); // no proper prefix of this document is complete
        u32 seed = 12345u;
        auto rnd = [&] { return seed = seed * 1664525u + 1013904223u; };
        std::string mutated;
        int parsed = 0;
        for (int it = 0; it < 3000; ++it) {
            mutated.assign(valid);
            const int edits = 1 + int(rnd() % 4);
            for (int e = 0; e < edits; ++e) {
                const std::size_t at = rnd() % mutated.size();
                switch (rnd() % 4) {
                case 0: mutated[at] = char(rnd() % 256); break;
                case 1: mutated.erase(at, 1 + rnd() % 8); break;
                case 2: mutated.insert(at, 1, "<>\"'/= #().-eE0123456789zMaA"[rnd() % 29]); break;
                default: mutated.insert(at, mutated, rnd() % mutated.size(), rnd() % 16); break;
                }
                if (mutated.empty()) mutated = "<";
            }
            auto r = SvgImage::parse(mutated);
            if (r) {
                ++parsed;
                const Image img = draw(*r, 24, 24); // whatever parsed must also render
                UI_CHECK(img.width == 24);
            }
        }
        std::printf("fuzz: %d truncations rejected, %d / 3000 mutations still parsed and rendered\n", rejected, parsed);
        UI_CHECK(parsed > 0 && parsed < 3000);
    }
    // ---- built-in icons ----
    {
        for (u32 i = 0; i < kIconCount; ++i) {
            const Icon icon = Icon(i);
            const std::string_view name = iconName(icon);
            UI_CHECK(!name.empty());
            Icon found = Icon::Count;
            UI_CHECK(findIcon(name, found) && found == icon);
            auto parsed = SvgImage::parse(iconSvg(icon)); // the source is a complete, valid document
            UI_CHECK(parsed.has_value());
            const SvgImage& img = iconImage(icon);
            UI_CHECK(!img.empty() && img.unsupportedElements() == 0);
            UI_CHECK_RECT(img.viewBox(), 0, 0, 24, 24);
            double a24 = 0.0;
            for (int size : {16, 24, 48}) {
                const int pad = 4;
                const Image r = draw(img, size + 2 * pad, size + 2 * pad, {f32(pad), f32(pad), f32(size), f32(size)});
                const double a = area(r), box = double(size) * size;
                if (!(a > 0.03 * box && a < 0.6 * box)) std::fprintf(stderr, "icon %.*s at %d px covers %.1f%%\n", int(name.size()), name.data(), size, 100.0 * a / box);
                UI_CHECK(a > 0.03 * box && a < 0.6 * box);
                double outside = 0.0; // stays inside its box
                for (int y = 0; y < r.height; ++y)
                    for (int x = 0; x < r.width; ++x)
                        if (x < pad || y < pad || x >= pad + size || y >= pad + size) outside += double(r.at(x, y) >> 24);
                UI_CHECK(outside == 0.0);
                if (size == 24) a24 = a;
                if (size == 48) UI_CHECK(nearRel(a, a24 * 4.0, 0.03)); // the 2 px stroke scales with the icon
            }
        }
        Icon none = Icon::Check;
        UI_CHECK(!findIcon("no-such-icon", none) && none == Icon::Check);
        // a few shapes, exactly: the plus is two 14 x 2 bars with round caps; the check's elbow is where it was drawn
        const Image plus = draw(iconImage(Icon::Plus), 24, 24);
        UI_CHECK_EQ(plus.at(12, 12), 0xFFFFFFFFu);
        UI_CHECK_EQ(plus.at(6, 11), 0xFFFFFFFFu);
        UI_CHECK_EQ(plus.at(11, 17), 0xFFFFFFFFu);
        UI_CHECK_EQ(plus.at(6, 6), 0u);
        UI_CHECK(nearRel(area(plus), 2.0 * 14.0 * 2.0 - 4.0 + 2.0 * 3.14159, 0.01));
        const Image tinted = draw(iconImage(Icon::Minus), 24, 24, {}, Color::rgb(0xFF8000));
        UI_CHECK_EQ(tinted.at(12, 12), 0xFF0080FFu); // currentColor
        DrawList dl;
        Painter p(dl);
        drawIcon(p, Icon::Close, {0, 0, 16, 16}, Color::rgb(0xFFFFFF));
        UI_CHECK(dl.size() == 1 && dl.cmds()[0].op == DrawOp::Path && dl.paths()[0].stroked);
        UI_CHECK(std::abs(dl.paths()[0].stroke.width - 2.f * 16.f / 24.f) < 1e-4f);
    }
    // ---- widgets ----
    {
        auto bad = w::svgCreate("<svg><g></svg>");
        UI_CHECK(!bad.has_value());
        auto h = w::svgCreate(R"svg(<svg width="30" height="20" viewBox="0 0 30 20"><rect width="30" height="20" fill="currentColor"/></svg>)svg");
        UI_CHECK(h.has_value() && h->valid());
        const w::SvgHandle svg = *h;
        UI_CHECK(w::svgGet(svg) && w::svgGet(svg)->shapeCount() == 1);
        UI_CHECK(w::iconHandle(Icon::Save) == w::iconHandle(Icon::Save) && w::svgGet(w::iconHandle(Icon::Save)) != nullptr);

        UiContext ui({.viewport = {200, 200}, .font = &font()});
        w::install(ui);
        ui.setCss(ui.root(), "padding", "10px");
        ui.setCss(ui.root(), "gap", "6px");
        ui.setCss(ui.root(), "align-items", "start");
        const NodeHandle img = w::svgImage(ui, ui.root(), svg, 0.f, 0.f, "img");
        ui.setCss(img, "color", "#ff0000");
        const NodeHandle plain = w::button(ui, ui.root(), "Save", "plain");
        const NodeHandle withIcon = w::button(ui, ui.root(), "Save", "icon");
        const NodeHandle only = w::iconButton(ui, ui.root(), Icon::Settings, {}, "gear");
        ui.update();
        UI_CHECK(ui.rect(img).w == 30.f && ui.rect(img).h == 20.f); // intrinsic size
        UI_CHECK(w::svgImageOf(ui, img) == svg);
        const Rect before = ui.rect(withIcon);
        UI_CHECK(before.w == ui.rect(plain).w);
        UI_CHECK_EQ(countOp(ui.drawList(), DrawOp::Path, plain.index), 0);
        UI_CHECK_EQ(countOp(ui.drawList(), DrawOp::Path, img.index), 1);
        UI_CHECK(countOp(ui.drawList(), DrawOp::Path, only.index) >= 2);
        const u64 plainHash = ui.drawList().hash();
        w::setIcon(ui, withIcon, Icon::Save);
        ui.update();
        UI_CHECK(ui.rect(withIcon).w == before.w + 16.f + 6.f); // icon + gap
        UI_CHECK(ui.rect(withIcon).h == before.h + std::max(0.f, 16.f - ui.contentRect(plain).h)); // at least the icon's height
        UI_CHECK(countOp(ui.drawList(), DrawOp::Path, withIcon.index) >= 3);
        UI_CHECK(w::iconOf(ui, withIcon) == w::iconHandle(Icon::Save));
        UI_CHECK(!w::iconOf(ui, plain).valid());
        { // the icon sits left of the text, inside the button
            f32 iconRight = 0.f, textLeft = 1e9f;
            for (const DrawCmd& c : ui.drawList().cmds()) {
                if (c.node != withIcon.index) continue;
                if (c.op == DrawOp::Path) iconRight = std::max(iconRight, c.rect.right());
                if (c.op == DrawOp::Glyph) textLeft = std::min(textLeft, c.rect.x);
            }
            UI_CHECK(iconRight <= textLeft + 2.f && iconRight > ui.contentRect(withIcon).x);
            UI_CHECK(textLeft < ui.contentRect(withIcon).right());
        }
        UI_CHECK(ui.rect(only).w >= 16.f && ui.rect(only).h >= 16.f);
        std::string a11y;
        ui.dumpA11y(a11y);
        UI_CHECK(a11y.find("settings") != std::string::npos); // an icon-only button is named after its icon
        // rendered: the image widget in its `color`, the icon in the button's text colour
        Image out;
        render(ui, out);
        const Rect ir = ui.rect(img);
        UI_CHECK_EQ(out.at(int(ir.x) + 15, int(ir.y) + 10), 0xFF0000FFu);
        w::clearIcon(ui, withIcon);
        ui.update();
        UI_CHECK(ui.rect(withIcon).w == before.w);
        UI_CHECK_EQ(ui.drawList().hash(), plainHash); // back to the exact list from before the icon
        // a destroyed image: the widget draws nothing (stale handle), and does not crash
        w::setSvgImage(ui, img, w::iconHandle(Icon::Info));
        ui.update();
        UI_CHECK(countOp(ui.drawList(), DrawOp::Path, img.index) >= 3);
        w::setSvgImage(ui, img, svg);
        w::svgDestroy(svg);
        UI_CHECK(w::svgGet(svg) == nullptr);
        ui.update();
        UI_CHECK_EQ(countOp(ui.drawList(), DrawOp::Path, img.index), 0);
        w::svgDestroy(w::iconHandle(Icon::Info)); // built-in icons cannot be destroyed
        UI_CHECK(w::svgGet(w::iconHandle(Icon::Info)) != nullptr);
        w::forgetIcons(ui);
        UI_CHECK(!w::iconOf(ui, only).valid());

        // render scale 2: the same logical layout, paths in device pixels (sharp), 4x the covered pixels
        auto iconFrame = [&](f32 scale, Image& o) {
            UiContext u({.viewport = {64, 64}, .font = &font()});
            w::install(u);
            u.setRenderScale(scale);
            const NodeHandle n = w::svgImage(u, u.root(), w::iconHandle(Icon::Search), 48.f, 48.f);
            u.setCss(n, "margin", "8px");
            u.update();
            UI_CHECK_RECT(u.rect(n), 8, 8, 48, 48); // logical
            o.resize(int(64 * scale), int(64 * scale), 0xFF000000u);
            CpuRasterizer r;
            r.render(u.drawList(), o);
        };
        Image s1, s2;
        iconFrame(1.f, s1);
        iconFrame(2.f, s2);
        // coverage = how far a pixel is from the background towards the icon colour (the brightest pixel)
        const int bg = int(s1.at(0, 0) & 0xFF);
        int fg = bg;
        for (u32 p : s1.pixels) fg = std::max(fg, int(p & 0xFF));
        UI_CHECK(fg > bg + 100);
        double a1 = 0.0, a2 = 0.0;
        int partial1 = 0, partial2 = 0;
        auto scan = [&](const Image& im, double& a, int& partial) {
            for (u32 p : im.pixels) {
                const int v = int(p & 0xFF) - bg;
                a += double(v) / double(fg - bg);
                partial += v > 8 && v < fg - bg - 8;
            }
        };
        scan(s1, a1, partial1);
        scan(s2, a2, partial2);
        UI_CHECK(a1 > 100.0 && nearRel(a2, a1 * 4.0, 0.01));
        UI_CHECK(partial1 > 50 && partial2 < partial1 * 3); // antialiased edge pixels grow with the perimeter (2x), not the area
    }
    return finish("fuse_ui_svg");
}
