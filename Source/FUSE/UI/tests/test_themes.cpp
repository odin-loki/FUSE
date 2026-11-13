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
//                  `theme-exempt` (data colours: the colour picker's gradients and markers). Scans the sources. Also
//                  App/src (the title bar / client-side decoration): no colour literal in code or style strings; and
//                  every `tok::` name of widgets_internal.hpp is a token of the theme table.
//   focus_ring     every focusable class of the library shows keyboard focus: rendering a node of the class with
//                  and without :focus-visible differs inside its box, in each of the three variants
#include "test_util.hpp"

#include "fuse/ui/color_space.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
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

    // the rules of the ported widgets are in the sheet (and so under the checks above)
    for (std::string_view sel : {"completer {", "completeritem {", "editcombo {", "combodrop {", ".dialog-buttonbox {", ".toolbox {",
                                 ".toolbox-header.open {", "lcdnumber {", "keysequenceedit {", "mdiarea {", "mdisubwindow {",
                                 "mdititle {", "graphicsview {", "vkeyboard {", ".vkey:checked {"})
        UI_CHECK(d.find(sel) != std::string_view::npos);

    // paint code reads tokens: a slider's track is painted in --track, whatever a later sheet sets it to
    UiContext ui({.viewport = {200, 40}, .font = &font()});
    w::install(ui);
    // every token of the table is answered from the engine's colour cache once asked for (none falls back to
    // resolving its sheet text on every paint), together with a handful of application tokens
    UI_CHECK(ui.addStyleSheet(":root { --app-a: #010203; --app-b: #040506; --app-c: var(--app-a); --app-d: #070809; }").has_value());
    for (int pass = 0; pass < 2; ++pass)
        for (u32 i = 0; i < w::themeTokenCount() + 4; ++i) {
            const char* app[] = {"--app-a", "--app-b", "--app-c", "--app-d"};
            const std::string_view name = i < w::themeTokenCount() ? w::themeTokenName(i) : std::string_view(app[i - w::themeTokenCount()]);
            Color want;
            if (i < w::themeTokenCount()) UI_CHECK(w::themeTokenColor({}, name, want));
            else want = Color::rgb(i - w::themeTokenCount() == 1 ? 0x040506 : i - w::themeTokenCount() == 3 ? 0x070809 : 0x010203);
            UI_CHECK(ui.themeColor(hashId(name), Color::rgb(0xFF00FF)) == want);
            if (!ui.styles().tokenColorCached(hashId(name))) {
                std::fprintf(stderr, "token %.*s is not cached\n", int(name.size()), name.data());
                UI_CHECK(false);
            }
        }
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
    // the ported widgets and options (widgets/edit_options.hpp, widgets/extra.hpp, dock splits)
    r = row();
    w::setLineEditOptions(ui, w::lineEdit(ui, r, "", "placeholder"), {.placeholder = "Placeholder"});
    w::setLineEditOptions(ui, w::lineEdit(ui, r, "secret", "password"), {.echo = w::EchoMode::Password});
    w::setSpinBoxOptions(ui, w::spinBox(ui, r, 0.f, 9.f, 3.f, 1.f, "optspin"), {.prefix = "$ ", .suffix = " ea"});
    w::editableComboBox(ui, r, "Red\nGreen", 1, w::InsertPolicy::NoInsert, "editcombo");
    w::keySequenceEdit(ui, r, Shortcut{Key::S, kModCtrl});
    r = row();
    const NodeHandle tabs = w::tabBar(ui, r, "Open\nOff\nLast", 0, "opttabs");
    w::setTabsClosable(ui, tabs, true);
    w::setTabEnabled(ui, tabs, 1, false);
    w::setLcdText(ui, w::lcdNumber(ui, r, 4), "12.5");
    const NodeHandle tbox = w::toolBox(ui, r, "General\nAdvanced");
    w::label(ui, w::toolBoxPage(ui, tbox, 1), "tool box page");
    w::setToolBoxIndex(ui, tbox, 1);
    w::dialogButtonBox(ui, r, w::StdButton::Ok | w::StdButton::Cancel | w::StdButton::Help);
    r = row();
    const NodeHandle mdi = w::mdiArea(ui, r);
    ui.setCss(mdi, "width", "260px");
    ui.setCss(mdi, "height", "120px");
    w::label(ui, w::mdiBody(ui, w::mdiSubWindow(ui, mdi, "Document", {8, 8, 160, 90})), "mdi body");
    w::label(ui, w::mdiBody(ui, w::mdiSubWindow(ui, mdi, "Active", {60, 30, 160, 80})), "active body");
    const NodeHandle view = w::graphicsView(ui, r, nullptr);
    ui.setCss(view, "width", "80px");
    ui.setCss(view, "height", "60px");
    const NodeHandle kb = w::virtualKeyboard(ui, r);
    ui.setChecked(w::virtualKeyboardKey(ui, kb, "Shift"), true); // a latched key: --on-accent on --accent
    const NodeHandle area = w::dockArea(ui, root, "docks");
    ui.setCss(area, "height", "120px");
    const NodeHandle da = w::dockWidget(ui, area, "Outline", w::DockSide::Left, "dock.a");
    const NodeHandle db = w::dockWidget(ui, area, "Assets", w::DockSide::Left, "dock.b");
    const NodeHandle dc = w::dockWidget(ui, area, "History", w::DockSide::Left, "dock.c");
    w::label(ui, w::dockBody(ui, da), "dock body");
    ui.update();
    UI_CHECK(w::dockSplit(ui, db, da, w::DockEdge::Bottom) && w::dockSplit(ui, dc, db, w::DockEdge::Center));
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
    // the ported widgets: LCD digits, placeholder / "None" text of fields, the open tool box header, text over the MDI
    // workspace and the graphics view, the latched on-screen key (--on-accent on --accent, above), the title bar's
    // close button under the pointer (App/src/chrome.cpp)
    {"--lcd-text", "--lcd-bg"}, {"--placeholder", "--field"}, {"--placeholder", "--surface-hi"}, {"--heading", "--surface-hi"},
    {"--text", "--workspace"}, {"--text", "--scene-bg"}, {"--on-accent", "--window-close"},
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
    {".vkey:checked", 3.3f},                // computed: a latched on-screen keyboard key, white on the accent face
    {"--placeholder on --field", 4.4f},     // placeholder / "None" text: the text colour at half alpha (port_edit_options)
    {"--placeholder on --surface-hi", 3.7f}, // the same inside an editable combo box
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
        std::set<std::string> seen;
        std::vector<std::string> classes;
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
            if (ui.hasClass(n, "vkey") && ui.checked(n)) what = ".vkey:checked";
            if (ui.hasClass(n, "toolbox-header")) what += ui.hasClass(n, "open") ? ".toolbox-header.open" : ".toolbox-header";
            if (seen.insert(what).second) classes.push_back(what);
            what += " \"" + std::string(ui.text(n).substr(0, 16)) + "\"";
            for (char& ch : what)
                if (ch == '\n') ch = ' ';
            judge(what, color::contrastRatio(fg, bg));
            ++textNodes;
        }
        UI_CHECK(textNodes >= 35);
        // the widgets ported after the first theme audit are in the measured set
        for (const char* need : {"editcombo", "keysequenceedit", ".vkey:checked", "button.toolbox-header.open",
                                 "button.toolbox-header", "docktitle", "tabbar", "lineedit"})
            if (!seen.count(need)) {
                std::fprintf(stderr, "%s: no text node of %s was measured\n", variantName(v), need);
                UI_CHECK(false);
            }
        if (v == w::ThemeVariant::Dark) {
            std::printf("  measured classes:");
            for (const std::string& c : classes) std::printf(" %s", c.c_str());
            std::printf("\n");
        }
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
#ifndef FUSE_UI_APP_SRC
#define FUSE_UI_APP_SRC ""
#endif
/// The argument text of the call that opens at `open` (the index just after its '('), up to the matching ')'.
std::string_view callArgs(std::string_view line, std::size_t open) {
    int depth = 1;
    for (std::size_t i = open; i < line.size(); ++i) {
        if (line[i] == '(') ++depth;
        else if (line[i] == ')' && --depth == 0) return line.substr(open, i - open);
    }
    return line.substr(open);
}
/// True when `args` (the arguments of a Color::rgb / themed call) holds a colour literal: `0xRRGGBB` as a value of its
/// own - at the start, after `,`, or as a branch of `?:` - not a mask (`x & 0xFFFFFFu`) or a shift operand.
bool hasColourLiteral(std::string_view args) {
    for (std::size_t at = args.find("0x"); at != std::string_view::npos; at = args.find("0x", at + 2)) {
        std::size_t n = 0;
        while (at + 2 + n < args.size() && std::isxdigit(static_cast<unsigned char>(args[at + 2 + n]))) ++n;
        if (n < 6) continue;
        std::size_t b = at;
        while (b > 0 && args[b - 1] == ' ') --b;
        if (b == 0 || args[b - 1] == ',' || args[b - 1] == '?' || args[b - 1] == ':' || args[b - 1] == '(') return true;
    }
    return false;
}
bool hexColourAt(std::string_view t, std::size_t i) { // "#rgb" .. "#rrggbbaa" inside a style string
    if (t[i] != '#') return false;
    std::size_t n = 0;
    while (i + 1 + n < t.size() && std::isxdigit(static_cast<unsigned char>(t[i + 1 + n]))) ++n;
    const bool wordEnd = i + 1 + n >= t.size() || !std::isalnum(static_cast<unsigned char>(t[i + 1 + n]));
    return wordEnd && (n == 3 || n == 4 || n == 6 || n == 8);
}
void gatePaintLiterals() {
    namespace fs = std::filesystem;
    std::error_code ec;
    int files = 0, tokens = 0, exempt = 0, bad = 0;
    // (1) Widgets/src: every Color::rgb( with a hex literal in its argument (also behind a condition:
    //     `Color::rgb(on ? 0x.. : 0x..)`) must be the fallback of themed( / themedA( or sit on a theme-exempt line
    std::set<std::string> widgetSources;
    for (const fs::directory_entry& e : fs::directory_iterator(FUSE_UI_WIDGETS_SRC, ec)) {
        if (e.path().extension() != ".cpp") continue;
        ++files;
        widgetSources.insert(e.path().filename().string());
        std::ifstream in(e.path());
        std::string line;
        for (int n = 1; std::getline(in, line); ++n) {
            const bool marked = line.find("theme-exempt") != std::string::npos;
            for (std::size_t at = line.find("Color::rgb("); at != std::string::npos; at = line.find("Color::rgb(", at + 11)) {
                if (!hasColourLiteral(callArgs(line, at + 11))) continue; // a variable, a table entry, a parsed value
                if (marked) { ++exempt; continue; }
                std::fprintf(stderr, "%s:%d: hard-coded colour in widget code: use themed(pc, tok::..., 0x...) or mark the line "
                                     "theme-exempt\n", e.path().filename().string().c_str(), n);
                ++bad;
            }
            for (const char* fn : {"themed(", "themedA("})
                for (std::size_t at = line.find(fn); at != std::string::npos; at = line.find(fn, at + 1))
                    tokens += hasColourLiteral(callArgs(line, at + std::strlen(fn)));
        }
    }
    std::printf("  %d widget sources: %d token colours, %d exempt data-colour literals, %d hard-coded\n", files, tokens, exempt, bad);
    UI_CHECK(files >= 10);   // the directory was found
    UI_CHECK(tokens >= 100); // the paint code does read tokens
    UI_CHECK_EQ(bad, 0);
    // the sources of the widget port are in the scanned set
    for (const char* f : {"widgets_extra.cpp", "widgets_buttonbox.cpp", "widgets_lineedit.cpp", "widgets_spinbox.cpp", "widgets_tabs.cpp",
                          "completer.cpp", "widgets_tier_b2.cpp", "widgets_tier_d2.cpp"})
        UI_CHECK(widgetSources.count(f) == 1);

    // (2) App/src (window chrome, client-side decoration, the platform backends): no Color::rgb(0x...) and no literal
    //     colour in a style string; the title bar's sheet uses tokens, so a theme switch restyles it. Objective-C++
    //     sources are not scanned (not built here).
    int appFiles = 0, appBad = 0;
    bool sawChrome = false;
    for (const fs::directory_entry& e : fs::directory_iterator(FUSE_UI_APP_SRC, ec)) {
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".inl" && ext != ".hpp") continue;
        ++appFiles;
        const std::string name = e.path().filename().string();
        sawChrome = sawChrome || name == "chrome.cpp";
        std::ifstream in(e.path());
        std::string line;
        for (int n = 1; std::getline(in, line); ++n) {
            if (line.find("theme-exempt") != std::string::npos) continue;
            bool hit = false;
            for (std::size_t at = line.find("Color::rgb("); at != std::string::npos && !hit; at = line.find("Color::rgb(", at + 11))
                hit = hasColourLiteral(callArgs(line, at + 11));
            // a string literal that is style text (has a `:` declaration) with a colour literal in it
            bool inString = false;
            for (std::size_t i = 0; i < line.size() && !hit; ++i) {
                if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) inString = !inString;
                else if (inString && (hexColourAt(line, i) || line.compare(i, 5, "rgba(") == 0 || line.compare(i, 4, "rgb(") == 0))
                    hit = line.find(": ") != std::string::npos || line.find("background") != std::string::npos;
            }
            if (hit) {
                std::fprintf(stderr, "%s:%d: hard-coded colour in fuse_app code: use a theme token (var(--...)) or mark the line "
                                     "theme-exempt\n", name.c_str(), n);
                ++appBad;
            }
        }
    }
    std::printf("  %d fuse_app sources: %d hard-coded colours\n", appFiles, appBad);
    UI_CHECK(appFiles >= 10 && sawChrome);
    UI_CHECK_EQ(appBad, 0);

    // (3) every token the paint code names (widgets_internal.hpp `tok::`) is a token of the theme table, in every variant
    std::ifstream hdr(std::string(FUSE_UI_WIDGETS_SRC) + "/widgets_internal.hpp");
    std::string line;
    int named = 0;
    bool inTok = false;
    while (std::getline(hdr, line)) {
        if (line.find("@tok-begin") != std::string::npos) inTok = true;
        else if (line.find("@tok-end") != std::string::npos) inTok = false;
        const std::size_t at = line.find("hashId(\"--");
        if (!inTok || at == std::string::npos) continue;
        const std::string name = line.substr(at + 8, line.find('"', at + 8) - (at + 8));
        ++named;
        for (w::ThemeVariant v : kVariants) {
            Color c;
            if (!w::themeTokenColor({.variant = v}, name, c)) {
                std::fprintf(stderr, "tok:: names %s, which the %s theme does not define\n", name.c_str(), variantName(v));
                UI_CHECK(false);
            }
        }
    }
    std::printf("  %d paint tokens named in widgets_internal.hpp, %u in the theme table\n", named, w::themeTokenCount());
    UI_CHECK(named >= 80);
    UI_CHECK_EQ(u32(named), w::themeTokenCount());
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
        for (const char* cls : {"keysequenceedit", "graphicsview", "splithandle", "lineedit", "spinbox", "tabbar"}) // in the list
            UI_CHECK(std::find(classes.begin(), classes.end(), cls) != classes.end());

        // Widgets the class loop cannot reach: composites that keep the focus in a child, and the option-driven paint
        // paths (a line edit / spin box / tab bar with options paints in widgets_lineedit / _spinbox / _tabs.cpp).
        // `box` is where the ring must show, `target` takes the keyboard focus.
        // `inside`: the widget shows its focus inside the box (a split handle lights its grip), not on the outline.
        struct Probe { const char* name; void (*build)(UiContext& ui, NodeHandle& box, NodeHandle& target); bool inside = false; };
        static const Probe kProbes[] = {
            {"editcombo (focus in its field)", [](UiContext& ui, NodeHandle& box, NodeHandle& target) {
                 box = w::editableComboBox(ui, ui.root(), "Red\nGreen", 0, w::InsertPolicy::NoInsert, "c");
                 target = w::comboLineEdit(ui, box);
             }},
            {"lineedit with options", [](UiContext& ui, NodeHandle& box, NodeHandle& target) {
                 box = target = w::lineEdit(ui, ui.root(), "", "e");
                 w::setLineEditOptions(ui, box, {.placeholder = "Placeholder", .inputMask = "000.000;_"});
             }},
            {"lineedit with a completer", [](UiContext& ui, NodeHandle& box, NodeHandle& target) {
                 box = target = w::lineEdit(ui, ui.root(), "", "e");
                 w::setCompleter(ui, box, {.items = "Apple\nApricot"});
             }},
            {"spinbox with options", [](UiContext& ui, NodeHandle& box, NodeHandle& target) {
                 box = target = w::spinBox(ui, ui.root(), 0.f, 9.f, 3.f, 1.f, "s");
                 w::setSpinBoxOptions(ui, box, {.prefix = "$ ", .suffix = " ea"});
             }},
            {"tabbar with options", [](UiContext& ui, NodeHandle& box, NodeHandle& target) {
                 box = target = w::tabBar(ui, ui.root(), "One\nTwo\nThree", 0, "t");
                 w::setTabsClosable(ui, box, true);
                 w::setTabsMovable(ui, box, true);
             }},
            {"keysequenceedit", [](UiContext& ui, NodeHandle& box, NodeHandle& target) {
                 box = target = w::keySequenceEdit(ui, ui.root(), Shortcut{Key::S, kModCtrl});
             }},
            {"graphicsview", [](UiContext& ui, NodeHandle& box, NodeHandle& target) {
                 box = target = w::graphicsView(ui, ui.root(), nullptr);
                 ui.setCss(box, "height", "50px");
             }},
            {"toolbox header", [](UiContext& ui, NodeHandle& box, NodeHandle& target) {
                 const NodeHandle tb = w::toolBox(ui, ui.root(), "General\nAdvanced");
                 box = target = ui.firstChild(tb);
             }},
            {"dialog button box button", [](UiContext& ui, NodeHandle& box, NodeHandle& target) {
                 const NodeHandle bb = w::dialogButtonBox(ui, ui.root(), w::StdButton::Ok | w::StdButton::Cancel);
                 box = target = w::dialogButtonBoxButton(ui, bb, w::StdButton::Cancel);
             }},
            {"docksplit handle", [](UiContext& ui, NodeHandle& box, NodeHandle& target) {
                 const NodeHandle area = w::dockArea(ui, ui.root(), "area");
                 ui.setCss(area, "height", "70px");
                 const NodeHandle a = w::dockWidget(ui, area, "A", w::DockSide::Left, "a");
                 const NodeHandle b = w::dockWidget(ui, area, "B", w::DockSide::Left, "b");
                 ui.update();
                 UI_CHECK(w::dockSplit(ui, b, a, w::DockEdge::Bottom));
                 ui.update();
                 const NodeHandle split = w::dockSplitOf(ui, a);
                 UI_CHECK(ui.hasClass(split, "docksplit"));
                 box = target = ui.nextSibling(ui.parent(a)); // the handle between the two panes
             }, true},
        };
        int probed = 0;
        for (const Probe& p : kProbes) {
            UiContext ui({.viewport = {260, 140}, .font = &font()});
            w::install(ui);
            UI_CHECK(w::setTheme(ui, v).has_value());
            ui.setCss(ui.root(), "padding", "20px");
            NodeHandle box, target;
            p.build(ui, box, target);
            UI_CHECK(ui.alive(box) && ui.alive(target));
            if (!ui.alive(box) || !ui.alive(target)) continue;
            auto shot = [&](Image& img) {
                for (int i = 0; i < 30; ++i) ui.update(1.f / 60.f);
                ui.update(0.f);
                render(ui, img);
            };
            Image plain, focused;
            shot(plain);
            ui.setFocus(target, FocusReason::Keyboard);
            shot(focused);
            if (ui.focused() != target) {
                std::fprintf(stderr, "%s: %s does not take the keyboard focus\n", variantName(v), p.name);
                UI_CHECK(false);
                continue;
            }
            // counted on the outline of `box` only (1 px band): a caret or a selection inside is not a focus ring
            const Rect r = ui.rect(box);
            int diff = 0;
            for (int y = std::max(0, int(r.y) - 1); y < std::min(plain.height, int(r.bottom()) + 1); ++y)
                for (int x = std::max(0, int(r.x) - 1); x < std::min(plain.width, int(r.right()) + 1); ++x) {
                    const bool edge = x < int(r.x) + 2 || x >= int(r.right()) - 2 || y < int(r.y) + 2 || y >= int(r.bottom()) - 2;
                    diff += (edge || p.inside) && px(plain, x, y) != px(focused, x, y);
                }
            if (diff < 20) {
                std::fprintf(stderr, "%s: %s shows no keyboard focus (%d outline pixels differ)\n", variantName(v), p.name, diff);
                UI_CHECK(false);
            } else {
                ++probed;
            }
        }
        std::printf("  %-8s %d of %d composite / option-driven widgets show keyboard focus\n", variantName(v), probed, int(std::size(kProbes)));
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
