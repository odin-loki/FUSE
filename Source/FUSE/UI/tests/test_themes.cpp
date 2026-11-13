// fuse_ui_theme_*: the widget theme variants (dark, light, high contrast). One gate per `--gate=<name>`:
//   sheet_handles  StyleEngine / UiContext sheets by handle: replace keeps the cascade position, remove drops the
//                  rules, the theme slot never stacks sheets, theme token colours follow the newest sheet
//   token_only     every variant defines every token as a colour; outside `:root` the theme text has no literal
//                  colour; paint code picks up a token override (a slider track painted in the overridden colour)
//   contrast       WCAG 2 contrast of text against its background, per widget: >= 4.5 in dark and light, >= 7 in
//                  high contrast. Measured two ways: (1) from the computed styles of a widget gallery (text colour
//                  against the composited backgrounds of the node and its ancestors; disabled nodes are exempt, as
//                  WCAG exempts inactive controls), (2) for text the paint code draws in token colours, from the
//                  token pairs it uses. The dark values that miss 4.5 are listed in kDarkKnown with a pinned floor
//                  (the dark theme is frozen pixel for pixel by the existing goldens); nothing else may miss.
//   paint_literals no hard-coded colour in the widget paint code: every `Color::rgb(0x...)` in Widgets/src/*.cpp is the
//                  fallback of a theme token (themed / themedA, widgets_internal.hpp) or sits on a line marked
//                  `theme-exempt` (data colours: the colour picker's gradients and markers). Scans the sources.
//   focus_ring     every focusable class of the library shows keyboard focus: rendering a node of the class with
//                  and without :focus-visible differs inside its box, in each of the three variants
#include "test_util.hpp"

#include "fuse/ui/color_space.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
const w::ThemeVariant kVariants[] = {w::ThemeVariant::Dark, w::ThemeVariant::Light, w::ThemeVariant::HighContrast};
const char* variantName(w::ThemeVariant v) {
    return v == w::ThemeVariant::Light ? "light" : v == w::ThemeVariant::HighContrast ? "contrast" : "dark";
}
Color over(Color top, Color under) { // source-over, straight alpha, opaque `under`
    return {top.r * top.a + under.r * (1.f - top.a), top.g * top.a + under.g * (1.f - top.a),
            top.b * top.a + under.b * (1.f - top.a), 1.f};
}
u32 px(const Image& img, int x, int y) { return img.pixels[std::size_t(y) * std::size_t(img.width) + std::size_t(x)]; }

