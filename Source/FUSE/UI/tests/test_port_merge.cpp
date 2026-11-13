// Gates for what the style engine port, the text / icon port, the themes and the widget port have to agree on:
//   fuse_ui_merge_registry    one property registry (97 properties, every one reachable by name, written and read
//                             back), the three added value types through the cooked sheet format (Raw payloads next to
//                             FontSettings / Ident tags, tokens with their @media block), refused old versions
//   fuse_ui_merge_style_rules the text / icon properties under the style engine's rules: `icon` / `icon-size` in
//                             @media, nested rules, @layer and @container; `font-stretch` in @keyframes animations,
//                             scripted animations and transitions; `icon` (discrete) in keyframes; theme token sheets
//                             with @layer and @media (tokens of a block that does not apply are not seen)
//   fuse_ui_merge_glyph_fx    MSDF and colour-image glyph commands inside transform / filter / mask groups on the
//                             CPU rasteriser, from loose draw lists and from a styled tree
//   fuse_ui_merge_shell_core  core behaviour the FUSE-UI editor shell needs: unconsumed keys on the root's Event
//                             signal, hover changes that leave shared ancestors alone, Tab through a viewport widget
#include "fuse/ui/doc/cook.hpp"
#include "fuse/ui/icons.hpp"
#include "fuse/ui/widgets/widgets.hpp"
#include "test_util.hpp"

#include <cmath>
#include <cstring>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
bool near(f32 a, f32 b, f32 eps = 2e-3f) { return std::fabs(a - b) <= eps; }
NodeHandle make(UiContext& ui, NodeHandle parent, std::string_view cls, std::string_view type = "panel") {
    const NodeHandle n = ui.create(ui.registry().find(type), parent);
    if (!cls.empty()) ui.addClass(n, cls);
    return n;
}
constexpr u32 tag(const char (&t)[5]) { return (u32(u8(t[0])) << 24) | (u32(u8(t[1])) << 16) | (u32(u8(t[2])) << 8) | u32(u8(t[3])); }

