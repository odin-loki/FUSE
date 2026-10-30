// Gate fuse_ui_css_masking: clip-path path() / url() / reference boxes / polygons past 8 points; mask-* (gradient and
// image layers, mask-mode luminance, mask-composite, size / position / repeat, the mask shorthand; the texture clip
// layer on the CPU rasteriser; masks leave hit testing alone); container / viewport units; @container style();
// @media hover / pointer / resolution; @scope (root, limit, :scope); the cooked round trip of all of it.
#include "test_util.hpp"
#include "fuse/ui/css_masking.hpp"
#include "fuse/ui/doc/cook.hpp"

#include <cmath>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) < eps; }
StyleSheet sheet(std::string_view css) {
    std::vector<StyleDiagnostic> d;
    auto s = StyleSheet::parse(css, &d);
    UI_CHECK(s.has_value());
    for (const auto& x : d) std::fprintf(stderr, "diag line %u: %s\n", x.line, x.message.c_str());
    UI_CHECK(d.empty());
    return s ? std::move(*s) : StyleSheet{};
}
NodeHandle make(UiContext& ui, NodeHandle p, std::string_view cls) {
    const NodeHandle n = ui.create(ui.registry().find("panel"), p);
    if (!cls.empty()) ui.addClass(n, cls);
    return n;
}
Image render(UiContext& ui, int w, int h) {
    Image img;
    img.resize(w, h, 0xFF000000u);
    CpuRasterizer().render(ui.drawList(), img);
    return img;
}
int red(const Image& img, f32 x, f32 y) { return int(img.at(int(x), int(y)) & 0xFF); }
} // namespace