// ---- sheet_handles ---------------------------------------------------------------------------------------------------
void gateSheetHandles() {
    UiContext ui({.viewport = {200, 100}, .font = &font()});
    const NodeHandle a = w::label(ui, ui.root(), "a", "a");
    auto parse = [](std::string_view css) {
        auto s = StyleSheet::parse(css);
        UI_CHECK(s.has_value());
        return std::move(*s);
    };
    const u32 base = ui.styles().sheetCount();
    const u32 h1 = ui.addStyleSheetTracked(parse(":root { --c: #ff0000; } label { color: var(--c); font-size: 10px; }"));
    ui.addStyleSheet(parse("label { font-size: 20px; }")); // later sheet: wins for font-size
    UI_CHECK(h1 != 0 && ui.styles().hasSheet(h1));
    UI_CHECK_EQ(ui.styles().sheetCount(), base + 2);
    ui.update();
    UI_CHECK(ui.computed(a).color == Color::rgb(0xFF0000));
    UI_CHECK_EQ(ui.computed(a).fontSize, 20.f);
    UI_CHECK(ui.themeColor(hashId("--c"), {}) == Color::rgb(0xFF0000));
    UI_CHECK(ui.themeColor(hashId("--missing"), Color::rgb(0x123456)) == Color::rgb(0x123456));

    // replace: same handle, same position (the later sheet still wins font-size), new values, no growth
    UI_CHECK(ui.replaceStyleSheet(h1, parse(":root { --c: #00ff00; --d: var(--c); } label { color: var(--c); font-size: 12px; }")));
    UI_CHECK_EQ(ui.styles().sheetCount(), base + 2);
    ui.update();
    UI_CHECK(ui.computed(a).color == Color::rgb(0x00FF00));
    UI_CHECK_EQ(ui.computed(a).fontSize, 20.f);
    UI_CHECK(ui.themeColor(hashId("--c"), {}) == Color::rgb(0x00FF00)); // the token cache followed the replace
    UI_CHECK(ui.themeColor(hashId("--d"), {}) == Color::rgb(0x00FF00)); // var() chain
    for (int i = 0; i < 20; ++i) UI_CHECK(ui.replaceStyleSheet(h1, parse("label { color: #0000ff; }")));
    UI_CHECK_EQ(ui.styles().sheetCount(), base + 2);
    ui.update();
    UI_CHECK(ui.computed(a).color == Color::rgb(0x0000FF));

    // remove: the rules are gone, the other sheet stays; the handle is dead
    UI_CHECK(ui.removeStyleSheet(h1));
    UI_CHECK(!ui.removeStyleSheet(h1) && !ui.replaceStyleSheet(h1, parse("label { color: #ffffff; }")));
    UI_CHECK(!ui.styles().hasSheet(h1) && !ui.replaceStyleSheet(0, parse("label { }")));
    UI_CHECK_EQ(ui.styles().sheetCount(), base + 1);
    ui.update();
    UI_CHECK(!(ui.computed(a).color == Color::rgb(0x0000FF)));
    UI_CHECK_EQ(ui.computed(a).fontSize, 20.f);
    const u32 h2 = ui.addStyleSheetTracked(parse("label { color: #112233; }"));
    UI_CHECK(h2 != 0 && h2 != h1);
    ui.clearStyleSheets();
    UI_CHECK(!ui.styles().hasSheet(h2) && ui.styles().sheetCount() == 0);

    // the theme slot: install puts the dark theme there; switching replaces it in place, under the app's sheet
    UiContext t({.viewport = {200, 100}, .font = &font()});
    w::install(t);
    const u32 slot = t.themeStyleSheet();
    UI_CHECK(slot != 0);
    UI_CHECK(t.addStyleSheet("button { color: #010203; }").has_value()); // an app sheet after the theme
    const u32 count = t.styles().sheetCount();
    const NodeHandle b = w::button(t, t.root(), "B");
    t.update();
    const Color darkBg = t.computed(t.root()).background;
    for (int i = 0; i < 4; ++i)
        for (w::ThemeVariant v : kVariants) {
            UI_CHECK(w::setTheme(t, v).has_value());
            t.update();
            Color bg;
            UI_CHECK(w::themeTokenColor({.variant = v}, "--bg", bg));
            UI_CHECK(t.computed(t.root()).background == bg);
            UI_CHECK(t.computed(b).color == Color::rgb(0x010203)); // the app sheet still wins
            UI_CHECK(t.themeColor(hashId("--bg"), {}) == bg);
        }
    UI_CHECK_EQ(t.styles().sheetCount(), count);
    UI_CHECK_EQ(t.themeStyleSheet(), slot);
    UI_CHECK(w::setTheme(t, w::ThemeVariant::Dark).has_value());
    t.update();
    UI_CHECK(t.computed(t.root()).background == darkBg);
    // an accent / a custom high-contrast palette go through the same slot
    UI_CHECK(w::setTheme(t, {.variant = w::ThemeVariant::Light, .hasAccent = true, .accent = Color::rgb(0xCC3366)}).has_value());
    UI_CHECK(t.themeColor(hashId("--accent"), {}) == Color::rgb(0xCC3366));
    w::HighContrastPalette pal;
    pal.window = Color::rgb(0x000020);
    pal.windowText = Color::rgb(0xF0F0F0);
    UI_CHECK(w::setTheme(t, {.variant = w::ThemeVariant::HighContrast, .contrast = pal}).has_value());
    t.update();
    UI_CHECK(t.computed(t.root()).background == Color::rgb(0x000020));
    UI_CHECK_EQ(t.styles().sheetCount(), count);
}