// ---- fuse_ui_merge_registry ----------------------------------------------------------------------------------------
int registry() {
    // The registry: Prop::Count entries, each findable by its CSS name and of a known type.
    UI_CHECK_EQ(kPropCount, 97);
    int raw = 0, settings = 0, ident = 0;
    for (int i = 0; i < kPropCount; ++i) {
        const PropInfo& info = propInfo(Prop(i));
        UI_CHECK(info.id == Prop(i) && !info.cssName.empty());
        Prop found = Prop::Count;
        UI_CHECK(findProp(info.cssName, found) && found == Prop(i));
        UI_CHECK(u8(info.type) <= u8(PropType::Ident));
        raw += info.type == PropType::Raw;
        settings += info.type == PropType::FontSettings;
        ident += info.type == PropType::Ident;
        // every property reads back what was applied to a default style (applyProp / readProp cover the whole enum)
        ComputedStyle a, b;
        const PropValue v = readProp(a, Prop(i));
        applyProp(b, Prop(i), v);
        UI_CHECK(sameStyleProp(a, b, Prop(i)));
    }
    UI_CHECK(raw == 13 && settings == 2 && ident == 1);
    UI_CHECK(u8(Prop::Position) == u8(Prop::ExitDuration) + 1 && u8(Prop::FontStretch) == u8(Prop::ContainerName) + 1 &&
             u8(Prop::IconSize) + 1 == u8(Prop::Count));

    // One sheet with all three added value types, tokens inside and outside an @media block, cooked and loaded.
    const char* css =
        ":root { --tint: #336699; } "
        "@keyframes widen { from { font-stretch: 75%; icon: save; } to { font-stretch: 125%; transform: rotate(90deg); } } "
        ".a { transform: translate(4px, 50%) rotate(30deg); filter: blur(2px) hue-rotate(45deg); "
        "     clip-path: circle(20px); mask-image: linear-gradient(to right, black, transparent); "
        "     font-variation-settings: \"wght\" 650, \"wdth\" 87.5; font-feature-settings: \"liga\" off, \"ss01\", \"tnum\" 1; "
        "     font-stretch: condensed; icon: save; icon-size: 20px; color: var(--tint); animation: widen 2s linear 100s; "
        "     & > .b { icon: none; font-feature-settings: normal; } } "
        "@media (prefers-color-scheme: dark) { :root { --tint: #99ccff; } .a { icon: search; } } "
        "@media (prefers-contrast: more) { .a { icon-size: 32px; } }";
    auto parsed = StyleSheet::parse(css);
    UI_CHECK(parsed.has_value());
    auto bytes = doc::cookStyle(css);
    UI_CHECK(bytes.has_value());
    if (!parsed || !bytes) return finish("fuse_ui_merge_registry");
    UI_CHECK(doc::kCookedVersion == 5u && bytes->size() > 8 && (*bytes)[4] == 5u);
    auto back = doc::loadCookedStyle(*bytes);
    UI_CHECK(back.has_value());
    if (!back) return finish("fuse_ui_merge_registry");
    // declaration by declaration: type, Raw payload and tags survive
    UI_CHECK_EQ(back->rules().size(), parsed->rules().size());
    int sawRaw = 0, sawSettings = 0, sawIdent = 0;
    for (std::size_t r = 0; r < parsed->rules().size() && r < back->rules().size(); ++r) {
        const auto& da = parsed->rules()[r].decls;
        const auto& db = back->rules()[r].decls;
        UI_CHECK_EQ(da.size(), db.size());
        for (std::size_t k = 0; k < da.size() && k < db.size(); ++k) {
            const PropValue& x = da[k].value;
            const PropValue& y = db[k].value;
            UI_CHECK(da[k].prop == db[k].prop && da[k].raw == db[k].raw && x.type == y.type && x.trackCount == y.trackCount);
            if (x.type == PropType::Raw) {
                ++sawRaw;
                UI_CHECK(x.fCount == y.fCount && x.fPct == y.fPct && std::memcmp(x.f.data(), y.f.data(), sizeof x.f) == 0);
            } else if (x.type == PropType::FontSettings || x.type == PropType::Ident) {
                (x.type == PropType::Ident ? sawIdent : sawSettings) += 1;
                for (u8 t = 0; t < x.trackCount; ++t) UI_CHECK(x.tags[t] == y.tags[t] && x.tracks[t] == y.tracks[t]);
            }
        }
    }
    UI_CHECK(sawRaw >= 5 && sawSettings >= 3 && sawIdent >= 3);
    UI_CHECK(back->tokens() == parsed->tokens() && back->tokenConds() == parsed->tokenConds());
    UI_CHECK(back->tokenConds().size() == 2 && back->tokenConds()[0] == -1 && back->tokenConds()[1] == 0);
    UI_CHECK(back->keyframes().size() == 1 && back->keyframes()[0].frames.size() == 2);
    if (back->keyframes().size() == 1 && back->keyframes()[0].frames.size() == 2) {
        const auto& from = back->keyframes()[0].frames[0].decls;
        UI_CHECK(from.size() == 2 && from[1].prop == Prop::Icon && from[1].value.ident() == hashId("save"));
    }
    // the same tree computes the same styles from the text and from the cooked sheet, in both colour schemes
    UiContext a({.viewport = {400, 300}}), b({.viewport = {400, 300}});
    a.styles().addSheet(std::move(*parsed));
    b.styles().addSheet(std::move(*back));
    NodeHandle na, nb, ka, kb;
    for (UiContext* ui : {&a, &b}) {
        const NodeHandle n = make(*ui, ui->root(), "a"), kid = make(*ui, n, "b");
        (ui == &a ? na : nb) = n;
        (ui == &a ? ka : kb) = kid;
    }
    for (int pass = 0; pass < 3; ++pass) {
        for (UiContext* ui : {&a, &b}) {
            ui->setMediaTheme(pass >= 1, pass == 2);
            ui->update(0.f);
        }
        for (u32 i = 0; i < a.nodeCount(); ++i) UI_CHECK(a.computed(a.handleOf(i)) == b.computed(b.handleOf(i)));
        const ComputedStyle& s = b.computed(nb);
        UI_CHECK(s.icon == hashId(pass >= 1 ? "search" : "save") && near(s.iconSize, pass == 2 ? 32.f : 20.f));
        UI_CHECK(s.color == Color::rgb(pass >= 1 ? 0x99ccffu : 0x336699u)); // the token of the block that applies
        UI_CHECK(s.fontVariationCount == 2 && s.fontVariationTags[0] == tag("wght") && near(s.fontVariationValues[0], 650.f) &&
                 s.fontVariationTags[1] == tag("wdth") && near(s.fontVariationValues[1], 87.5f));
        UI_CHECK(s.fontFeatureCount == 3 && s.fontFeatureTags[0] == tag("liga") && s.fontFeatureValues[0] == 0u &&
                 s.fontFeatureTags[1] == tag("ss01") && s.fontFeatureValues[1] == 1u && s.fontFeatureTags[2] == tag("tnum"));
        UI_CHECK(near(s.fontStretch, 75.f) && s.filter.count == 2 && s.clipPath.kind == ClipShapeKind::Circle &&
                 s.mask.kind == GradientSpecKind::Linear && !s.transform.identity() && s.animationCount == 1);
        const ComputedStyle& k = b.computed(kb);
        UI_CHECK(k.icon == 0u && k.fontFeatureCount == 0 && k.fontVariationCount == 2 && near(k.fontStretch, 75.f)); // inherited
    }
    (void)na; (void)ka;
    // older formats are refused: 4 is the style engine port alone (no tags, no token conditions)
    for (u8 v : {u8(3), u8(4), u8(6)}) {
        std::vector<u8> old = *bytes;
        old[4] = v;
        UI_CHECK(!doc::loadCookedStyle(old).has_value());
    }
    // truncated / bent blobs of the new format never crash, and what loads is usable
    for (std::size_t cut = 0; cut < bytes->size(); cut += 5) {
        std::vector<u8> part(bytes->begin(), bytes->begin() + std::ptrdiff_t(cut));
        UI_CHECK(!doc::loadCookedStyle(part).has_value());
    }
    u32 seed = 777;
    for (int k = 0; k < 600; ++k) {
        std::vector<u8> bent = *bytes;
        seed = seed * 1664525u + 1013904223u;
        bent[8 + (seed >> 8) % (bent.size() - 8)] ^= u8(1u << (seed & 7));
        if (auto r = doc::loadCookedStyle(bent)) {
            UiContext ui({.viewport = {300, 200}});
            ui.styles().addSheet(std::move(*r));
            make(ui, make(ui, ui.root(), "a"), "b");
            ui.setMediaTheme(true, true);
            ui.update(0.f);
            ui.update(0.5f);
            Color c;
            (void)ui.styles().tokenColor(hashId("--tint"), c);
        }
    }
    return finish("fuse_ui_merge_registry");
}

