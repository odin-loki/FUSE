// Gate fuse_ui_css_cascade: CSS Nesting (& and implicit descendant, at-rules inside rules), Cascade Layers (@layer
// statement / block / anonymous / nested; unlayered rules win; layers merge across sheets), @media (viewport ranges,
// orientation, prefers-*; re-evaluated when the viewport or the preferences change, nothing restyled otherwise),
// @supports, @container (container-type / container-name / the container shorthand; the subtree is restyled when
// the container's size changes), @scope (root, limit, :scope) and the cooked round trip of all of it.
// (Adapted from the gui-framework gate of the same name.)
#include "fuse/ui/doc/cook.hpp"
#include "test_util.hpp"

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
    for (const auto& x : d) std::fprintf(stderr, "  diagnostic line %u: %s\n", x.line, x.message.c_str());
    UI_CHECK(d.empty());
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
        // own declarations after nested rules still belong to the parent rule, which comes first
        auto s = StyleSheet::parse(".p { .q { opacity: 0.1; } opacity: 0.2; }");
        UI_CHECK(s && s->rules().size() == 2 && s->rules()[0].chain.size() == 1 && s->rules()[1].chain.size() == 2);
        // a parent selector list nests through :is()
        ui.styles().addSheet(sheet(".m, .n { > .k { gap: 9px; } }"));
        const NodeHandle nn = make(ui, ui.root(), "n"), k = make(ui, nn, "k");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(k).gap, 9.f));
        // a conditional group inside a rule holds declarations of that rule
        ui.styles().addSheet(sheet(".r { gap: 1px; @media (min-width: 300px) { gap: 2px; .s { gap: 3px; } } }"));
        const NodeHandle r = make(ui, ui.root(), "r"), sN = make(ui, r, "s");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(r).gap, 2.f) && near(ui.computed(sN).gap, 3.f));
        ui.setViewport({200, 300});
        ui.update(0.f);
        UI_CHECK(near(ui.computed(r).gap, 1.f) && near(ui.computed(sN).gap, 0.f));
        std::vector<StyleDiagnostic> diags;
        UI_CHECK(!StyleSheet::parse(".p { .q { opacity: 0.1; }", &diags)); // unbalanced
        UI_CHECK(!StyleSheet::parse("width: 1px;"));                        // a declaration at the top level
    }
    // ---- @layer ----
    {
        UiContext ui({.viewport = {400, 300}});
        // order: base, theme (theme.dark before theme's own rules), the anonymous layer, then unlayered rules.
        // Specificity only decides inside one layer.
        ui.styles().addSheet(sheet("@layer base, theme; #n { opacity: 0.9; } "
                                   "@layer theme { .k.k.k { opacity: 0.3; } @layer dark { .k.k.k.k { opacity: 0.2; } } } "
                                   "@layer base { #n.k { opacity: 0.1; } .q { opacity: 0.15; } } "
                                   "@layer { .z { opacity: 0.6; } } @layer theme.dark { .z { opacity: 0.4; } }"));
        const NodeHandle n = ui.create(ui.registry().find("panel"), ui.root(), "n");
        ui.addClass(n, "k");
        const NodeHandle m = make(ui, ui.root(), "k");
        const NodeHandle z = make(ui, ui.root(), "z");
        const NodeHandle q = make(ui, ui.root(), "q");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(n).opacity, 0.9f)); // unlayered wins over any layered specificity
        UI_CHECK(near(ui.computed(m).opacity, 0.3f)); // theme's own rule comes after theme.dark
        UI_CHECK(near(ui.computed(z).opacity, 0.6f)); // the anonymous layer was declared after theme
        UI_CHECK(near(ui.computed(q).opacity, 0.15f));
        UI_CHECK_EQ(ui.styles().sheet(0).layers().size(), std::size_t(4)); // base, theme, theme.dark, the anonymous one
        // a later sheet's layers merge by name: base keeps its rank (lowest), also against a higher specificity there
        ui.styles().addSheet(sheet("@layer base { #root .z { opacity: 0.05; } } @layer late { .q { opacity: 0.7; } }"));
        ui.update(0.f);
        UI_CHECK(near(ui.computed(z).opacity, 0.6f));
        UI_CHECK(near(ui.computed(q).opacity, 0.7f)); // a new layer ranks after the existing ones
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
                                   "@media (bogus-feature: 1) { .m { opacity: 0; } } "
                                   "@media (hover: none) or (pointer: coarse) { .t { opacity: 0.5; } } "
                                   "@media (min-resolution: 2dppx) { .hd { opacity: 0.5; } }"));
        const NodeHandle m = make(ui, ui.root(), "m"), r = make(ui, ui.root(), "r"), t = make(ui, ui.root(), "t"), hd = make(ui, ui.root(), "hd");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(m).opacity, 1.f) && near(ui.computed(r).opacity, 0.5f) && near(ui.computed(t).opacity, 1.f));
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
        ui.setMediaPreferences(true);
        ui.update(0.f);
        UI_CHECK(near(ui.computed(m).opacity, 0.125f));
        ui.setReducedMotion(true);
        ui.update(0.f);
        UI_CHECK(near(ui.computed(r).opacity, 1.f));
        ui.setMediaPreferences(true, false, 1); // a touch screen
        ui.update(0.f);
        UI_CHECK(near(ui.computed(t).opacity, 0.5f));
        UI_CHECK(near(ui.computed(hd).opacity, 1.f));
        ui.setRenderScale(2.f);
        ui.update(0.f);
        UI_CHECK(near(ui.computed(hd).opacity, 0.5f));
        const u32 v = ui.styles().sheetVersion();
        ui.setViewport({460, 300}); // no query changed its answer: nothing is restyled
        ui.update(0.f);
        UI_CHECK_EQ(ui.styles().sheetVersion(), v);
        UI_CHECK_EQ(ui.stats().styled, 0u);
        ui.setViewport({650, 300});
        ui.update(0.f);
        UI_CHECK(ui.styles().sheetVersion() != v && ui.stats().styled > 0u);
    }
    // ---- @supports ----
    {
        auto s = StyleSheet::parse("@supports (opacity: 0.5) { .a { opacity: 0.5; } } @supports (opacity: banana) { .b {} } "
                                   "@supports not (fake-prop: 1) { .c {} } @supports (display: flex) and (not (x: y)) { .d {} } "
                                   "@supports (x: y) or selector(a > b) { .e {} } @supports selector(:bogus) { .f {} } "
                                   "@supports (margin: 1px 2px) { .g {} } @supports (transform: rotate(10deg)) and (filter: blur(2px)) { .h {} } "
                                   "@supports (transform: rotate3d(1, 0, 0, 10deg)) { .i {} }");
        UI_CHECK(s.has_value());
        std::string got;
        if (s)
            for (const auto& r : s->rules())
                for (const char* n : {"a", "b", "c", "d", "e", "f", "g", "h", "i"})
                    if (r.chain[0].classes[0] == hashId(n)) got += n;
        UI_CHECK_EQ(got, std::string("acdegh"));
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
        const NodeHandle deep = make(ui, t, "t");
        const NodeHandle free = make(ui, ui.root(), "t");
        ui.update(0.f);
        UI_CHECK(ui.styles().usesContainerQueries());
        UI_CHECK_EQ(ui.computed(w).containerType, u8(1));
        UI_CHECK(ui.computed(w).containerName == animationNameId("card"));
        UI_CHECK(near(ui.computed(t).opacity, 0.5f) && near(ui.computed(deep).opacity, 0.5f)); // settled in the same update
        UI_CHECK(near(ui.computed(free).opacity, 1.f)); // no container: the query is false
        UI_CHECK(near(ui.computed(u).opacity, 1.f));    // inline-size cannot answer a height query
        UI_CHECK(ui.setCss(w, "width", "150px"));
        ui.update(0.f);
        UI_CHECK(near(ui.computed(t).opacity, 0.25f) && near(ui.computed(deep).opacity, 0.25f));
        UI_CHECK(ui.setCss(w, "container-type", "size"));
        ui.update(0.f);
        UI_CHECK(near(ui.computed(u).opacity, 0.75f));
        UI_CHECK(ui.setCss(w, "container-name", "none"));
        ui.update(0.f);
        UI_CHECK(near(ui.computed(t).opacity, 1.f)); // `card` is gone; 150 < 300
        // the container follows the viewport: a resize restyles its subtree in the same update
        UiContext fl({.viewport = {800, 300}});
        fl.styles().addSheet(sheet(".w { container-type: inline-size; } .t { gap: 1px; } @container (max-width: 500px) { .t { gap: 2px; } }"));
        const NodeHandle fw = make(fl, fl.root(), "w"), ft = make(fl, fw, "t");
        fl.update(0.f);
        UI_CHECK(near(fl.computed(ft).gap, 1.f));
        fl.setViewport({480, 300});
        fl.update(0.f);
        UI_CHECK(near(fl.computed(ft).gap, 2.f));
        fl.setViewport({900, 300});
        fl.update(0.f);
        UI_CHECK(near(fl.computed(ft).gap, 1.f));
        fl.update(0.f);
        UI_CHECK(!fl.needsUpdate());
    }
    // ---- @scope ----
    {
        UiContext ui({.viewport = {400, 300}});
        ui.styles().addSheet(sheet("@scope (.card) to (.content) { .img { opacity: 0.5; } :scope { gap: 4px; } :scope > .img { min-width: 7px; } } "
                                   "@scope (.x, .y) { .img { min-height: 9px; } }"));
        const NodeHandle card = make(ui, ui.root(), "card");
        const NodeHandle img = make(ui, card, "img");
        const NodeHandle content = make(ui, card, "content");
        const NodeHandle inner = make(ui, content, "img"); // behind the limit
        const NodeHandle wrap = make(ui, card, ""), nested = make(ui, wrap, "img");
        const NodeHandle out = make(ui, ui.root(), "img");
        const NodeHandle y = make(ui, ui.root(), "y"), yImg = make(ui, y, "img");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(img).opacity, 0.5f) && near(ui.computed(nested).opacity, 0.5f));
        UI_CHECK(near(ui.computed(inner).opacity, 1.f) && near(ui.computed(out).opacity, 1.f));
        UI_CHECK(near(ui.computed(card).gap, 4.f) && near(ui.computed(content).gap, 0.f));
        UI_CHECK(near(ui.computed(img).minWidth.value, 7.f) && !near(ui.computed(nested).minWidth.value, 7.f));
        UI_CHECK(near(ui.computed(yImg).minHeight.value, 9.f) && !near(ui.computed(img).minHeight.value, 9.f));
        // the scope root is found again when the class moves
        ui.removeClass(card, "card");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(img).opacity, 1.f) && near(ui.computed(nested).opacity, 1.f));
        ui.addClass(wrap, "card");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(nested).opacity, 0.5f) && near(ui.computed(img).opacity, 1.f));
    }
    // ---- cooked round trip ----
    {
        const char* css = "@layer a, b; @layer b { .x { opacity: 0.5; } } @layer a { .x { opacity: 0.25; } } "
                          "@media (min-width: 500px) { @container c (height > 10px) { .y { opacity: 0.5; } } } "
                          ".p:has(> [data-k^=v i]):nth-child(2n+1) ~ .q:not(.r, .s) { gap: 3px; } "
                          "@scope (.card) to (.content) { .img { opacity: 0.5; } } "
                          "@keyframes spin { from { transform: rotate(0deg); } 50% { opacity: 0.5; animation-timing-function: ease-in; } to { transform: rotate(360deg); } } "
                          ".z { transform: translate(10px, 50%) rotate(45deg); filter: blur(2px) drop-shadow(1px 2px 3px red); "
                          "clip-path: polygon(0 0, 100% 0, 50% 100%); mask-image: linear-gradient(to right, black, transparent); "
                          "animation: spin 2s linear infinite alternate; position: sticky; z-index: 3; aspect-ratio: 16 / 9; }";
        auto bytes = doc::cookStyle(css);
        UI_CHECK(bytes.has_value());
        auto back = bytes ? doc::loadCookedStyle(*bytes) : std::expected<StyleSheet, std::string>(std::unexpected("no bytes"));
        UI_CHECK(back.has_value());
        if (back) {
            UI_CHECK_EQ(back->layers().size(), std::size_t(2));
            UI_CHECK_EQ(back->conditions().size(), std::size_t(2));
            UI_CHECK(back->conditions()[1].parent == 0 && back->conditions()[1].kind == StyleCondition::Kind::Container);
            UI_CHECK(back->rules()[0].layer == 2 && back->rules()[2].cond == 1);
            UI_CHECK(back->conditions()[1].containerName == animationNameId("c") && back->conditions()[1].needsHeight);
            UI_CHECK(back->keyframes().size() == 1 && back->keyframes()[0].frames.size() == 3 && back->keyframes()[0].frames[1].hasEase);
            UI_CHECK(back->rules()[3].parts.size() >= 4 && !back->rules()[3].lists.empty() && back->rules()[4].scoped);
            UiContext a({.viewport = {600, 300}}), b({.viewport = {600, 300}});
            a.styles().addSheet(sheet(css));
            b.styles().addSheet(std::move(*back));
            // the same tree styles the same from the text and from the cooked sheet
            for (UiContext* ui : {&a, &b}) {
                const NodeHandle p = make(*ui, ui->root(), "p"), x = make(*ui, ui->root(), "x");
                const NodeHandle q = make(*ui, ui->root(), "q"), z = make(*ui, ui->root(), "z");
                const NodeHandle kid = make(*ui, p, "");
                ui->setAttribute(kid, "data-k", "Value");
                const NodeHandle card = make(*ui, ui->root(), "card"), img = make(*ui, card, "img");
                ui->update(0.f);
                (void)x; (void)q; (void)z; (void)img;
            }
            for (u32 i = 0; i < a.nodeCount(); ++i) UI_CHECK(a.computed(a.handleOf(i)) == b.computed(b.handleOf(i)));
            const NodeHandle x = b.handleOf(2), q = b.handleOf(3), z = b.handleOf(4), img = b.handleOf(7);
            UI_CHECK(near(b.computed(x).opacity, 0.5f));
            UI_CHECK(near(b.computed(q).gap, 3.f));
            UI_CHECK(b.computed(z).position == PositionKind::Sticky && b.computed(z).zIndex == 3 && near(b.computed(z).aspectRatio, 16.f / 9.f));
            UI_CHECK(b.computed(z).filter.count == 2 && b.computed(z).clipPath.kind == ClipShapeKind::Polygon &&
                     b.computed(z).mask.kind == GradientSpecKind::Linear && b.computed(z).animationCount == 1);
            UI_CHECK(near(b.computed(img).opacity, 0.5f));
        }
        // corrupt cooked sheets are refused, never crash
        if (bytes) {
            for (std::size_t cut = 0; cut < bytes->size(); cut += 7) {
                std::vector<u8> part(bytes->begin(), bytes->begin() + std::ptrdiff_t(cut));
                UI_CHECK(!doc::loadCookedStyle(part).has_value());
            }
            u32 seed = 12345;
            for (int k = 0; k < 400; ++k) {
                std::vector<u8> bent = *bytes;
                seed = seed * 1664525u + 1013904223u;
                bent[8 + (seed >> 8) % (bent.size() - 8)] ^= u8(1u << (seed & 7));
                auto r = doc::loadCookedStyle(bent);
                if (r) { // whatever loads has to be usable
                    UiContext ui({.viewport = {300, 200}});
                    ui.styles().addSheet(std::move(*r));
                    make(ui, make(ui, ui.root(), "p"), "q");
                    ui.update(0.f);
                }
            }
        }
    }
    return fuse::ui::test::finish("fuse_ui_css_cascade");
}
