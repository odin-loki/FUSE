// Gate fuse_ui_css_position: position relative / absolute / fixed / sticky with insets; z-index stacking contexts
// (paint order and hit testing, negative z, nested contexts); flex-wrap + align-content; row / column gaps and the
// two-value gap shorthand; aspect-ratio; grid masonry and subgrid; large mask textures; @scope proximity.
#include "test_util.hpp"
#include "fuse/ui/css_masking.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
bool near(f32 a, f32 b, f32 eps = 0.5f) { return std::fabs(a - b) < eps; }
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
u32 px(const Image& img, f32 x, f32 y) { return img.at(int(x), int(y)) & 0xFFFFFFu; }
void dump(const char* what, Rect r) { std::fprintf(stderr, "%s: %g %g %g %g\n", what, r.x, r.y, r.w, r.h); }
} // namespace

int main() {
    // ---- relative / absolute / fixed ----
    {
        UiContext ui({.viewport = {400, 300}});
        ui.styles().addSheet(sheet(".c { position: relative; width: 200px; height: 100px; margin-left: 50px; flex-shrink: 0; } "
                                   ".r { position: relative; left: 10px; top: 5px; width: 20px; height: 20px; flex-shrink: 0; } "
                                   ".a { position: absolute; right: 10px; bottom: 10px; width: 30px; height: 30px; } "
                                   ".s { position: absolute; inset: 10px 20px; } "
                                   ".f { position: fixed; left: 5px; top: 7px; width: 10px; height: 10px; } "
                                   ".ar { width: 80px; aspect-ratio: 2 / 1; flex-shrink: 0; }"));
        const NodeHandle c = make(ui, ui.root(), "c"), r = make(ui, c, "r"), r2 = make(ui, c, "ar");
        const NodeHandle a = make(ui, c, "a"), s = make(ui, c, "s"), f = make(ui, c, "f");
        ui.update(0.f);
        const Rect rc = ui.rect(c), rr = ui.rect(r), ra = ui.rect(a), rs = ui.rect(s), rf = ui.rect(f), rar = ui.rect(r2);
        dump("c", rc), dump("r", rr), dump("a", ra), dump("s", rs), dump("f", rf), dump("ar", rar);
        UI_CHECK(near(rr.x, rc.x + 10) && near(rr.y, rc.y + 5));
        UI_CHECK(near(rar.y, rc.y + 20) && near(rar.h, 40)); // relative keeps its flow slot; aspect-ratio
        UI_CHECK(near(ra.x, rc.x + rc.w - 40) && near(ra.y, rc.y + rc.h - 40) && near(ra.w, 30));
        UI_CHECK(near(rs.x, rc.x + 20) && near(rs.y, rc.y + 10) && near(rs.w, rc.w - 40) && near(rs.h, rc.h - 20));
        UI_CHECK(near(rf.x, 5) && near(rf.y, 7));
    }
    // ---- sticky ----
    {
        UiContext ui({.viewport = {400, 300}});
        ui.styles().addSheet(sheet(".sc { overflow: scroll; height: 100px; width: 200px; flex-shrink: 0; } "
                                   ".in { height: 400px; flex-shrink: 0; } "
                                   ".k { position: sticky; top: 0px; height: 20px; flex-shrink: 0; } .gap { height: 50px; flex-shrink: 0; }"));
        const NodeHandle sc = make(ui, ui.root(), "sc"), in = make(ui, sc, "in");
        make(ui, in, "gap");
        const NodeHandle k = make(ui, in, "k");
        ui.update(0.f);
        const Rect r0 = ui.rect(k), rsc = ui.rect(sc);
        dump("k0", r0);
        UI_CHECK(near(r0.y, rsc.y + 50));
        ui.scrollTo(sc, {0, 120});
        ui.update(0.f);
        dump("k1", ui.rect(k));
        UI_CHECK(near(ui.rect(k).y, rsc.y)); // stuck at the scrollport top
    }
    // ---- z-index painting and hit testing ----
    {
        UiContext ui({.viewport = {300, 200}});
        ui.styles().addSheet(sheet(".b { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px; } "
                                   ".red { background: #f00; z-index: 2; } .grn { background: #0f0; z-index: 1; } "
                                   ".blu { background: #00f; z-index: -1; left: 50px; } "
                                   ".ctx { position: absolute; left: 150px; top: 0px; width: 100px; height: 100px; z-index: 1; } "
                                   ".hi { background: #fff; z-index: 99; } .over { position: absolute; left: 150px; top: 0px; width: 100px; height: 100px; background: #f0f; z-index: 2; }"));
        const NodeHandle red = make(ui, ui.root(), "b"), grn = make(ui, ui.root(), "b"), blu = make(ui, ui.root(), "b");
        ui.addClass(red, "red"), ui.addClass(grn, "grn"), ui.addClass(blu, "blu");
        const NodeHandle ctx = make(ui, ui.root(), "ctx"), hi = make(ui, ctx, "b");
        ui.addClass(hi, "hi");
        const NodeHandle over = make(ui, ui.root(), "over");
        ui.update(0.f);
        Image img = render(ui, 300, 200);
        std::fprintf(stderr, "z px %06x %06x %06x\n", px(img, 20, 20), px(img, 90, 20), px(img, 200, 50));
        UI_CHECK(px(img, 20, 20) == 0x0000FFu || px(img, 20, 20) == 0xFF0000u); // red (z 2) over green, either byte order
        UI_CHECK(ui.hitTest({20, 20}) == red && ui.hitTest({90, 20}) == red);
        UI_CHECK(ui.hitTest({120, 20}) == blu); // negative z: only where nothing else is
        UI_CHECK(ui.hitTest({200, 50}) == over); // z 99 is trapped in ctx (z 1) under over (z 2)
        (void)grn, (void)hi;
    }
    // ---- flex-wrap, align-content, gaps ----
    {
        UiContext ui({.viewport = {400, 300}});
        ui.styles().addSheet(sheet(".w { flex-direction: row; flex-wrap: wrap; width: 100px; height: 200px; gap: 10px 4px; "
                                   "align-content: space-between; flex-shrink: 0; } .i { width: 30px; height: 20px; flex-shrink: 0; }"));
        const NodeHandle w = make(ui, ui.root(), "w");
        NodeHandle it[5];
        for (auto& n : it) n = make(ui, w, "i");
        ui.update(0.f);
        const Rect rw = ui.rect(w);
        for (auto n : it) dump("i", ui.rect(n));
        UI_CHECK(near(ui.rect(it[1]).x, rw.x + 34) && near(ui.rect(it[2]).x, rw.x + 68)); // column gap 4
        UI_CHECK(near(ui.rect(it[3]).x, rw.x) && near(ui.rect(it[3]).y, rw.y + 180));     // 2nd line at the end
        UI_CHECK(near(ui.rect(it[0]).y, rw.y));
        UI_CHECK(near(ui.computed(w).rowGap, 10) && near(ui.computed(w).columnGap, 4));
    }
    // ---- grid: gaps, masonry, subgrid ----
    {
        UiContext ui({.viewport = {400, 400}});
        ui.styles().addSheet(sheet(".g { layout: grid; grid-template-columns: 100px 100px; grid-template-rows: masonry; gap: 10px; width: 210px; flex-shrink: 0; } "
                                   ".t { height: 50px; } .s { height: 20px; } "
                                   ".h { layout: grid; grid-template-columns: 60px 140px; width: 200px; flex-shrink: 0; } "
                                   ".sub { layout: grid; grid-template-columns: subgrid; grid-column-span: 2; }"));
        const NodeHandle g = make(ui, ui.root(), "g"), a = make(ui, g, "t"), b = make(ui, g, "s"), c = make(ui, g, "s");
        ui.update(0.f);
        const Rect rg = ui.rect(g);
        dump("ma", ui.rect(a)), dump("mb", ui.rect(b)), dump("mc", ui.rect(c));
        UI_CHECK(near(ui.rect(b).x, rg.x + 110) && near(ui.rect(c).x, rg.x + 110) && near(ui.rect(c).y, ui.rect(b).y + 30));
        const NodeHandle h = make(ui, ui.root(), "h"), sub = make(ui, h, "sub"), x = make(ui, sub, ""), y = make(ui, sub, "");
        ui.update(0.f);
        dump("sub", ui.rect(sub)), dump("x", ui.rect(x)), dump("y", ui.rect(y));
        UI_CHECK(near(ui.rect(x).w, 60) && near(ui.rect(y).w, 140) && near(ui.rect(y).x, ui.rect(h).x + 60));
    }
    // ---- large mask texture ----
    {
        UiContext ui({.viewport = {500, 500}});
        ui.styles().addSheet(sheet(".m { width: 400px; height: 300px; flex-shrink: 0; background: #f00; mask-image: linear-gradient(to right, black, transparent); }"));
        const NodeHandle m = make(ui, ui.root(), "m");
        ui.update(0.f);
        ClipLayer L;
        UI_CHECK(buildMaskClip(ui.computed(m), ui.rect(m), [](std::string_view) -> const Image* { return nullptr; }, L));
        std::fprintf(stderr, "tex %d %d\n", L.texW, L.texH);
        UI_CHECK(L.texW * L.texH <= kMaxMaskTexelArea && L.texW >= 256 && L.texW > L.texH);
        UI_CHECK(ui.hitTest({ui.rect(m).x + 20, ui.rect(m).y + 20}) == m && !(ui.hitTest({ui.rect(m).x + 380, ui.rect(m).y + 20}) == m));
    }
    // ---- @scope proximity ----
    {
        UiContext ui({.viewport = {400, 300}});
        ui.styles().addSheet(sheet("@scope (.light) { .t { opacity: 0.25; } } @scope (.dark) { .t { opacity: 0.5; } } "
                                   "@scope (#id) { .u { opacity: 0.75; } } .u { opacity: 0.5; } .v.v { opacity: 0.5; } @scope (.x) { .v { opacity: 0.25; } }"));
        const NodeHandle light = make(ui, ui.root(), "light"), dark = make(ui, light, "dark"), t = make(ui, dark, "t");
        const NodeHandle dark2 = make(ui, ui.root(), "dark"), light2 = make(ui, dark2, "light"), t2 = make(ui, light2, "t");
        const NodeHandle x = make(ui, ui.root(), "x"), v = make(ui, x, "v");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(t).opacity, 0.5f, 1e-3f) && near(ui.computed(t2).opacity, 0.25f, 1e-3f)); // nearer root wins
        UI_CHECK(near(ui.computed(v).opacity, 0.25f, 1e-3f)); // proximity beats specificity
        (void)light, (void)dark2;
        const StyleSheet& sh = ui.styles().sheet(0);
        for (const auto& r : sh.rules()) std::fprintf(stderr, "spec %06x\n", r.specificity);
        UI_CHECK(sh.rules()[0].specificity == 0x100 && sh.rules()[2].specificity == 0x100); // implicit root adds none
    }
    return fuse::ui::test::finish("fuse_ui_css_position");
}