// ---- fuse_ui_merge_style_rules -------------------------------------------------------------------------------------
int styleRules() {
    {
        UiContext ui({.viewport = {800, 600}});
        UI_CHECK(ui.addStyleSheet(
            "@layer base, app; "
            "@layer app { .t { icon: save; icon-size: 24px; } } "
            "@layer base { .t { icon: search; icon-size: 12px; font-stretch: 50%; } } " // an earlier layer loses
            ".bar { container-type: inline-size; width: 300px; height: 40px; "
            "       & .t { font-stretch: expanded; } "
            "       & > .u { icon: open; &.on { icon: close; icon-size: 18px; } } } "
            "@media (max-width: 500px) { .t { icon: menu; } .bar > .u.on { icon-size: 10px; } } "
            "@container (min-width: 400px) { .t { icon-size: 40px; } } "
            ".s:has(> .t) { font-feature-settings: \"smcp\"; }").has_value());
        const NodeHandle bar = make(ui, ui.root(), "bar"), t = make(ui, bar, "t"), u = make(ui, bar, "u");
        ui.addClass(bar, "s");
        ui.update(0.f);
        UI_CHECK(ui.computed(t).icon == hashId("save") && near(ui.computed(t).iconSize, 24.f)); // the later layer wins
        UI_CHECK(near(ui.computed(t).fontStretch, 125.f));                                         // unlayered wins over both
        UI_CHECK(ui.computed(u).icon == hashId("open") && near(ui.computed(u).iconSize, 0.f));
        UI_CHECK(ui.computed(bar).fontFeatureCount == 1 && ui.computed(t).fontFeatureCount == 1); // :has(), inherited
        ui.addClass(u, "on");
        ui.update(0.f);
        UI_CHECK(ui.computed(u).icon == hashId("close") && near(ui.computed(u).iconSize, 18.f));
        ui.setViewport({480, 600}); // @media flips: unlayered rules after the nested ones
        ui.update(0.f);
        UI_CHECK(ui.computed(t).icon == hashId("menu") && near(ui.computed(u).iconSize, 10.f));
        ui.setViewport({800, 600});
        ui.setCss(bar, "width", "450px"); // @container flips inside one update
        ui.update(0.f);
        UI_CHECK(ui.computed(t).icon == hashId("save") && near(ui.computed(t).iconSize, 40.f));
        ui.setCss(bar, "width", "300px");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(t).iconSize, 24.f));
    }
    { // font-stretch (a number) interpolates in @keyframes, scripted animations and transitions; icon switches half-way
        UiContext ui({.viewport = {400, 300}});
        UI_CHECK(ui.addStyleSheet("@keyframes widen { from { font-stretch: 50%; icon: save; icon-size: 10px; } "
                                  "                   to { font-stretch: 150%; icon: search; icon-size: 30px; } } "
                                  ".k { animation: widen 2s linear both; } "
                                  ".tr { font-stretch: 100%; icon-size: 16px; transition: font-stretch 1s linear, icon-size 1s linear; } "
                                  ".tr.wide { font-stretch: 200%; icon-size: 32px; }").has_value());
        const NodeHandle k = make(ui, ui.root(), "k"), tr = make(ui, ui.root(), "tr"), sc = make(ui, ui.root(), "");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(k).fontStretch, 50.f) && ui.computed(k).icon == hashId("save"));
        ui.update(0.5f);
        UI_CHECK(near(ui.computed(k).fontStretch, 75.f, 0.05f) && near(ui.computed(k).iconSize, 15.f, 0.05f) && ui.computed(k).icon == hashId("save"));
        ui.update(1.0f);
        UI_CHECK(near(ui.computed(k).fontStretch, 125.f, 0.05f) && ui.computed(k).icon == hashId("search")); // past the middle
        ui.update(1.0f);
        UI_CHECK(near(ui.computed(k).fontStretch, 150.f) && near(ui.computed(k).iconSize, 30.f));
        ui.addClass(tr, "wide");
        ui.update(0.f);
        ui.update(0.25f);
        UI_CHECK(near(ui.computed(tr).fontStretch, 125.f, 0.05f) && near(ui.computed(tr).iconSize, 20.f, 0.05f));
        ui.update(1.f);
        UI_CHECK(near(ui.computed(tr).fontStretch, 200.f) && near(ui.computed(tr).iconSize, 32.f));
        const Keyframe keys[] = {{0.f, Prop::FontStretch, PropValue::ofNumber(80.f)}, {1.f, Prop::FontStretch, PropValue::ofNumber(120.f)},
                                 {0.f, Prop::Icon, PropValue::ofIdent(hashId("open"))}, {1.f, Prop::Icon, PropValue::ofIdent(hashId("close"))}};
        AnimationTiming timing;
        timing.duration = 1.f;
        timing.easing = Easing::Linear;
        timing.fill = AnimFill::Forwards;
        const AnimationHandle h = ui.animate(sc, keys, timing);
        ui.update(0.f);
        ui.update(0.25f);
        UI_CHECK(near(ui.computed(sc).fontStretch, 90.f, 0.05f) && ui.computed(sc).icon == hashId("open"));
        ui.update(0.5f);
        UI_CHECK(near(ui.computed(sc).fontStretch, 110.f, 0.05f) && ui.computed(sc).icon == hashId("close"));
        ui.finishAnimation(h);
        ui.update(0.f);
        UI_CHECK(near(ui.computed(sc).fontStretch, 120.f));
    }
    { // The widget theme (a :root token table in the theme slot, replaced by handle) under the style engine's rules.
        UiContext ui({.viewport = {640, 480}, .font = &font()});
        w::install(ui);
        UI_CHECK(ui.mediaDark());
        // an application sheet: layered rules lose to the (unlayered) theme, unlayered ones win; tokens redefined per
        // colour scheme; an icon per scheme
        UI_CHECK(ui.addStyleSheet(
            "@layer app { button { background: #010203; } .mine { min-height: 7px; } } "
            ".own { background: var(--brand); color: var(--text); } "
            ":root { --brand: #aa0000; } "
            "@media (prefers-color-scheme: light) { :root { --brand: #00aa00; --accent: #123456; } .own { icon: search; } } "
            "@media (prefers-contrast: more) { :root { --brand: #ffff00; } } "
            "@media (prefers-color-scheme: dark) { .own { icon: save; } }").has_value());
        const NodeHandle b = w::button(ui, ui.root(), "Theme"), mine = make(ui, ui.root(), "mine"), own = w::button(ui, ui.root(), "Own");
        ui.addClass(own, "own");
        const NodeHandle ic = w::icon(ui, ui.root(), "none", 0.f);
        ui.addClass(ic, "own");
        ui.update(0.f);
        Color want;
        for (int pass = 0; pass < 4; ++pass) {
            const w::ThemeVariant v = pass == 1 ? w::ThemeVariant::Light : pass == 2 ? w::ThemeVariant::HighContrast : w::ThemeVariant::Dark;
            if (pass) UI_CHECK(w::setTheme(ui, v).has_value());
            for (int i = 0; i < 40; ++i) ui.update(0.05f); // past the theme's transitions
            UI_CHECK(!(ui.computed(b).background == Color::rgb(0x010203))); // the theme's button rule, not the layered one
            UI_CHECK(ui.computed(mine).minHeight == Length::px(7.f)); // nothing unlayered competes
            const Color brand = Color::rgb(pass == 1 ? 0x00aa00u : pass == 2 ? 0xffff00u : 0xaa0000u);
            UI_CHECK(ui.computed(own).background == brand);
            UI_CHECK(ui.themeColor(hashId("--brand"), {}) == brand); // what paint code reads
            UI_CHECK(w::themeTokenColor({.variant = v}, "--text", want) && ui.computed(own).color == want);
            UI_CHECK(w::themeTokenColor({.variant = v}, "--accent", want));
            if (pass == 1) want = Color::rgb(0x123456); // the application's light-only override of a theme token
            UI_CHECK(ui.themeColor(hashId("--accent"), {}) == want);
            UI_CHECK(ui.computed(own).icon == hashId(pass == 1 ? "search" : "save"));
            UI_CHECK(ui.computed(ic).icon == ui.computed(own).icon && ui.computed(ic).color == ui.computed(own).color);
            // the icon is drawn in the node's colour (currentColor): it follows the theme with no rule of its own
            Image img;
            ui.setCss(ic, "icon-size", "24px");
            ui.update(0.f);
            render(ui, img);
            const Rect r = ui.rect(ic);
            const Color tc = ui.computed(ic).color;
            int tinted = 0;
            for (int y = int(r.y); y < int(r.bottom()); ++y)
                for (int x = int(r.x); x < int(r.right()); ++x) {
                    const u32 px = img.at(x, y);
                    const int dr = int(px & 0xFF) - int(tc.r * 255.f + 0.5f), dg = int((px >> 8) & 0xFF) - int(tc.g * 255.f + 0.5f),
                              db = int((px >> 16) & 0xFF) - int(tc.b * 255.f + 0.5f);
                    tinted += std::abs(dr) <= 2 && std::abs(dg) <= 2 && std::abs(db) <= 2;
                }
            if (pass == 0) std::printf("icon node %.0fx%.0f, %d pixels in the text colour\n", double(r.w), double(r.h), tinted);
            UI_CHECK(r.h == 24.f && tinted >= 12);
        }
        UI_CHECK(ui.mediaDark() && !ui.mediaMoreContrast());
    }
    return finish("fuse_ui_merge_style_rules");
}