// ---- token_only ------------------------------------------------------------------------------------------------------
void gateTokenOnly() {
    UI_CHECK(w::themeTokenCount() >= 60u);
    for (w::ThemeVariant v : kVariants) {
        const std::string_view css = w::themeCss(v);
        auto sheet = StyleSheet::parse(css);
        UI_CHECK(sheet.has_value());
        if (!sheet) continue;
        // every token is defined in this variant and is a colour
        for (u32 i = 0; i < w::themeTokenCount(); ++i) {
            const std::string_view name = w::themeTokenName(i);
            std::string_view raw;
            UI_CHECK(sheet->token(hashId(name), raw));
            Color c;
            if (!w::themeTokenColor({.variant = v}, name, c)) {
                std::fprintf(stderr, "%s: token %.*s is not a colour\n", variantName(v), int(name.size()), name.data());
                UI_CHECK(false);
            }
        }
        // after the :root block: no literal colour (#rgb, rgb(), rgba(), named white / black)
        const std::size_t rootEnd = css.find("}\n");
        UI_CHECK(rootEnd != std::string_view::npos && css.substr(0, 8).find(":root") != std::string_view::npos);
        const std::string_view rules = css.substr(rootEnd);
        for (std::size_t i = 0; i + 1 < rules.size(); ++i) {
            const bool hex = rules[i] == '#' && std::isxdigit(static_cast<unsigned char>(rules[i + 1]));
            const bool fn = rules.substr(i, 4) == "rgb(" || rules.substr(i, 5) == "rgba(";
            if (hex || fn) {
                std::fprintf(stderr, "%s theme: literal colour in the rules: %.*s\n", variantName(v), 24, rules.data() + i);
                UI_CHECK(false);
                break;
            }
        }
        for (std::string_view word : {": white", ": black", " white;", " black;"}) UI_CHECK(rules.find(word) == std::string_view::npos);
    }
    // the dark sheet is the default one and the variants differ only in :root (same rule text for dark and light)
    UI_CHECK(w::defaultThemeCss() == w::themeCss(w::ThemeVariant::Dark));
    const std::string_view d = w::themeCss(w::ThemeVariant::Dark), l = w::lightThemeCss();
    UI_CHECK(d.substr(d.find("}\n")) == l.substr(l.find("}\n")));
    UI_CHECK(w::highContrastThemeCss() == w::themeCss(w::ThemeVariant::HighContrast));

    // paint code reads tokens: a slider's track is painted in --track, whatever a later sheet sets it to
    UiContext ui({.viewport = {200, 40}, .font = &font()});
    w::install(ui);
    const NodeHandle s = w::slider(ui, ui.root(), 0.f, 1.f, 0.f, 0.f, "s");
    ui.setCss(s, "width", "180px");
    auto trackPixel = [&] {
        ui.update();
        Image img;
        render(ui, img);
        const Rect r = ui.contentRect(s);
        return px(img, int(r.x + r.w * 0.75f), int(r.y + r.h * 0.5f));
    };
    const u32 dark = trackPixel();
    UI_CHECK_EQ(dark, 0xFF665A5Au); // #5a5a66, the value the literal had (ABGR)
    UI_CHECK(ui.addStyleSheet(":root { --track: #00ff00; }").has_value());
    UI_CHECK_EQ(trackPixel(), 0xFF00FF00u);
    UI_CHECK(w::setTheme(ui, w::ThemeVariant::Light).has_value()); // the theme slot is under the override: still green
    UI_CHECK_EQ(trackPixel(), 0xFF00FF00u);
    // a context without the widget theme: paint falls back to the dark literals
    UiContext bare({.viewport = {200, 40}, .font = &font()});
    w::registerTierA(bare);
    const NodeHandle bs = w::slider(bare, bare.root(), 0.f, 1.f, 0.f, 0.f, "s");
    bare.setCss(bs, "width", "180px");
    bare.setCss(bs, "height", "22px");
    bare.update();
    Image img;
    render(bare, img);
    const Rect r = bare.contentRect(bs);
    UI_CHECK_EQ(px(img, int(r.x + r.w * 0.75f), int(r.y + r.h * 0.5f)), 0xFF665A5Au);
}