int main() {
    // ---- clip-path: path(), polygons past the old 8-point cap, reference boxes, url() ----
    {
        UiContext ui({.viewport = {400, 500}});
        std::string star = "polygon(";
        for (int k = 0; k < 20; ++k) { // 20-gon: a near-circle of radius 50 about (50, 50)
            const f32 a = f32(k) * 6.2831853f / 20.f;
            star += (k ? ", " : "") + std::to_string(50.f + 50.f * std::cos(a)) + "px " + std::to_string(50.f + 50.f * std::sin(a)) + "px";
        }
        star += ")";
        ui.styles().addSheet(sheet(".s { width: 100px; height: 100px; flex-shrink: 0; background: #f00; } "
                                   ".p { clip-path: path('M 0 0 L 100 0 L 0 100 Z'); } "
                                   ".q { padding: 20px; clip-path: content-box; } "
                                   ".u { clip-path: url(#tri); } .m { clip-path: url(#missing); }"));
        UI_CHECK(ui.styles().sheet(0).rules().size() == 5);
        const NodeHandle g = make(ui, ui.root(), "s"), p = make(ui, ui.root(), "s"), q = make(ui, ui.root(), "s");
        UI_CHECK(ui.setCss(g, "clip-path", star));
        ui.addClass(p, "p");
        ui.addClass(q, "q");
        ui.update(0.f);
        const Rect rg = ui.rect(g), rp = ui.rect(p), rq = ui.rect(q);
        Image img = render(ui, 400, 500);
        UI_CHECK(red(img, rg.x + 50, rg.y + 50) > 200 && red(img, rg.x + 95, rg.y + 50) > 150 && red(img, rg.x + 4, rg.y + 4) < 30);
        UI_CHECK(red(img, rp.x + 20, rp.y + 20) > 200 && red(img, rp.x + 80, rp.y + 80) < 30);
        UI_CHECK(red(img, rq.x + 50, rq.y + 50) > 200 && red(img, rq.x + 10, rq.y + 50) < 30); // padding clipped
        UI_CHECK(ui.hitTest({rp.x + 20, rp.y + 20}) == p && !(ui.hitTest({rp.x + 80, rp.y + 80}) == p));
        UI_CHECK(ui.hitTest({rq.x + 50, rq.y + 50}) == q && !(ui.hitTest({rq.x + 10, rq.y + 50}) == q));
        UI_CHECK(ui.hitTest({rg.x + 50, rg.y + 50}) == g && !(ui.hitTest({rg.x + 4, rg.y + 4}) == g));
        // url(): defined (objectBoundingBox triangle), missing (no clip)
        ui.removeClass(p, "p");
        ui.addClass(p, "u");
        ui.removeClass(q, "q");
        ui.addClass(q, "m");
        ui.defineClipPath("tri", "M 0 0 L 1 0 L 0 1 Z", false, true);
        ui.update(0.f);
        img = render(ui, 400, 500);
        const Rect rp2 = ui.rect(p), rq2 = ui.rect(q);
        UI_CHECK(red(img, rp2.x + 20, rp2.y + 20) > 200 && red(img, rp2.x + 80, rp2.y + 80) < 30);
        UI_CHECK(red(img, rq2.x + 5, rq2.y + 5) > 200 && red(img, rq2.x + 95, rq2.y + 95) > 200);
        UI_CHECK(!(ui.hitTest({rp2.x + 80, rp2.y + 80}) == p) && ui.hitTest({rq2.x + 95, rq2.y + 95}) == q);
    }
    // ---- mask texture clip layer (DrawList level) ----
    {
        DrawList dl;
        ClipLayer L;
        L.gradient = 3;
        L.path.rect({0, 0, 40, 10});
        L.texW = 4, L.texH = 1;
        L.texels = {0.f, 1.f, 1.f, 0.f};
        L.toUnit = {0.1f, 0.f, 0.f, 0.1f, 0.f, 0.f}; // 10 px per texel
        dl.pushClipLayers(std::span<const ClipLayer>(&L, 1));
        dl.rect({0, 0, 40, 10}, Color{1.f, 0.f, 0.f, 1.f});
        dl.popClip();
        Image img;
        img.resize(40, 10, 0xFF000000u);
        CpuRasterizer().render(dl, img);
        UI_CHECK(red(img, 2, 5) < 10 && red(img, 15, 5) > 245 && red(img, 24, 5) > 245 && red(img, 38, 5) < 10);
        UI_CHECK(red(img, 10, 5) > 100 && red(img, 10, 5) < 160); // bilinear halfway between texels 0 and 1
    }
    // ---- mask-* ----
    {
        PropValue v;
        for (const char* ok : {"none", "linear-gradient(to right, #000, transparent)", "url(a), radial-gradient(circle 10px at 0 0, red, blue 50%)",
                               "repeating-linear-gradient(45deg, #000 0 10px, transparent 10px 20px)"})
            UI_CHECK(parsePropValue(Prop::MaskImage, ok, v));
        for (const char* bad : {"linear-gradient()", "foo(1)", "linear-gradient(red)"}) UI_CHECK(!parsePropValue(Prop::MaskImage, bad, v));
        UI_CHECK(parsePropValue(Prop::MaskComposite, "add, subtract, intersect, exclude", v) && !parsePropValue(Prop::MaskComposite, "xor", v));
        UI_CHECK(parsePropValue(Prop::MaskRepeat, "repeat-x, no-repeat space", v) && parsePropValue(Prop::MaskClip, "no-clip", v));
        UI_CHECK(parsePropValue(Prop::MaskSize, "cover, 10px auto, 50%", v) && parsePropValue(Prop::MaskPosition, "right 10px, center", v));

        UiContext ui({.viewport = {400, 500}});
        ui.styles().addSheet(sheet(".s { width: 100px; height: 100px; flex-shrink: 0; background: #f00; } "
                                   ".g { mask-image: linear-gradient(to right, #000, #000 50%, transparent 50%); } "
                                   ".i { mask: url(lum) 0 0 / 100% 100% luminance; } "
                                   ".c { mask: linear-gradient(to right, #000 50%, transparent 50%) intersect, linear-gradient(#000 50%, transparent 50%); } "
                                   ".n { mask: linear-gradient(#000, #000) 100% 0 / 50% 50% no-repeat; }"));
        const NodeHandle g = make(ui, ui.root(), "s"), i = make(ui, ui.root(), "s"), c = make(ui, ui.root(), "s"), n = make(ui, ui.root(), "s");
        ui.addClass(g, "g"), ui.addClass(i, "i"), ui.addClass(c, "c"), ui.addClass(n, "n");
        Image lum; // left half white, right half black (opaque): luminance mask keeps the left
        lum.resize(2, 1, 0xFF000000u);
        lum.pixels[0] = 0xFFFFFFFFu;
        ui.defineMaskImage("lum", lum);
        ui.update(0.f);
        const Image img = render(ui, 400, 500);
        const Rect rg = ui.rect(g), ri = ui.rect(i), rc = ui.rect(c), rn = ui.rect(n);
        UI_CHECK(red(img, rg.x + 20, rg.y + 50) > 245 && red(img, rg.x + 80, rg.y + 50) < 10);
        UI_CHECK(red(img, ri.x + 20, ri.y + 50) > 245 && red(img, ri.x + 80, ri.y + 50) < 10);
        UI_CHECK(red(img, rc.x + 20, rc.y + 20) > 245 && red(img, rc.x + 80, rc.y + 20) < 10 && red(img, rc.x + 20, rc.y + 80) < 10);
        UI_CHECK(red(img, rn.x + 80, rn.y + 20) > 245 && red(img, rn.x + 20, rn.y + 20) < 10 && red(img, rn.x + 80, rn.y + 80) < 10);
        UI_CHECK(ui.hitTest({rg.x + 80, rg.y + 50}) == g); // masks do not clip hit testing
        const std::vector<MaskLayer>& layers = maskLayers(ui.computed(c));
        UI_CHECK(layers.size() == 2 && layers[0].composite == 2 && layers[1].composite == 0);
        bool tex = false;
        for (const DrawCmd& d : ui.drawList().cmds()) tex = tex || d.op == DrawOp::ClipPath;
        UI_CHECK(tex);
    }
    // ---- container / viewport units ----
    {
        UiContext ui({.viewport = {400, 300}});
        ui.styles().addSheet(sheet(".box { container-type: inline-size; width: 200px; height: 50px; flex-shrink: 0; } "
                                   ".half { width: 50cqw; height: 10px; flex-shrink: 0; } .vp { width: 10vw; height: 5vh; flex-shrink: 0; } "
                                   ".mn { width: 10vmin; height: 10px; flex-shrink: 0; }"));
        const NodeHandle box = make(ui, ui.root(), "box"), half = make(ui, box, "half");
        const NodeHandle vp = make(ui, ui.root(), "vp"), mn = make(ui, ui.root(), "mn");
        const NodeHandle inl = make(ui, box, "");
        UI_CHECK(ui.setCss(inl, "width", "25cqi") && ui.setCss(inl, "height", "1px") && ui.setCss(inl, "flex-shrink", "0"));
        ui.update(0.f);
        UI_CHECK(near(ui.rect(half).w, 100.f) && near(ui.rect(inl).w, 50.f));
        UI_CHECK(near(ui.rect(vp).w, 40.f) && near(ui.rect(vp).h, 15.f) && near(ui.rect(mn).w, 30.f));
        UI_CHECK(ui.setCss(box, "width", "300px"));
        ui.update(0.f);
        UI_CHECK(near(ui.rect(half).w, 150.f) && near(ui.rect(inl).w, 75.f));
        ui.setViewport({600, 200});
        ui.update(0.f);
        UI_CHECK(near(ui.rect(vp).w, 60.f) && near(ui.rect(vp).h, 10.f) && near(ui.rect(mn).w, 20.f));
    }
    // ---- @container style() ----
    {
        UiContext ui({.viewport = {400, 300}});
        ui.styles().addSheet(sheet(".dark { --theme: dark; } .named { container-name: card; --size: big; } "
                                   "@container style(--theme: dark) { .t { opacity: 0.5; } } "
                                   "@container card style(--size) { .u { opacity: 0.25; } } "
                                   "@container not style(--theme: dark) { .v { opacity: 0.75; } }"));
        const NodeHandle outer = make(ui, ui.root(), "dark"), mid = make(ui, outer, ""), t = make(ui, mid, "t");
        const NodeHandle named = make(ui, ui.root(), "named"), inner = make(ui, named, ""), u = make(ui, inner, "u");
        const NodeHandle v = make(ui, ui.root(), "v");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(t).opacity, 0.5f)); // --theme inherited by mid
        UI_CHECK(near(ui.computed(u).opacity, 0.25f) && near(ui.computed(v).opacity, 0.75f));
        ui.removeClass(outer, "dark");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(t).opacity, 1.f)); // deep invalidation through the ancestor's class change
    }
    // ---- @media hover / pointer / resolution ----
    {
        UiContext ui({.viewport = {400, 300}});
        ui.styles().addSheet(sheet("@media (hover: hover) and (pointer: fine) { .a { opacity: 0.5; } } "
                                   "@media (any-pointer: coarse) { .b { opacity: 0.5; } } @media (hover: none) { .c { opacity: 0.5; } } "
                                   "@media (min-resolution: 2dppx) { .d { opacity: 0.5; } } @media (resolution < 144dpi) { .e { opacity: 0.5; } } "
                                   "@media (pointer) { .f { opacity: 0.5; } }"));
        const NodeHandle a = make(ui, ui.root(), "a"), b = make(ui, ui.root(), "b"), c = make(ui, ui.root(), "c");
        const NodeHandle d = make(ui, ui.root(), "d"), e = make(ui, ui.root(), "e"), f = make(ui, ui.root(), "f");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(a).opacity, 0.5f) && near(ui.computed(b).opacity, 1.f) && near(ui.computed(c).opacity, 1.f));
        UI_CHECK(near(ui.computed(d).opacity, 1.f) && near(ui.computed(e).opacity, 0.5f) && near(ui.computed(f).opacity, 0.5f));
        ui.setInputMedia(false, false, 1, 1);
        ui.setMediaResolution(2.f);
        ui.update(0.f);
        UI_CHECK(near(ui.computed(a).opacity, 1.f) && near(ui.computed(b).opacity, 0.5f) && near(ui.computed(c).opacity, 0.5f));
        UI_CHECK(near(ui.computed(d).opacity, 0.5f) && near(ui.computed(e).opacity, 1.f));
        ui.setInputMedia(false, false, 0, 0);
        ui.update(0.f);
        UI_CHECK(near(ui.computed(f).opacity, 1.f));
    }
    // ---- @scope ----
    {
        UiContext ui({.viewport = {400, 300}});
        ui.styles().addSheet(sheet("@scope (.card) to (.content) { .t { opacity: 0.5; } :scope > .k { opacity: 0.25; } opacity: 0.75; } "
                                   ".outer { @scope { .t2 { opacity: 0.5; } } }"));
        const NodeHandle card = make(ui, ui.root(), "card"), t = make(ui, card, "t"), k = make(ui, card, "k");
        const NodeHandle content = make(ui, card, "content"), tIn = make(ui, content, "t"), tLimit = make(ui, card, "t");
        ui.addClass(tLimit, "content");
        const NodeHandle tOut = make(ui, ui.root(), "t");
        const NodeHandle outer = make(ui, ui.root(), "outer"), t2 = make(ui, outer, "t2"), t2Out = make(ui, ui.root(), "t2");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(card).opacity, 0.75f) && near(ui.computed(t).opacity, 0.5f) && near(ui.computed(k).opacity, 0.25f));
        UI_CHECK(near(ui.computed(tIn).opacity, 1.f) && near(ui.computed(tLimit).opacity, 1.f) && near(ui.computed(tOut).opacity, 1.f));
        UI_CHECK(near(ui.computed(t2).opacity, 0.5f) && near(ui.computed(t2Out).opacity, 1.f));
        ui.removeClass(content, "content"); // the limit goes: tIn is in scope
        ui.update(0.f);
        UI_CHECK(near(ui.computed(tIn).opacity, 0.5f));
    }
    // ---- cooked round trip ----
    {
        const char* css = "@scope (.card) to (.content) { .t { opacity: 0.5; } } "
                          ".dark { --theme: dark; } @container style(--theme: dark) { .s { opacity: 0.25; } } "
                          "@media (pointer: fine) { .w { width: 50vw; height: 10px; flex-shrink: 0; clip-path: path('M0 0 L10 0 L0 10 Z'); "
                          "mask: linear-gradient(#000, transparent) luminance; } }";
        auto bytes = doc::cookStyle(css);
        UI_CHECK(bytes.has_value());
        auto back = bytes ? doc::loadCookedStyle(*bytes) : std::unexpected(std::string("no bytes"));
        UI_CHECK(back.has_value());
        if (back) {
            UI_CHECK_EQ(back->scopes().size(), std::size_t(1));
            UI_CHECK(back->scopes()[0].roots.size() == 1 && back->scopes()[0].limits.size() == 1);
            UiContext ui({.viewport = {400, 300}});
            ui.styles().addSheet(std::move(*back));
            const NodeHandle card = make(ui, ui.root(), "card"), t = make(ui, card, "t"), content = make(ui, card, "content");
            const NodeHandle tIn = make(ui, content, "t");
            const NodeHandle dark = make(ui, ui.root(), "dark"), s = make(ui, dark, "s"), w = make(ui, ui.root(), "w");
            ui.update(0.f);
            UI_CHECK(near(ui.computed(t).opacity, 0.5f) && near(ui.computed(tIn).opacity, 1.f) && near(ui.computed(s).opacity, 0.25f));
            UI_CHECK(near(ui.rect(w).w, 200.f));
            UI_CHECK(clipPathSpec(ui.computed(w).clipPath).kind == ClipPathSpec::Kind::Path);
            const auto& ml = maskLayers(ui.computed(w));
            UI_CHECK(ml.size() == 1 && ml[0].mode == 2 && ml[0].kind == MaskLayer::Kind::Linear);
        }
    }
    return fuse::ui::test::finish("fuse_ui_css_masking");
}