// ---- fuse_ui_merge_glyph_fx ----------------------------------------------------------------------------------------
struct Lit {
    int count = 0;   ///< pixels that differ from the background
    int colored = 0; ///< of those, pixels that are clearly not a shade of grey (a colour glyph or a coloured tint)
};
Lit lit(const Image& img, u32 bg, Rect area = {}) {
    Lit l;
    const int x0 = area.empty() ? 0 : std::max(0, int(area.x)), y0 = area.empty() ? 0 : std::max(0, int(area.y));
    const int x1 = area.empty() ? img.width : std::min(img.width, int(area.right()));
    const int y1 = area.empty() ? img.height : std::min(img.height, int(area.bottom()));
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const u32 px = img.at(x, y);
            if (px == bg) continue;
            ++l.count;
            const int r = int(px & 0xFF), g = int((px >> 8) & 0xFF), b = int((px >> 16) & 0xFF);
            l.colored += std::max({r, g, b}) - std::min({r, g, b}) > 60;
        }
    return l;
}
int maxDiff(const Image& a, const Image& b, int dx = 0, int dy = 0) {
    int m = 0;
    for (int y = std::max(0, dy); y < a.height && y - dy < b.height; ++y)
        for (int x = std::max(0, dx); x < a.width && x - dx < b.width; ++x) {
            const u32 p = a.at(x, y), q = b.at(x - dx, y - dy);
            for (int k = 0; k < 32; k += 8) m = std::max(m, std::abs(int((p >> k) & 0xFF) - int((q >> k) & 0xFF)));
        }
    return m;
}