// ---- contrast --------------------------------------------------------------------------------------------------------
/// A gallery with every text-bearing widget kind of the library.
void buildGallery(UiContext& ui) {
    const NodeHandle root = ui.root();
    ui.setCss(root, "padding", "8px");
    ui.setCss(root, "gap", "6px");
    const NodeHandle mb = w::menuBar(ui, root, "mb");
    w::menuBarMenu(ui, mb, "File", "New\nOpen");
    const NodeHandle tb = w::toolBar(ui, root, "tb");
    w::toolBarAction(ui, tb, "Run");
    auto row = [&] {
        const NodeHandle r = w::panel(ui, root);
        ui.addClass(r, "row");
        return r;
    };
    NodeHandle r = row();
    w::label(ui, r, "label");
    ui.addClass(w::label(ui, r, "dim"), "dim");
    ui.addClass(w::label(ui, r, "title"), "title");
    w::button(ui, r, "button");
    ui.addClass(w::button(ui, r, "primary"), "primary");
    w::toolButton(ui, r, "tool");
    w::link(ui, r, "link");
    r = row();
    w::checkBox(ui, r, "check", true);
    w::radioButton(ui, r, "radio", true);
    w::toggleSwitch(ui, r, "switch", true);
    w::lineEdit(ui, r, "edit");
    w::spinBox(ui, r, 0.f, 9.f, 3.f);
    w::comboBox(ui, r, "one\ntwo", 0);
    r = row();
    w::tabBar(ui, r, "Tab A\nTab B", 0);
    w::listView(ui, r, "row one\nrow two", 0);
    w::treeView(ui, r, "root\n child");
    const NodeHandle card = w::panel(ui, r);
    ui.addClass(card, "card");
    w::label(ui, card, "card text");
    ui.addClass(w::label(ui, card, "card dim"), "dim");
    r = row();
    const NodeHandle gb = w::groupBox(ui, r, "Group", false, true);
    w::label(ui, w::groupBoxBody(ui, gb), "in group");
    w::markdownView(ui, r, "# H1\n## H2\n### H3\ntext\n\n- item\n\n> quote\n\n```\ncode\n```");
    w::dateTimeEdit(ui, r, 2026, 11, 13);
    w::textEdit(ui, r, "text edit");
    w::headerView(ui, root, "Name\nKind", 100.f);
    const NodeHandle sb = w::statusBar(ui, root, "sb");
    w::statusMessage(ui, sb, "status");
    w::statusBarPermanent(ui, sb, "perm");
    w::toast(ui, "toast", 60.f);
    w::messageBox(ui, "Title", "Message text", "OK\nCancel", w::MessageIcon::Warning);
}

