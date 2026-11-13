// Gate fuse_ui_css_cascade: CSS Nesting (& and implicit descendant), Cascade Layers (@layer block / statement /
// anonymous / nested, unlayered last), @media (viewport width / height / orientation / prefers-*; re-evaluated on
// viewport change), @supports, @container (container-type / container-name / container shorthand; re-styled after
// the container's size changes), clip-path basic shapes (CPU paint + hit testing), and the cooked round trip.
#include "test_util.hpp"
#include "fuse/ui/doc/cook.hpp"

#include <cmath>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
bool near(f32 a, f32 b) { return std::fabs(a - b) < 1e-3f; }
StyleSheet sheet(std::string_view css) {
    std::vector<StyleDiagnostic> d;
    auto s = StyleSheet::parse(css, &d);
    UI_CHECK(s.has_value());
    return s ? std::move(*s) : StyleSheet{};
}
NodeHandle make(UiContext& ui, NodeHandle p, std::string_view cls) {
    const NodeHandle n = ui.create(ui.registry().find("panel"), p);
    if (!cls.empty()) ui.addClass(n, cls);
    return n;
}
} // namespace

int main() {
    // ---- nesting ----
    {
        UiContext ui({.viewport = {400, 300}});
        ui.styles().addSheet(sheet(".a { opacity: 0.5; .b { opacity: 0.25; } &.c { opacity: 0.75; } > .d { opacity: 0.125; } "
                                   "& + .e, .x & { opacity: 0.375; } }"));
        const NodeHandle a = make(ui, ui.root(), "a"), b = make(ui, a, "b"), d = make(ui, a, "d");
        const NodeHandle ac = make(ui, ui.root(), "a");
        ui.addClass(ac, "c");
        const NodeHandle e = make(ui, ui.root(), "e"), bOut = make(ui, ui.root(), "b");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(a).opacity, 0.5f) && near(ui.computed(b).opacity, 0.25f) && near(ui.computed(d).opacity, 0.125f));
        UI_CHECK(near(ui.computed(ac).opacity, 0.75f) && near(ui.computed(e).opacity, 0.375f) && near(ui.computed(bOut).opacity, 1.f));
        // own declarations after nested rules still belong to the parent rule
        auto s = StyleSheet::parse(".p { .q { opacity: 0.1; } opacity: 0.2; }");
        UI_CHECK(s && s->rules().size() == 2 && s->rules()[0].chain.size() == 1 && s->rules()[1].chain.size() == 2);
        std::vector<StyleDiagnostic> diags;
        UI_CHECK(!StyleSheet::parse(".p { .q { opacity: 0.1; }", &diags)); // unbalanced
        UI_CHECK(!StyleSheet::parse("width: 1px;"));                        // declaration at top level
    }
    // ---- @layer ----
    {
        UiContext ui({.viewport = {400, 300}});
        // order: base, theme (theme.dark before theme's own rules), then unlayered — specificity only within a rank
        ui.styles().addSheet(sheet("@layer base, theme; #n { opacity: 0.9; } "
                                   "@layer theme { .k.k.k { opacity: 0.3; } @layer dark { .k.k.k.k { opacity: 0.2; } } } "
                                   "@layer base { #n.k { opacity: 0.1; } } "
                                   "@layer { .z { opacity: 0.6; } } @layer theme.dark { .z { opacity: 0.4; } }"));
        const NodeHandle n = ui.create(ui.registry().find("panel"), ui.root(), "n");
        ui.addClass(n, "k");
        const NodeHandle m = make(ui, ui.root(), "k");
        const NodeHandle z = make(ui, ui.root(), "z");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(n).opacity, 0.9f)); // unlayered wins over any layered specificity
        UI_CHECK(near(ui.computed(m).opacity, 0.3f)); // theme's own rule after theme.dark
        UI_CHECK(near(ui.computed(z).opacity, 0.6f)); // anonymous layer declared after theme
        UI_CHECK_EQ(ui.styles().sheet(0).layers().size(), std::size_t(4));
        // a later sheet's layers merge by name: base keeps its rank (lowest)
        ui.styles().addSheet(sheet("@layer base { .z { opacity: 0.05; } }"));
        ui.update(0.f);
        UI_CHECK(near(ui.computed(z).opacity, 0.6f));
    }
    // ---- @media ----
    {
        UiContext ui({.viewport = {400, 300}});
        ui.styles().addSheet(sheet(".m { opacity: 1; } @media (min-width: 600px) { .m { opacity: 0.5; } } "
                                   "@media (width < 300px), print { .m { opacity: 0.25; } } "
                                   "@media screen and (orientation: portrait) { .m { opacity: 0.75; } } "
                                   "@media (400px <= width <= 500px) and (prefers-color-scheme: dark) { .m { opacity: 0.125; } } "
                                   "@media not all and (prefers-reduced-motion: reduce) { .r { opacity: 0.5; } } "
                                   "@media (min-width: 100px) { @media (max-height: 200px) { .m { opacity: 0.375; } } } "
                                   "@media (bogus-feature: 1) { .m { opacity: 0; } }"));
        const NodeHandle m = make(ui, ui.root(), "m"), r = make(ui, ui.root(), "r");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(m).opacity, 1.f) && near(ui.computed(r).opacity, 0.5f));
        ui.setViewport({700, 300});
        ui.update(0.f);
        UI_CHECK(near(ui.computed(m).opacity, 0.5f));
        ui.setViewport({200, 300});
        ui.update(0.f);
        UI_CHECK(near(ui.computed(m).opacity, 0.75f)); // portrait (later) beats width < 300
        ui.setViewport({250, 200});
        ui.update(0.f);
        UI_CHECK(near(ui.computed(m).opacity, 0.375f)); // nested @media: both hold
        ui.setViewport({450, 300});
        ui.update(0.f);
        UI_CHECK(near(ui.computed(m).opacity, 1.f));
        ui.setMediaPreferences(true, true);
        ui.update(0.f);
        UI_CHECK(near(ui.computed(m).opacity, 0.125f) && near(ui.computed(r).opacity, 1.f));
        const u32 v = ui.styles().sheetVersion();
        ui.setViewport({460, 300}); // no result changed: no restyle
        ui.update(0.f);
        UI_CHECK_EQ(ui.styles().sheetVersion(), v);
    }
    // ---- @supports ----
    {
        auto s = StyleSheet::parse("@supports (opacity: 0.5) { .a { opacity: 0.5; } } @supports (opacity: banana) { .b {} } "
                                   "@supports not (fake-prop: 1) { .c {} } @supports (display: flex) and (not (x: y)) { .d {} } "
                                   "@supports (x: y) or selector(a > b) { .e {} } @supports selector(:bogus) { .f {} } "
                                   "@supports (margin: 1px 2px) { .g {} }");
        UI_CHECK(s.has_value());
        std::string got;
        if (s) for (const auto& r : s->rules()) got += char(int(r.chain[0].classes[0] == hashId("a")) ? 'a' : r.chain[0].classes[0] == hashId("c") ? 'c' : r.chain[0].classes[0] == hashId("d") ? 'd' : r.chain[0].classes[0] == hashId("e") ? 'e' : r.chain[0].classes[0] == hashId("g") ? 'g' : '?');
        UI_CHECK_EQ(got, std::string("acdeg"));
        UI_CHECK_EQ(s->rules().size(), std::size_t(5)); // a c d e g
    }
    // ---- @container ----
    {
        UiContext ui({.viewport = {800, 300}});
        ui.styles().addSheet(sheet(".w { container: card / inline-size; } .h { container-type: size; } "
                                   ".t { opacity: 1; } @container (min-width: 300px) { .t { opacity: 0.5; } } "
                                   "@container card (width < 200px) { .t { opacity: 0.25; } } "
                                   "@container (min-height: 50px) { .u { opacity: 0.75; } }"));
        const NodeHandle w = make(ui, ui.root(), "w");
        UI_CHECK(ui.setCss(w, "width", "400px") && ui.setCss(w, "height", "100px") && ui.setCss(w, "flex-shrink", "0"));
        const NodeHandle t = make(ui, w, "t"), u = make(ui, w, "u");
        const NodeHandle free = make(ui, ui.root(), "t");
        ui.update(0.f);
        UI_CHECK(ui.styles().usesContainerQueries());
        UI_CHECK_EQ(ui.computed(w).containerType, u8(2));
        UI_CHECK(ui.computed(w).containerNameCount == 1 && ui.computed(w).containerName[0] == animationNameId("card"));
        UI_CHECK(near(ui.computed(t).opacity, 0.5f)); // settled in the same update
        UI_CHECK(near(ui.computed(free).opacity, 1.f)); // no container: false
        UI_CHECK(near(ui.computed(u).opacity, 1.f));    // inline-size can't answer height
        UI_CHECK(ui.setCss(w, "width", "150px"));
        ui.update(0.f);
        UI_CHECK(near(ui.computed(t).opacity, 0.25f));
        UI_CHECK(ui.setCss(w, "container-type", "size"));
        ui.update(0.f);
        UI_CHECK(near(ui.computed(u).opacity, 0.75f));
        UI_CHECK(ui.setCss(w, "container-name", "none"));
        ui.update(0.f);
        UI_CHECK(near(ui.computed(t).opacity, 1.f)); // card gone; 150 < 300
    }
    // ---- clip-path ----
    {
        PropValue v;
        for (const char* ok : {"none", "inset(10px)", "inset(5% 10px round 4px)", "circle()", "circle(50px at left top)", "circle(closest-side at 20% 30%)",
                               "ellipse(40px 20px)", "ellipse(farthest-side 10% at center)", "polygon(0 0, 100% 0, 50% 100%)",
                               "polygon(evenodd, 0 0, 10px 0, 10px 10px) border-box", "url(#x)", "padding-box", "path('M 0 0 L 10 0 L 0 10 Z')", "path(evenodd, 'M0 0h10v10z') content-box"})
            UI_CHECK(parsePropValue(Prop::ClipPath, ok, v));
        for (const char* bad : {"inset()", "circle(1px 2px)", "polygon(0 0, 1px 1px)", "polygon(0 0 0, 1 1, 2 2)", "url()", "path('')", "path('M 0 0 Q')", "inset(1px) circle()", "border-box padding-box", "ellipse(1px)"})
            UI_CHECK(!parsePropValue(Prop::ClipPath, bad, v));
        UiContext ui({.viewport = {200, 200}});
        ui.styles().addSheet(sheet(".c { width: 100px; height: 100px; flex-shrink: 0; background: #f00; clip-path: circle(50px); } "
                                   ".k { width: 100px; height: 100px; flex-shrink: 0; background: #f00; clip-path: polygon(0 0, 100% 0, 0 100%); }"));
        const NodeHandle c = make(ui, ui.root(), "c"), k = make(ui, ui.root(), "k");
        const NodeHandle child = make(ui, k, "");
        UI_CHECK(ui.setCss(child, "width", "100px") && ui.setCss(child, "height", "100px") && ui.setCss(child, "background", "#00f"));
        ui.update(0.f);
        const Rect rc = ui.rect(c), rk = ui.rect(k);
        Image img;
        img.resize(200, 200, 0xFF000000u);
        CpuRasterizer().render(ui.drawList(), img);
        auto R = [&](f32 x, f32 y) { return int(img.at(int(x), int(y)) & 0xFF); };
        auto B = [&](f32 x, f32 y) { return int((img.at(int(x), int(y)) >> 16) & 0xFF); };
        UI_CHECK(R(rc.x + 50, rc.y + 50) > 200 && R(rc.x + 3, rc.y + 3) < 30); // corner outside the circle
        UI_CHECK(B(rk.x + 20, rk.y + 20) > 200 && B(rk.x + 80, rk.y + 80) < 30); // child clipped by the parent's triangle
        UI_CHECK(ui.hitTest({rc.x + 50, rc.y + 50}) == c && ui.hitTest({rc.x + 3, rc.y + 3}) != c);
        UI_CHECK(ui.hitTest({rk.x + 20, rk.y + 20}) == child && !(ui.hitTest({rk.x + 80, rk.y + 80}) == child));
        bool clipCmd = false;
        for (const DrawCmd& d : ui.drawList().cmds()) clipCmd = clipCmd || d.op == DrawOp::ClipPath;
        UI_CHECK(clipCmd);
    }
    // ---- cooked round trip ----
    {
        const char* css = "@layer a, b; @layer b { .x { opacity: 0.5; } } @layer a { .x { opacity: 0.25; } } "
                          "@media (min-width: 500px) { @container c (height > 10px) { .y { opacity: 0.5; } } }";
        auto bytes = doc::cookStyle(css);
        UI_CHECK(bytes.has_value());
        auto back = doc::loadCookedStyle(*bytes);
        UI_CHECK(back.has_value());
        if (back) {
            UI_CHECK_EQ(back->layers().size(), std::size_t(2));
            UI_CHECK_EQ(back->conditions().size(), std::size_t(2));
            UI_CHECK(back->conditions()[1].parent == 0 && back->conditions()[1].kind == StyleCondition::Kind::Container);
            UI_CHECK(back->rules()[0].layer == 1 && back->rules()[2].cond == 1);
            UI_CHECK(back->conditions()[1].containerName == animationNameId("c"));
            UiContext ui({.viewport = {400, 300}});
            ui.styles().addSheet(std::move(*back));
            const NodeHandle x = make(ui, ui.root(), "x");
            ui.update(0.f);
            UI_CHECK(near(ui.computed(x).opacity, 0.5f));
        }
    }
    return fuse::ui::test::finish("fuse_ui_css_cascade");
}