int glyphFx() {
    using namespace fuse::text;
    enableColorGlyphs();
    enableMsdf();
    auto colr = Font::fromFile(testFontPath("colrv1_test"));
    if (!colr || !msdfEnabled()) {
        std::printf("fuse_ui_merge_glyph_fx: no colour test font / MSDF in this build, skipped\n");
        return 77;
    }
    Font base = std::move(*Font::bundled());
    std::vector<const Font*> chain{&*colr};
    base.setFallbacks(chain);
    std::string text = "Mg&8";
    appendUtf8(text, U''); // colour glyphs of the test font
    appendUtf8(text, U'');

    // A plain scene first: one 56 px label = MSDF glyphs (threshold 40) and colour-image glyphs, no group.
    constexpr u32 kBg = 0xFF202020u;
    auto scene = [&](UiContext& ui, std::string_view extraCss) {
        ui.setMsdfThreshold(40.f);
        UI_CHECK(ui.addStyleSheet(std::string(".big { font-size: 56px; color: #ffffff; left: 40px; top: 60px; } .stage { layout: absolute; width: 480px; height: 260px; } ") +
                                  std::string(extraCss)).has_value());
        const NodeHandle stage = make(ui, ui.root(), "stage");
        const NodeHandle label = make(ui, stage, "big", "label");
        ui.setText(label, text);
        ui.update(0.f);
        return label;
    };
    auto count = [](const DrawList& dl, u8 mode, bool grouped) {
        int n = 0, depth = 0;
        for (const DrawCmd& c : dl.cmds()) {
            if (c.op == DrawOp::PushTransform || c.op == DrawOp::PushFilter || c.op == DrawOp::PushMask) ++depth;
            else if (c.op == DrawOp::PushClip || c.op == DrawOp::PushClipPath || c.op == DrawOp::PushLayer) depth += depth > 0;
            else if (c.op == DrawOp::PopClip) depth -= depth > 0;
            else if (c.op == DrawOp::Glyph && c.gradient == mode && (depth > 0) == grouped) ++n;
        }
        return n;
    };
    auto draw = [&](UiContext& ui, const DrawList& dl, Image& img) {
        img.resize(480, 260, kBg);
        CpuRasterizer r;
        r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        r.render(dl, img);
    };
    UiContext plain({.viewport = {480, 260}, .font = &base});
    const NodeHandle plainLabel = scene(plain, "");
    const int msdf = count(plain.drawList(), kGlyphMsdf, false), color = count(plain.drawList(), kGlyphColor, false);
    std::printf("plain: %d MSDF, %d colour glyph commands\n", msdf, color);
    UI_CHECK(msdf == 4 && color == 2);
    Image ref;
    draw(plain, plain.drawList(), ref);
    const Lit refLit = lit(ref, kBg);
    UI_CHECK(refLit.count > 2000 && refLit.colored > 150);
    Rect box; // the ink: what the glyph quads cover
    for (const DrawCmd& c : plain.drawList().cmds())
        if (c.op == DrawOp::Glyph) box = box.empty() ? c.rect : box.unite(c.rect);
    UI_CHECK(!box.empty() && plain.alive(plainLabel));

    // The same glyph commands wrapped in each group kind by hand.
    auto wrapped = [&](auto&& open) {
        DrawList dl;
        open(dl);
        for (const DrawCmd& c : plain.drawList().cmds())
            if (c.op == DrawOp::Glyph) dl.push(c);
        dl.popClip();
        return dl;
    };
    const Rect src{box.x - 8.f, box.y - 8.f, box.w + 16.f, box.h + 16.f};
    Image out;
    { // translation by whole pixels: drawn in place, the same texels moved
        const DrawList dl = wrapped([&](DrawList& d) { d.pushTransform(Affine::translation(13.f, 7.f), src); });
        draw(plain, dl, out);
        Image glyphsOnly;
        draw(plain, wrapped([&](DrawList& d) { d.pushTransform(Affine::translation(0.f, 0.f), src); }), glyphsOnly);
        UI_CHECK_EQ(maxDiff(out, glyphsOnly, 13, 7), 0);
        UI_CHECK(lit(out, kBg).colored > 150);
    }
    { // a scaling transform: both kinds are there, about four times the pixels (resampled from the 1:1 layer)
        const Affine xf = Affine::translation(src.x, src.y) * Affine::scaling(2.f, 2.f) * Affine::translation(-src.x, -src.y);
        draw(plain, wrapped([&](DrawList& d) { d.pushTransform(xf, src); }), out);
        const Rect half{src.x, src.y, src.w, src.h}; // the top-left quarter of the doubled label, clipped by the image
        Image one;
        draw(plain, wrapped([&](DrawList& d) { d.pushTransform(Affine::translation(0.f, 0.f), src); }), one);
        const Lit a = lit(one, kBg, {src.x, src.y, 100.f, 60.f}), b = lit(out, kBg, {src.x, src.y, 200.f, 120.f});
        (void)half;
        std::printf("scale 2: %d lit of %d at 1:1 (x%.2f), %d coloured\n", b.count, a.count, a.count ? double(b.count) / a.count : 0.0, lit(out, kBg).colored);
        UI_CHECK(a.count > 300 && b.count > a.count * 7 / 2 && b.count < a.count * 6);
        UI_CHECK(lit(out, kBg).colored > refLit.colored * 2);
    }
    { // a rotation keeps the ink (area preserved) and the colours
        const Vec2 c{src.x + src.w * 0.5f, src.y + src.h * 0.5f};
        const Affine xf = Affine::translation(c.x, c.y) * Affine::rotation(0.35f) * Affine::translation(-c.x, -c.y);
        draw(plain, wrapped([&](DrawList& d) { d.pushTransform(xf, src); }), out);
        const Lit l = lit(out, kBg);
        UI_CHECK(l.count > refLit.count * 8 / 10 && l.count < refLit.count * 14 / 10 && l.colored > refLit.colored / 2);
        UI_CHECK(maxDiff(out, ref) > 100); // and it moved
    }
    { // filters: an identity colour filter changes nothing; grayscale removes the colour, not the glyphs; blur spreads
        const FilterOp same{FilterKind::Brightness, 1.f, {}, {}}, gray{FilterKind::Grayscale, 1.f, {}, {}}, blur{FilterKind::Blur, 3.f, {}, {}};
        draw(plain, wrapped([&](DrawList& d) { d.pushFilter({&same, 1}, src); }), out);
        UI_CHECK(maxDiff(out, ref) <= 2);
        draw(plain, wrapped([&](DrawList& d) { d.pushFilter({&gray, 1}, src); }), out);
        const Lit g = lit(out, kBg);
        UI_CHECK(g.count > refLit.count * 9 / 10 && g.colored == 0);
        draw(plain, wrapped([&](DrawList& d) { d.pushFilter({&blur, 1}, src); }), out);
        UI_CHECK(lit(out, kBg).count > refLit.count * 12 / 10 && lit(out, kBg).colored > 0);
    }
    { // masks: an opaque mask changes nothing; a left-to-right fade keeps the left glyphs and drops the right ones
        const GradientStop solid[2]{{0.f, Color::rgb(0x000000)}, {1.f, Color::rgb(0x000000)}};
        const GradientStop fade[2]{{0.f, Color::rgb(0x000000)}, {1.f, Color{0.f, 0.f, 0.f, 0.f}}};
        draw(plain, wrapped([&](DrawList& d) { d.pushMask(Gradient::linear({src.x, 0}, {src.right(), 0}, solid), src); }), out);
        UI_CHECK(maxDiff(out, ref) <= 2);
        draw(plain, wrapped([&](DrawList& d) { d.pushMask(Gradient::linear({src.x, 0}, {src.right(), 0}, fade), src); }), out);
        const Rect left{src.x, src.y, src.w * 0.3f, src.h}, right{src.x + src.w * 0.85f, src.y, src.w * 0.15f, src.h};
        UI_CHECK(lit(out, kBg, left).count > lit(ref, kBg, left).count * 8 / 10);
        const Lit rr = lit(ref, kBg, right);
        UI_CHECK(rr.colored > 20); // the colour glyphs are at the right end ...
        int strong = 0, strongRef = 0;
        for (int y = int(right.y); y < int(right.bottom()); ++y)
            for (int x = int(right.x); x < int(right.right()) && x < out.width; ++x) {
                auto peak = [](u32 px) { return std::max({px & 0xFF, (px >> 8) & 0xFF, (px >> 16) & 0xFF}); };
                strong += peak(out.at(x, y)) > 150u;
                strongRef += peak(ref.at(x, y)) > 150u;
            }
        std::printf("mask fade, right end: %d strong pixels of %d\n", strong, strongRef);
        UI_CHECK(strongRef > 20 && strong < strongRef / 4); // ... and faded there
    }

    // From a styled tree: CSS transform / filter / mask-image put the label's MSDF and colour glyphs into groups.
    struct Case { const char* css; int minLit; bool colored; };
    const Case cases[] = {{".big { transform: rotate(8deg) scale(1.2); }", 2400, true},
                          {".big { filter: grayscale(1) blur(1px); }", 2000, false},
                          {".big { mask-image: linear-gradient(to right, black, black); filter: hue-rotate(90deg); }", 2000, true},
                          {".big { transform: translate(12px, 9px); }", 2000, true}};
    for (const Case& c : cases) {
        UiContext ui({.viewport = {480, 260}, .font = &base});
        scene(ui, c.css);
        UI_CHECK_EQ(count(ui.drawList(), kGlyphMsdf, true), 4);
        UI_CHECK_EQ(count(ui.drawList(), kGlyphColor, true), 2);
        UI_CHECK_EQ(count(ui.drawList(), kGlyphMsdf, false) + count(ui.drawList(), kGlyphColor, false), 0);
        draw(ui, ui.drawList(), out);
        const Lit l = lit(out, kBg);
        std::printf("%-40.40s  lit %d, coloured %d\n", c.css, l.count, l.colored);
        UI_CHECK(l.count >= c.minLit && (l.colored > 100) == c.colored);
        if (std::strstr(c.css, "translate(12px")) UI_CHECK_EQ(maxDiff(out, ref, 12, 9), 0); // exactly the plain label, moved
    }
    return finish("fuse_ui_merge_glyph_fx");
}