struct TokenPair { const char* text; const char* bg; };
/// Text the paint code draws in a token colour, with the token it is drawn on.
const TokenPair kPairs[] = {
    {"--text", "--bg"}, {"--text", "--surface"}, {"--text", "--surface-hi"}, {"--text", "--field"}, {"--text", "--field-alt"},
    {"--text", "--canvas"}, {"--text", "--hover"}, {"--text", "--pressed"}, {"--text", "--tooltip-bg"}, {"--text", "--raised"},
    {"--text", "--strip"}, {"--text", "--node-head"}, {"--text", "--media-bar"}, {"--text", "--dropzone"},
    {"--text-dim", "--bg"}, {"--text-dim", "--surface"}, {"--text-dim", "--quote-bg"}, {"--text-dim", "--field"},
    {"--heading", "--bg"}, {"--heading", "--surface"}, {"--accent-hi", "--bg"}, {"--accent-hi", "--surface"},
    {"--link-hover", "--bg"}, {"--on-accent", "--accent"}, {"--on-accent", "--danger"}, {"--on-accent", "--warning"},
    {"--code-text", "--code-bg"}, {"--text-faint", "--strip"}, {"--text-faint", "--canvas"},
    {"--code-fg", "--code-editor-bg"}, {"--code-keyword", "--code-editor-bg"}, {"--code-string", "--code-editor-bg"},
    {"--code-number", "--code-editor-bg"}, {"--code-comment", "--code-editor-bg"}, {"--code-fg", "--selection"},
    {"--gutter-text", "--gutter"}, {"--gutter-text-hi", "--gutter"}, {"--dropzone-glyph", "--dropzone"},
};
/// Dark values below 4.5 (frozen by the dark goldens): the pair or widget, and the floor it must keep.
struct Known { const char* what; f32 floor; };
const Known kDarkKnown[] = {
    {"--on-accent on --accent", 3.3f},      // white on #3d8bfd: primary buttons, selected calendar day, check marks
    {"--on-accent on --danger", 3.8f},      // message box error badge
    {"--on-accent on --warning", 2.2f},     // message box warning badge
    {"--gutter-text on --gutter", 2.9f},    // code editor line numbers (the current line number passes)
    {"button.primary", 3.3f},               // computed: white text on the accent face
    {".msgicon", 2.2f},                     // computed: badge letter on the warning / accent / danger disc
};
bool knownDark(const std::string& what, f32 ratio) {
    for (const Known& k : kDarkKnown)
        if (what.find(k.what) != std::string::npos) {
            if (ratio < k.floor) std::fprintf(stderr, "dark: %s fell to %.2f (floor %.2f)\n", what.c_str(), double(ratio), double(k.floor));
            else std::printf("  dark, known: %s = %.2f (floor %.2f)\n", what.c_str(), double(ratio), double(k.floor));
            return ratio >= k.floor;
        }
    return false;
}

void gateContrast() {
    for (w::ThemeVariant v : kVariants) {
        const f32 need = v == w::ThemeVariant::HighContrast ? 7.f : 4.5f;
        f32 worst = 21.f;
        int measured = 0, excused = 0;
        auto judge = [&](const std::string& what, f32 ratio) {
            ++measured;
            if (ratio >= need) { worst = std::min(worst, ratio); return; }
            if (v == w::ThemeVariant::Dark && knownDark(what, ratio)) { ++excused; return; }
            std::fprintf(stderr, "%s: %s contrast %.2f < %.1f\n", variantName(v), what.c_str(), double(ratio), double(need));
            UI_CHECK(false);
        };
        // (1) computed styles of the gallery
        UiContext ui({.viewport = {900, 700}, .font = &font()});
        w::install(ui);
        UI_CHECK(w::setTheme(ui, v).has_value());
        buildGallery(ui);
        for (int i = 0; i < 30; ++i) ui.update(1.f / 60.f); // transitions and enter animations run out
        ui.update(0.f);
        const Color rootBg = ui.computed(ui.root()).background;
        UI_CHECK(rootBg.a == 1.f);
        std::vector<NodeHandle> stack{ui.root()}, chain;
        int textNodes = 0;
        while (!stack.empty()) {
            const NodeHandle n = stack.back();
            stack.pop_back();
            for (NodeHandle c = ui.firstChild(n); c.valid(); c = ui.nextSibling(c)) stack.push_back(c);
            const ComputedStyle& cs = ui.computed(n);
            if (ui.text(n).empty() || !cs.visible) continue;
            chain.clear();
            bool inactive = false, hidden = false;
            for (NodeHandle a = n; a.valid(); a = ui.parent(a)) {
                chain.push_back(a);
                inactive = inactive || ui.disabled(a);
                hidden = hidden || !ui.computed(a).visible;
            }
            if (inactive || hidden) continue;
            Color bg = rootBg;
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                const Color b = ui.computed(*it).background;
                if (b.a > 0.f) bg = over(b, bg);
            }
            const Color fg = over(cs.color, bg);
            std::string what(ui.registry().get(ui.classOf(n)).name);
            if (ui.hasClass(n, "primary")) what += ".primary";
            if (ui.hasClass(n, "msgicon")) what = ".msgicon";
            what += " \"" + std::string(ui.text(n).substr(0, 16)) + "\"";
            for (char& ch : what)
                if (ch == '\n') ch = ' ';
            judge(what, color::contrastRatio(fg, bg));
            ++textNodes;
        }
        UI_CHECK(textNodes >= 35);
        // (2) token pairs of the paint code
        for (const TokenPair& p : kPairs) {
            Color fg, bg, base;
            UI_CHECK(w::themeTokenColor({.variant = v}, p.text, fg) && w::themeTokenColor({.variant = v}, p.bg, bg));
            UI_CHECK(w::themeTokenColor({.variant = v}, "--bg", base));
            bg = over(bg, base);
            judge(std::string(p.text) + " on " + p.bg, color::contrastRatio(over(fg, bg), bg));
        }
        std::printf("  %-8s %d text / background pairs, need %.1f, worst passing %.2f, known dark exceptions %d\n", variantName(v),
                    measured, double(need), double(worst), excused);
        if (v != w::ThemeVariant::Dark) UI_CHECK_EQ(excused, 0);
    }
    // a custom high-contrast palette keeps the guarantee as long as the palette itself does
    w::HighContrastPalette pal;
    pal.window = Color::rgb(0xFFFFFF);
    pal.windowText = Color::rgb(0x000000);
    pal.buttonFace = Color::rgb(0xFFFFFF);
    pal.highlight = Color::rgb(0x37006E);
    pal.highlightText = Color::rgb(0xFFFFFF);
    pal.grayText = Color::rgb(0x600000);
    pal.hotlight = Color::rgb(0x00009F);
    const w::ThemeDesc white{.variant = w::ThemeVariant::HighContrast, .contrast = pal};
    for (const TokenPair& p : kPairs) {
        Color fg, bg;
        UI_CHECK(w::themeTokenColor(white, p.text, fg) && w::themeTokenColor(white, p.bg, bg));
        const f32 ratio = color::contrastRatio(over(fg, over(bg, pal.window)), over(bg, pal.window));
        if (ratio < 7.f) std::fprintf(stderr, "white high contrast: %s on %s = %.2f\n", p.text, p.bg, double(ratio));
        UI_CHECK(ratio >= 7.f);
    }
}

// ---- paint_literals --------------------------------------------------------------------------------------------------
#ifndef FUSE_UI_WIDGETS_SRC
#define FUSE_UI_WIDGETS_SRC ""
#endif
void gatePaintLiterals() {
    namespace fs = std::filesystem;
    std::error_code ec;
    int files = 0, tokens = 0, exempt = 0, bad = 0;
    for (const fs::directory_entry& e : fs::directory_iterator(FUSE_UI_WIDGETS_SRC, ec)) {
        if (e.path().extension() != ".cpp") continue;
        ++files;
        std::ifstream in(e.path());
        std::string line;
        for (int n = 1; std::getline(in, line); ++n) {
            for (std::size_t at = line.find("0x"); at != std::string::npos; at = line.find("0x", at + 2)) {
                // a colour literal: `Color::rgb(0x` directly, or the fallback argument of themed( / themedA(
                const std::string_view before = std::string_view(line).substr(0, at);
                if (before.ends_with("Color::rgb(")) {
                    if (line.find("theme-exempt") != std::string::npos) { ++exempt; continue; }
                    std::fprintf(stderr, "%s:%d: hard-coded colour in widget code: use themed(pc, tok::..., 0x...) or mark the line "
                                         "theme-exempt\n", e.path().filename().string().c_str(), n);
                    ++bad;
                } else if (before.rfind("themed(") != std::string_view::npos || before.rfind("themedA(") != std::string_view::npos) {
                    ++tokens;
                }
            }
        }
    }
    std::printf("  %d widget sources: %d token colours, %d exempt data-colour literals, %d hard-coded\n", files, tokens, exempt, bad);
    UI_CHECK(files >= 10);   // the directory was found
    UI_CHECK(tokens >= 100); // the paint code does read tokens
    UI_CHECK_EQ(bad, 0);
}