// ---- fuse_ui_merge_shell_core ----------------------------------------------------------------------------------------
// Three core behaviours the FUSE-UI editor shell relies on (ported from the magical-bassi core deltas):
// keys nobody took reach the root's Event signal, a hover change leaves shared ancestors alone, and a viewport
// widget forwards Tab without keeping the focus.
int shellCore() {
    UiContext ui({.viewport = {800, 600}, .font = &font()});
    w::install(ui);
    const NodeHandle bar = w::panel(ui, ui.root());
    ui.addClass(bar, "row");
    const NodeHandle a = w::button(ui, bar, "One"), b = w::button(ui, bar, "Two");
    const NodeHandle vp = w::viewportWidget(ui, ui.root(), 0, 64, 64);
    const NodeHandle after = w::button(ui, ui.root(), "After");
    ui.update(0.f);
    // unconsumed keys: {value = key, event} on the root, with and without a focused node; consumed keys do not arrive
    int rootKeys = 0;
    Key lastKey = Key::None;
    ui.connect(ui.root(), SignalKind::Event, [&](UiContext&, NodeHandle, const SignalArgs& args) {
        if (!args.event || args.event->type != EventType::KeyDown) return;
        ++rootKeys;
        lastKey = Key(int(args.value));
        UI_CHECK(args.event->key == lastKey);
    });
    ui.keyDown(Key::Escape); // nothing has the focus
    UI_CHECK(rootKeys == 1 && lastKey == Key::Escape);
    ui.setFocus(a, FocusReason::Keyboard);
    ui.keyDown(Key::F5); // a button does not take F5
    UI_CHECK(rootKeys == 2 && lastKey == Key::F5);
    ui.keyDown(Key::Tab); // focus navigation takes it
    UI_CHECK(rootKeys == 2 && ui.focused() == b);
    // the viewport forwards keys to its Event signal; Tab still moves on
    int vpKeys = 0;
    ui.connect(vp, SignalKind::Event, [&](UiContext& c, NodeHandle n, const SignalArgs& args) {
        if (EventType(u32(args.value)) == EventType::KeyDown && !args.event) vpKeys += w::viewportLastInput(c, n).key != Key::None;
    });
    ui.setFocus(vp, FocusReason::Keyboard);
    ui.keyDown(Key::W);
    UI_CHECK(vpKeys == 1 && ui.focused() == vp && rootKeys == 2); // consumed by the viewport
    ui.keyDown(Key::Tab);
    UI_CHECK(vpKeys == 2 && ui.focused() == after); // forwarded, and the focus moved on
    ui.keyDown(Key::Tab, kModShift);
    UI_CHECK(ui.focused() == vp);
    // hover from one button to its neighbour: only the two buttons are damaged, the shared ancestors keep :hover
    const Rect ra = ui.rect(a), rb = ui.rect(b);
    ui.pointerMove({ra.x + ra.w * 0.5f, ra.y + ra.h * 0.5f});
    for (int i = 0; i < 30; ++i) ui.update(0.05f);
    UI_CHECK((ui.states(a) & kStateHover) && (ui.states(bar) & kStateHover) && (ui.states(ui.root()) & kStateHover));
    ui.pointerMove({rb.x + rb.w * 0.5f, rb.y + rb.h * 0.5f});
    ui.update(0.f);
    const Rect d = ui.damage(), both = ra.unite(rb);
    std::printf("hover a -> b: damage %.0fx%.0f, the two buttons %.0fx%.0f\n", double(d.w), double(d.h), double(both.w), double(both.h));
    UI_CHECK(!d.empty() && d.w <= both.w + 2.f && d.h <= both.h + 2.f);
    UI_CHECK(!(ui.states(a) & kStateHover) && (ui.states(b) & kStateHover) && (ui.states(bar) & kStateHover) &&
             (ui.states(ui.root()) & kStateHover));
    const Rect rv = ui.rect(vp); // to another branch of the tree: the bar loses :hover, the root keeps it
    ui.pointerMove({rv.x + rv.w * 0.5f, rv.y + rv.h * 0.5f});
    ui.update(0.f);
    UI_CHECK(!(ui.states(b) & kStateHover) && !(ui.states(bar) & kStateHover) && (ui.states(vp) & kStateHover) &&
             (ui.states(ui.root()) & kStateHover));
    return finish("fuse_ui_merge_shell_core");
}
} // namespace

int main(int argc, char** argv) {
    std::string gate;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--gate=", 7) == 0) gate = argv[i] + 7;
    if (gate == "registry") return registry();
    if (gate == "style_rules") return styleRules();
    if (gate == "glyph_fx") return glyphFx();
    if (gate == "shell_core") return shellCore();
    std::fprintf(stderr, "usage: fuse_ui_port_merge --gate=registry|style_rules|glyph_fx|shell_core\n");
    return 2;
}