// ---- focus_ring ------------------------------------------------------------------------------------------------------
void gateFocusRing() {
    for (w::ThemeVariant v : kVariants) {
        UiContext probe({.viewport = {8, 8}, .font = &font()});
        w::install(probe);
        std::vector<std::string> classes;
        for (u32 id = 0; id < probe.registry().size(); ++id)
            if (probe.registry().get(id).focusable) classes.emplace_back(probe.registry().get(id).name);
        UI_CHECK(classes.size() >= 25);
        int shown = 0;
        for (const std::string& cls : classes) {
            UiContext ui({.viewport = {200, 120}, .font = &font()});
            w::install(ui);
            UI_CHECK(w::setTheme(ui, v).has_value());
            ui.setCss(ui.root(), "padding", "20px");
            auto made = ui.create(cls, ui.root(), "probe");
            UI_CHECK(made.has_value());
            if (!made) continue;
            const NodeHandle n = *made;
            ui.setCss(n, "width", "140px");
            ui.setCss(n, "height", "60px");
            ui.setText(n, "Focus");
            auto shot = [&](Image& img) {
                for (int i = 0; i < 30; ++i) ui.update(1.f / 60.f);
                ui.update(0.f);
                render(ui, img);
            };
            Image plain, focused;
            shot(plain);
            ui.setFocus(n, FocusReason::Keyboard);
            shot(focused);
            if (ui.focused() != n) { // a class that hands focus to a child (composite): the child is in the list itself
                std::printf("  %s: %s passes focus on\n", variantName(v), cls.c_str());
                continue;
            }
            UI_CHECK((ui.states(n) & kStateFocusVisible) != 0);
            const Rect r = ui.rect(n);
            int diff = 0;
            for (int y = std::max(0, int(r.y) - 2); y < std::min(plain.height, int(r.bottom()) + 2); ++y)
                for (int x = std::max(0, int(r.x) - 2); x < std::min(plain.width, int(r.right()) + 2); ++x)
                    diff += px(plain, x, y) != px(focused, x, y);
            if (diff < 40) {
                std::fprintf(stderr, "%s: class %s shows no keyboard focus (%d pixels differ)\n", variantName(v), cls.c_str(), diff);
                UI_CHECK(false);
            } else {
                ++shown;
            }
        }
        std::printf("  %-8s %d focusable classes, %d show keyboard focus\n", variantName(v), int(classes.size()), shown);
        UI_CHECK(shown >= 25);
    }
}
} // namespace

int main(int argc, char** argv) {
    std::string gate;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--gate=", 7) == 0) gate = argv[i] + 7;
    if (gate == "sheet_handles") gateSheetHandles();
    else if (gate == "token_only") gateTokenOnly();
    else if (gate == "contrast") gateContrast();
    else if (gate == "focus_ring") gateFocusRing();
    else if (gate == "paint_literals") gatePaintLiterals();
    else {
        std::fprintf(stderr, "unknown gate '%s'\n", gate.c_str());
        return 2;
    }
    return finish(("fuse_ui_theme_" + gate).c_str());
}
