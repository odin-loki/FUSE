// fuse_ui_widget_golden: golden images of the widgets that complete the plan §5 catalogue (they have behaviour gates in
// test_tier_b3 / test_tier_b4 / test_game3). One scene per ctest gate (`--scene=<name>`, ctest
// fuse_ui_widget_golden_<name>), each against tests/golden/widgets_<name>[_full].png with the tolerance of
// fuse_ui_cpu_raster_golden; FUSE_UI_UPDATE_GOLDEN=1 rewrites the PNG. `--out=<dir>` also writes the rendered scenes
// there. Scenes:
//   containers — tool bar, group boxes (plain, checkable checked / unchecked), header with a sorted section, stack,
//                status bar (message + permanent labels), an open popover, two toasts
//   text       — text edit (wrapped, selection, focused caret), plain text edit, rich text (every markup tag)
//   dialogs    — message box with each icon kind and with a long, wrapped text; one context per cell, tiled
//   wizard     — wizard on its first (Back disabled) and last (Finish) page; one context per cell, tiled
//   mainwindow — main window: menu bar, tool bar, tabbed docks, a floating dock, flame-graph chart, status bar
//   spinmenu   — double spin boxes (0.1 / 0.05 / 0.5 steps, focused, disabled, negative) next to a whole-number one,
//                and an open context menu with a separator and a hovered item
//   hud        — minimap (circle with a map image and rotating map, square north-up; blips incl. clamped ones) and
//                ammo counters (pips, bar fallback, low ammo, reload)        [needs fuse_ui_game]
//   controls   — the Tier A controls in their states (checked, disabled, keyboard-focused with the focus ring), tab
//                bar, list, scroll bar, dial, progress bar, table rows with stripes     [MINIMAL goldens only]
//   editors    — code editor (syntax colours, selection, gutter), timeline, node graph, curve editor, calendar,
//                date-time edit: the widgets whose paint code reads theme tokens      [MINIMAL goldens only]
// Themes: `--theme=light` / `--theme=contrast` renders the same scenes with the light / high-contrast token sets
// (ctest fuse_ui_theme_golden_<light|contrast>_<scene>, goldens widgets_<scene>_<light|contrast>.png, MINIMAL text
// profile only). The dark goldens predate keyboard focus rings; their FULL-profile twins cannot be re-rendered without
// the fetched FULL text stack, so the dark dialogs / wizard cells are captured with the focus parked (their pixels
// are the ones from before the rings existed) and the themed goldens show the rings.
// The scenes are deterministic: fixed viewports, no clocks, timers / animations advanced by explicit update(dt).
#include "test_util.hpp"

#if FUSE_UI_GOLDEN_HAVE_GAME
#include "fuse/ui/game/game_extras.hpp"
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_write_impl.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
u8 ch(u32 p, int c) { return u8((p >> (8 * c)) & 0xFF); }
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }

w::ThemeVariant gTheme = w::ThemeVariant::Dark;
/// w::install plus the theme under test.
void installThemed(UiContext& ui) {
    w::install(ui);
    if (gTheme != w::ThemeVariant::Dark) UI_CHECK(w::setTheme(ui, gTheme).has_value());
}

/// Lets style transitions (focus ring, popup fade-in, check marks) run out: half a second in 60 Hz steps.
void settle(UiContext& ui) {
    for (int i = 0; i < 30; ++i) ui.update(1.f / 60.f);
    ui.update(0.f);
}
/// Copies `cell` into `sheet` at (x, y).
void blit(Image& sheet, const Image& cell, int x, int y) {
    for (int row = 0; row < cell.height; ++row)
        std::memcpy(&sheet.pixels[std::size_t(y + row) * std::size_t(sheet.width) + std::size_t(x)],
                    &cell.pixels[std::size_t(row) * std::size_t(cell.width)], std::size_t(cell.width) * sizeof(u32));
}

// ---- containers ----------------------------------------------------------------------------------------------------
void sceneContainers(Image& img) {
    UiContext ui({.viewport = {660, 470}, .font = &font()});
    installThemed(ui);
    ui.setCss(ui.root(), "padding", "12px");
    ui.setCss(ui.root(), "gap", "10px");
    const NodeHandle tb = w::toolBar(ui, ui.root(), "tb");
    w::toolBarAction(ui, tb, "New");
    w::toolBarAction(ui, tb, "Open");
    w::toolBarSeparator(ui, tb);
    w::toolBarAction(ui, tb, "Save");
    w::toolBarAction(ui, tb, "Run");

    const NodeHandle groups = w::panel(ui, ui.root(), "groups");
    ui.addClass(groups, "row");
    const NodeHandle plain = w::groupBox(ui, groups, "Rendering", false, true, "plain");
    w::checkBox(ui, w::groupBoxBody(ui, plain), "Shadows", true);
    w::checkBox(ui, w::groupBoxBody(ui, plain), "Bloom", false);
    const NodeHandle on = w::groupBox(ui, groups, "Advanced", true, true, "on");
    w::button(ui, w::groupBoxBody(ui, on), "Tune");
    w::checkBox(ui, w::groupBoxBody(ui, on), "Temporal AA", true);
    const NodeHandle off = w::groupBox(ui, groups, "Experimental", true, false, "off");
    w::button(ui, w::groupBoxBody(ui, off), "Enable");
    w::checkBox(ui, w::groupBoxBody(ui, off), "Path tracing", false);
    for (NodeHandle g : {plain, on, off}) ui.setCss(g, "flex-grow", "1");

    const NodeHandle header = w::headerView(ui, ui.root(), "Name\nKind\nSize\nModified", 130.f, "header");
    w::setHeaderSectionWidth(ui, header, 0, 180.f);

    const NodeHandle st = w::stack(ui, ui.root(), "stack");
    w::label(ui, w::stackAddPage(ui, st), "stack page one (hidden)");
    w::label(ui, w::stackAddPage(ui, st), "Stack: page two of three is the current page");
    w::label(ui, w::stackAddPage(ui, st), "stack page three (hidden)");
    w::setStackIndex(ui, st, 1);

    const NodeHandle sb = w::statusBar(ui, ui.root(), "sb");
    w::statusMessage(ui, sb, "Saved scene.fuse");
    w::statusBarPermanent(ui, sb, "Ln 12, Col 4");
    w::statusBarPermanent(ui, sb, "UTF-8");

    const NodeHandle anchor = w::button(ui, ui.root(), "Filters", "filters");
    ui.setCss(anchor, "align-self", "flex-start");
    ui.update();
    // sort by "Kind", descending (two clicks), then park the pointer on empty background
    const Rect hc = ui.contentRect(header);
    for (int i = 0; i < 2; ++i) {
        const Vec2 p{hc.x + 180.f + 60.f, hc.y + hc.h * 0.5f};
        ui.pointerMove(p);
        ui.pointerDown(p);
        ui.pointerUp(p);
        ui.update(1.f / 60.f);
    }
    bool asc = true;
    UI_CHECK_EQ(w::headerSortSection(ui, header, &asc), 1);
    UI_CHECK(!asc);
    ui.pointerMove({640.f, 400.f});
    const NodeHandle pop = w::popover(ui, anchor, "pop");
    w::checkBox(ui, pop, "Meshes", true);
    w::checkBox(ui, pop, "Textures", false);
    w::toast(ui, "Project saved", 60.f);
    w::toast(ui, "Build finished in 4.2 s", 60.f);
    settle(ui);
    UI_CHECK(ui.alive(pop));
    UI_CHECK_EQ(w::toastCount(ui), 2u);
    UI_CHECK(ui.checked(on) && !ui.checked(off));
    UI_CHECK(w::statusText(ui, sb) == "Saved scene.fuse");
    render(ui, img);
}

// ---- text ----------------------------------------------------------------------------------------------------------
void sceneText(Image& img) {
    UiContext ui({.viewport = {660, 400}, .font = &font()});
    installThemed(ui);
    ui.setCss(ui.root(), "padding", "12px");
    ui.setCss(ui.root(), "gap", "10px");
    const NodeHandle row = w::panel(ui, ui.root(), "row");
    ui.addClass(row, "row");
    const NodeHandle ed = w::textEdit(ui, row,
                                      "A word-wrapped text edit. Lines break at the widget width and the caret moves by visual "
                                      "line.\nA second paragraph after a line break.", "notes");
    ui.setCss(ed, "width", "300px");
    ui.setCss(ed, "height", "150px");
    const NodeHandle plain = w::plainTextEdit(ui, row, "plain text edit\n  no gutter, no colouring\nlines are not wrapped, however long they get", "plain");
    ui.setCss(plain, "flex-grow", "1");
    ui.setCss(plain, "height", "150px");
    const NodeHandle box = w::panel(ui, ui.root(), "box");
    ui.setCss(box, "width", "420px");
    const NodeHandle rt = w::richText(ui, box,
        "<b>Bold</b>, <i>italic</i>, <color=#ff8800>orange</color>, <u>underlined</u> and <s>struck</s> text with a "
        "<a href=\"docs/ui\">link to the docs</a> that wraps over several lines.<br><size=22>Big</size> and "
        "<size=10>small</size> sizes, &lt;escaped&gt; &amp; entities, <b><color=#4cc26a>nested <u>tags</u></color></b>.", "rich");
    ui.update();
    ui.setFocus(ed);
    w::textEditSetSelection(ui, ed, 26, 57); // "Lines break at the widget width": the selection spans a soft wrap
    settle(ui);
    UI_CHECK(ui.focused() == ed);
    UI_CHECK_EQ(w::textEditCaret(ui, ed), 57u);
    UI_CHECK(w::textEditVisualLines(ui, ed) >= 3);
    UI_CHECK_EQ(w::richTextLinkCount(ui, rt), 1u);
    render(ui, img);
}

// ---- dialogs -------------------------------------------------------------------------------------------------------
struct DialogCell {
    int cw, chh;
    UiContext ui;
    DialogCell(int cellW, int cellH) : cw(cellW), chh(cellH), ui({.viewport = {f32(cellW), f32(cellH)}, .font = &font()}) {
        installThemed(ui);
        ui.setCss(ui.root(), "padding", "10px");
        w::button(ui, ui.root(), "Behind the scrim"); // modal dialogs dim what is behind them
        ui.update();
    }
    void into(Image& sheet, int col, int rowIdx) {
        if (gTheme == w::ThemeVariant::Dark) ui.setFocus({}); // see the header: the dark goldens predate focus rings
        settle(ui);
        Image cell;
        render(ui, cell);
        blit(sheet, cell, col * cw, rowIdx * chh);
    }
};
void sceneDialogs(Image& img) {
    constexpr int kCellW = 400, kCellH = 230;
    img.resize(kCellW * 3, kCellH * 2, 0xFF000000u);
    struct Box { const char* title; const char* text; const char* buttons; w::MessageIcon icon; };
    static const Box kBoxes[] = {{"About", "FUSE UI, CPU reference rasteriser.", "OK", w::MessageIcon::None},
                                 {"Import finished", "42 assets were imported.", "OK", w::MessageIcon::Info},
                                 {"Delete asset", "This cannot be undone.", "Delete\nCancel", w::MessageIcon::Warning},
                                 {"Build failed", "shader.fx(12): syntax error.", "Retry\nClose", w::MessageIcon::Error},
                                 {"Unsaved changes", "Save the scene before closing?", "Save\nDiscard\nCancel", w::MessageIcon::Question}};
    int i = 0;
    for (const Box& b : kBoxes) {
        DialogCell c(kCellW, kCellH);
        const NodeHandle box = w::messageBox(c.ui, b.title, b.text, b.buttons, b.icon);
        c.ui.update();
        UI_CHECK(w::popupOpen(c.ui, c.ui.parent(box)));
        c.into(img, i % 3, i / 3);
        ++i;
    }
    // a long text wraps inside the message box
    {
        DialogCell c(kCellW, kCellH);
        w::messageBox(c.ui, "Update available",
                      "A newer version of the editor is available. It will be downloaded in the background and installed on the next start.",
                      "Later\nRestart now", w::MessageIcon::Info);
        c.into(img, 2, 1);
    }
}
void sceneWizard(Image& img) {
    constexpr int kCellW = 460, kCellH = 300;
    img.resize(kCellW * 2, kCellH, 0xFF000000u);
    for (int page = 0; page < 3; page += 2) { // first page: Back disabled; last page: Next reads Finish
        DialogCell c(kCellW, kCellH);
        const NodeHandle wiz = w::wizard(c.ui, "New Project", "wiz");
        const NodeHandle first = w::wizardAddPage(c.ui, wiz, "Template");
        w::label(c.ui, first, "Pick a template for the project.");
        w::radioButton(c.ui, first, "Empty", false);
        w::radioButton(c.ui, first, "First person", true);
        w::lineEdit(c.ui, w::wizardAddPage(c.ui, wiz, "Name"), "my_game");
        w::label(c.ui, w::wizardAddPage(c.ui, wiz, "Summary"), "Ready to create my_game.");
        w::openWizard(c.ui, wiz);
        c.ui.update();
        if (page) w::wizardSetPage(c.ui, wiz, u32(page));
        c.ui.update();
        UI_CHECK_EQ(w::wizardPage(c.ui, wiz), u32(page));
        UI_CHECK(c.ui.disabled(w::wizardButton(c.ui, wiz, w::WizardButton::Back)) == (page == 0));
        UI_CHECK(c.ui.text(w::wizardButton(c.ui, wiz, w::WizardButton::Next)) == (page ? "Finish" : "Next"));
        c.into(img, page / 2, 0);
    }
}

// ---- main window: docks, flame graph -------------------------------------------------------------------------------
void sceneMainWindow(Image& img) {
    UiContext ui({.viewport = {900, 560}, .font = &font()});
    installThemed(ui);
    const NodeHandle mw = w::mainWindow(ui, ui.root(), "mw");
    w::menuBarMenu(ui, w::mainWindowMenuBar(ui, mw), "File", "New\nOpen\n-\nQuit");
    w::menuBarMenu(ui, w::mainWindowMenuBar(ui, mw), "Edit", "Undo\nRedo");
    w::menuBarMenu(ui, w::mainWindowMenuBar(ui, mw), "View", "Outline\nProfiler");
    const NodeHandle tb = w::mainWindowToolBar(ui, mw);
    w::toolBarAction(ui, tb, "New");
    w::toolBarAction(ui, tb, "Open");
    w::toolBarSeparator(ui, tb);
    w::toolBarAction(ui, tb, "Run");
    const NodeHandle central = w::mainWindowCentral(ui, mw);
    ui.setCss(central, "padding", "10px");
    w::label(ui, central, "Central widget: the document goes here.");
    const NodeHandle area = w::mainWindowDockArea(ui, mw);
    const NodeHandle outline = w::dockWidget(ui, area, "Outline", w::DockSide::Left, "outline");
    const NodeHandle assets = w::dockWidget(ui, area, "Assets", w::DockSide::Left, "assets");
    const NodeHandle history = w::dockWidget(ui, area, "History", w::DockSide::Left, "history");
    const NodeHandle prof = w::dockWidget(ui, area, "Profiler", w::DockSide::Bottom, "profiler");
    const NodeHandle props = w::dockWidget(ui, area, "Properties", w::DockSide::Right, "props");
    for (NodeHandle d : {outline, assets, history, props}) ui.setCss(w::dockBody(ui, d), "padding", "8px");
    w::label(ui, w::dockBody(ui, outline), "outline");
    w::label(ui, w::dockBody(ui, assets), "assets: textures, meshes");
    w::label(ui, w::dockBody(ui, history), "history");
    w::label(ui, w::dockBody(ui, props), "A floating dock.");
    const NodeHandle flame = w::chart(ui, w::dockBody(ui, prof), w::ChartKind::Flame, "flame");
    ui.setCss(flame, "flex-grow", "1");
    ui.setCss(w::dockBody(ui, prof), "padding", "4px");
    const w::FlameSpan spans[] = {{"frame", 0.f, 100.f, 0},
                                  {"update", 0.f, 60.f, 1},
                                  {"render", 60.f, 40.f, 1, Color::rgb(0x3D8BFD)},
                                  {"physics", 10.f, 30.f, 2},
                                  {"ai_pathfinding_with_a_very_long_span_name", 45.f, 12.f, 2},
                                  {"shadows", 62.f, 14.f, 2},
                                  {"opaque", 76.f, 20.f, 2},
                                  {"broadphase", 12.f, 10.f, 3}};
    w::chartSetFlame(ui, flame, spans);
    w::dockTabify(ui, assets, outline);
    w::dockTabify(ui, history, outline);
    w::dockRaise(ui, assets);
    w::dockFloat(ui, props, {560, 110, 250, 150});
    const NodeHandle sb = w::mainWindowStatusBar(ui, mw);
    w::statusMessage(ui, sb, "Ready");
    w::statusBarPermanent(ui, sb, "8 spans");
    ui.update();
    ui.pointerMove(ctr(w::chartFlameSpanRect(ui, flame, 3))); // hover "physics": highlighted, tooltip-free
    settle(ui);
    UI_CHECK_EQ(w::chartFlameSpanCount(ui, flame), 8u);
    UI_CHECK(w::dockFloating(ui, props));
    render(ui, img);
}

// ---- HUD: minimap, ammo counter ------------------------------------------------------------------------------------
#if FUSE_UI_GOLDEN_HAVE_GAME
namespace g = fuse::ui::game;
void place(UiContext& ui, NodeHandle n, f32 x, f32 y, f32 wpx, f32 hpx) {
    ui.set(n, Prop::Left, PropValue::ofLength(Length::px(x)));
    ui.set(n, Prop::Top, PropValue::ofLength(Length::px(y)));
    ui.set(n, Prop::Width, PropValue::ofLength(Length::px(wpx)));
    ui.set(n, Prop::Height, PropValue::ofLength(Length::px(hpx)));
}
void sceneHud(Image& img) {
    UiContext ui({.viewport = {640, 300}, .font = &font()});
    installThemed(ui);
    g::install(ui);
    ui.setCss(ui.root(), "layout", "anchor");
    ui.setCss(ui.root(), "background", "#1B2230");
    const g::MinimapBlip blips[] = {{{10.f, 40.f}, Color::rgb(0xE5484D), g::MinimapBlipKind::Triangle},
                                    {{-35.f, 12.f}, Color::rgb(0x5EA2FF), g::MinimapBlipKind::Dot},
                                    {{30.f, -20.f}, Color::rgb(0x4CC26A), g::MinimapBlipKind::Square},
                                    {{20.f, 20.f}, Color::rgb(0xF2C14E), g::MinimapBlipKind::Diamond},
                                    {{400.f, 100.f}, Color::rgb(0xF2C14E), g::MinimapBlipKind::Diamond}, // clamped
                                    {{-300.f, -300.f}, Color::rgb(0xE5484D), g::MinimapBlipKind::Dot}};  // clamped
    // circle: rotating map (heading 30 degrees), map image behind the blips
    const NodeHandle round = g::minimap(ui, ui.root(), "round");
    place(ui, round, 20, 20, 200, 200);
    g::setMinimapRange(ui, round, 80.f);
    g::setMinimapHeading(ui, round, 30.f);
    g::setMinimapImage(ui, round, 0);
    g::minimapSetBlips(ui, round, blips);
    // square: north-up, the player arrow turns instead
    const NodeHandle square = g::minimap(ui, ui.root(), "square");
    place(ui, square, 240, 20, 160, 160);
    g::setMinimapShape(ui, square, g::MinimapShape::Square);
    g::setMinimapRange(ui, square, 80.f);
    g::setMinimapHeading(ui, square, 120.f, false);
    g::minimapSetBlips(ui, square, blips);

    const NodeHandle pips = g::ammoCounter(ui, ui.root(), "pips"); // one pip per round
    place(ui, pips, 420, 20, 200, 60);
    g::setAmmo(ui, pips, 22, 30, 90);
    const NodeHandle low = g::ammoCounter(ui, ui.root(), "low"); // low ammo: danger colour
    place(ui, low, 420, 90, 200, 60);
    g::setAmmo(ui, low, 5, 30, 12);
    const NodeHandle reload = g::ammoCounter(ui, ui.root(), "reload"); // empty magazine, reload bar at 60 %
    place(ui, reload, 420, 160, 200, 60);
    g::setAmmo(ui, reload, 0, 12, 48);
    g::setAmmoReload(ui, reload, 0.6f);
    const NodeHandle bar = g::ammoCounter(ui, ui.root(), "bar"); // magazine over kAmmoMaxPips: a bar
    place(ui, bar, 420, 230, 200, 60);
    g::setAmmo(ui, bar, 150, 200, 400);
    settle(ui);
    bool clamped = false;
    g::minimapBlipPos(ui, round, 4, &clamped);
    UI_CHECK(clamped);
    g::minimapBlipPos(ui, square, 5, &clamped);
    UI_CHECK(clamped);
    UI_CHECK(g::ammoInfo(ui, low).low && g::ammoInfo(ui, reload).reloading && g::ammoInfo(ui, bar).pips == 0u);

    img.resize(int(ui.viewport().w), int(ui.viewport().h), 0xFF000000u);
    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    Image map; // image index 0: a stand-in map texture, a 4 x 4 checker so the clip to the round frame shows
    map.resize(4, 4, 0xFF2A3A2Au);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            if ((x + y) & 1) map.pixels[std::size_t(y * 4 + x)] = 0xFF36503Au;
    r.addImage(&map);
    r.render(ui.drawList(), img);
}
#endif

// ---- double spin box + context menu ---------------------------------------------------------------------------------
void sceneSpinMenu(Image& img) {
    UiContext ui({.viewport = {520, 260}, .font = &font()});
    installThemed(ui);
    ui.setCss(ui.root(), "padding", "12px");
    ui.setCss(ui.root(), "gap", "10px");
    const NodeHandle row = w::panel(ui, ui.root(), "row");
    ui.addClass(row, "row");
    w::label(ui, row, "Gain");
    const NodeHandle gain = w::doubleSpinBox(ui, row, 0.f, 1.f, 0.5f, 0.1f, "gain");
    w::label(ui, row, "Fine");
    const NodeHandle fine = w::doubleSpinBox(ui, row, 0.f, 1.f, 0.25f, 0.05f, "fine");
    w::label(ui, row, "Offset");
    const NodeHandle off = w::doubleSpinBox(ui, row, -10.f, 10.f, -2.5f, 0.5f, "offset");
    const NodeHandle row2 = w::panel(ui, ui.root(), "row2");
    ui.addClass(row2, "row");
    w::label(ui, row2, "Disabled");
    const NodeHandle dis = w::doubleSpinBox(ui, row2, 0.f, 100.f, 42.5f, 0.5f, "disabled");
    ui.setDisabled(dis, true);
    w::label(ui, row2, "Whole");
    w::spinBox(ui, row2, 0.f, 240.f, 144.f, 1.f, "fps");
    const NodeHandle asset = w::button(ui, ui.root(), "Asset (right-click)", "asset");
    w::setContextMenu(ui, asset, "Rename\nDuplicate\nReveal in the asset browser\n-\nDelete");
    ui.update();
    // step the boxes with the keyboard: the text follows the value with the step's precision
    ui.setFocus(gain, FocusReason::Keyboard);
    ui.keyDown(Key::Up);
    ui.keyDown(Key::Up);
    ui.setFocus(fine, FocusReason::Keyboard);
    ui.keyDown(Key::Up);
    ui.setFocus(off, FocusReason::Keyboard);
    ui.update();
    UI_CHECK(std::abs(ui.value(gain) - 0.7f) < 1e-3f && std::abs(ui.value(fine) - 0.3f) < 1e-3f);
    // the context menu, opened by a right press, with the pointer on its second item
    const Vec2 at = ctr(ui.rect(asset));
    const u32 before = ui.nodeCount();
    ui.pointerMove(at);
    ui.pointerDown(at, MouseButton::Right);
    ui.pointerUp(at, MouseButton::Right);
    ui.update();
    UI_CHECK(ui.nodeCount() > before);
    const u32 itemCls = ui.registry().find("menuitem");
    u32 items = 0;
    NodeHandle second;
    for (u32 i = 0; i < 4096; ++i) {
        const NodeHandle h = ui.handleOf(i);
        if (!ui.alive(h) || ui.classOf(h) != itemCls) continue;
        if (++items == 2) second = h;
    }
    UI_CHECK(items >= 4 && second.valid());
    if (second.valid()) ui.pointerMove(ctr(ui.rect(second)));
    settle(ui);
    render(ui, img);
}

// ---- controls: Tier A in its states, one keyboard-focused ----------------------------------------------------------
void sceneControls(Image& img) {
    UiContext ui({.viewport = {660, 430}, .font = &font()});
    installThemed(ui);
    ui.setCss(ui.root(), "padding", "12px");
    ui.setCss(ui.root(), "gap", "10px");
    const auto row = [&] {
        const NodeHandle r = w::panel(ui, ui.root());
        ui.addClass(r, "row");
        return r;
    };
    NodeHandle r = row();
    w::button(ui, r, "Button");
    ui.addClass(w::button(ui, r, "Primary"), "primary");
    ui.setDisabled(w::button(ui, r, "Disabled"), true);
    w::toolButton(ui, r, "Tool");
    w::link(ui, r, "A link");
    ui.addClass(w::label(ui, r, "dim label"), "dim");
    r = row();
    w::checkBox(ui, r, "Checked", true);
    const NodeHandle focused = w::checkBox(ui, r, "Focused", false, "focused");
    ui.setDisabled(w::checkBox(ui, r, "Disabled", true), true);
    w::radioButton(ui, r, "Radio on", true);
    w::radioButton(ui, r, "Radio off", false);
    w::toggleSwitch(ui, r, "On", true);
    w::toggleSwitch(ui, r, "Off", false);
    r = row();
    ui.setCss(w::slider(ui, r, 0.f, 100.f, 35.f), "width", "160px");
    ui.setCss(w::progressBar(ui, r, 0.6f), "width", "140px");
    w::dial(ui, r, 0.f, 1.f, 0.7f);
    const NodeHandle sb = w::scrollBar(ui, r, false);
    ui.setCss(sb, "width", "150px");
    r = row();
    ui.setCss(w::lineEdit(ui, r, "line edit"), "width", "150px");
    w::spinBox(ui, r, 0.f, 10.f, 4.f);
    w::comboBox(ui, r, "Low\nMedium\nHigh", 1);
    ui.setDisabled(w::lineEdit(ui, r, "disabled"), true);
    w::tabBar(ui, ui.root(), "Scene\nAssets\nConsole", 1);
    r = row();
    ui.setCss(r, "align-items", "flex-start");
    ui.setCss(w::listView(ui, r, "Alpha\nBeta\nGamma\nDelta", 1), "width", "160px");
    ui.setCss(w::treeView(ui, r, "Root\n Child one\n Child two\nOther"), "width", "180px");
    const NodeHandle card = w::panel(ui, r);
    ui.addClass(card, "card");
    ui.addClass(w::label(ui, card, "Card title"), "title");
    w::label(ui, card, "Body text on a card.");
    ui.update();
    ui.setFocus(focused, FocusReason::Keyboard);
    ui.pointerMove({650.f, 425.f});
    settle(ui);
    UI_CHECK((ui.states(focused) & kStateFocusVisible) != 0);
    render(ui, img);
}

// ---- editors: the widgets whose paint code reads theme tokens ------------------------------------------------------
void sceneEditors(Image& img) {
    UiContext ui({.viewport = {820, 560}, .font = &font()});
    installThemed(ui);
    ui.setCss(ui.root(), "padding", "10px");
    ui.setCss(ui.root(), "gap", "8px");
    const auto row = [&] {
        const NodeHandle r = w::panel(ui, ui.root());
        ui.addClass(r, "row");
        ui.setCss(r, "align-items", "flex-start");
        return r;
    };
    NodeHandle r = row();
    const NodeHandle code = w::codeEdit(ui, r, "-- spawn the player\nlocal hp = 100\nfunction hit(n)\n  hp = hp - n\n  return \"ouch\"\nend\n");
    ui.setCss(code, "width", "300px");
    ui.setCss(code, "height", "150px");
    const NodeHandle graph = w::nodeGraph(ui, r, "graph");
    ui.setCss(graph, "width", "470px");
    ui.setCss(graph, "height", "150px");
    const w::GraphPin in1[] = {{"A", 1}, {"B", 2}};
    const w::GraphPin out1[] = {{"Out", 1}};
    const u32 a = w::nodeGraphAddNode(ui, graph, "Noise", {20.f, 20.f}, {}, out1);
    const u32 b = w::nodeGraphAddNode(ui, graph, "Multiply", {220.f, 40.f}, in1, out1);
    r = row();
    const NodeHandle tl = w::timeline(ui, r, 10.f, "tl");
    ui.setCss(tl, "width", "470px");
    ui.setCss(tl, "height", "120px");
    const u32 t0 = w::timelineAddTrack(ui, tl, "Camera");
    const u32 t1 = w::timelineAddTrack(ui, tl, "Audio");
    w::timelineAddClip(ui, tl, t0, 0.5f, 3.f, "Pan", Color::rgb(0x6BCB77));
    w::timelineAddClip(ui, tl, t1, 2.f, 6.f, "Theme", Color::rgb(0x4D96FF));
    const NodeHandle curve = w::curveEditor(ui, r, 0.f, 1.f, 0.f, 1.f, "curve");
    ui.setCss(curve, "width", "300px");
    ui.setCss(curve, "height", "120px");
    w::curveAddKey(ui, curve, 0.f, 0.1f);
    w::curveAddKey(ui, curve, 0.5f, 0.9f);
    w::curveAddKey(ui, curve, 1.f, 0.3f);
    r = row();
    w::calendar(ui, r, 2026, 11, 13);
    const NodeHandle col = w::panel(ui, r);
    ui.setCss(col, "gap", "8px");
    w::dateTimeEdit(ui, col, 2026, 11, 13, 9, 30);
    ui.setCss(w::markdownView(ui, col, "## Markdown\nText with `code`.\n\n> a quote\n\n```\ncode block\n```"), "width", "300px");
    ui.update();
    UI_CHECK(w::nodeGraphConnect(ui, graph, a, 0, b, 0));
    ui.pointerMove({815.f, 555.f});
    settle(ui);
    render(ui, img);
}

struct Scene { const char* name; void (*fn)(Image&); };
const Scene kScenes[] = {{"containers", &sceneContainers}, {"text", &sceneText}, {"dialogs", &sceneDialogs},
                         {"wizard", &sceneWizard},
                         {"mainwindow", &sceneMainWindow},
                         {"spinmenu", &sceneSpinMenu},
                         {"controls", &sceneControls},
                         {"editors", &sceneEditors},
#if FUSE_UI_GOLDEN_HAVE_GAME
                         {"hud", &sceneHud},
#endif
};

void compare(const Scene& s, const char* outDir) {
    const char* theme = gTheme == w::ThemeVariant::Light ? "_light" : gTheme == w::ThemeVariant::HighContrast ? "_contrast" : "";
#if FUSE_UI_TEST_TEXT_FULL
    const std::string golden = std::string("golden/widgets_") + s.name + theme + "_full.png"; // FreeType coverage differs from stb_truetype
#else
    const std::string golden = std::string("golden/widgets_") + s.name + theme + ".png";
#endif
    Image img;
    s.fn(img);
    if (outDir) stbi_write_png((std::string(outDir) + "/widgets_" + s.name + theme + ".png").c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
    if (std::getenv("FUSE_UI_UPDATE_GOLDEN")) {
        stbi_write_png(golden.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("golden written: %s\n", golden.c_str());
        return;
    }
    int gw = 0, gh = 0, n = 0;
    unsigned char* ref = stbi_load(golden.c_str(), &gw, &gh, &n, 4);
    UI_CHECK(ref != nullptr);
    if (!ref) {
        std::fprintf(stderr, "missing golden %s (run from the tests directory; FUSE_UI_UPDATE_GOLDEN=1 writes it)\n", golden.c_str());
        return;
    }
    UI_CHECK_EQ(gw, img.width);
    UI_CHECK_EQ(gh, img.height);
    if (gw == img.width && gh == img.height) {
        int bad = 0, maxd = 0;
        for (int i = 0; i < gw * gh; ++i)
            for (int c = 0; c < 4; ++c) {
                const int d = std::abs(int(ref[i * 4 + c]) - int(ch(img.pixels[std::size_t(i)], c)));
                maxd = std::max(maxd, d);
                if (d > 3) { ++bad; break; }
            }
        std::printf("%s golden diff: %d pixels > 3, max channel diff %d\n", s.name, bad, maxd);
        UI_CHECK(bad <= gw * gh / 1000); // ≤ 0.1% of pixels may differ (compiler / libm rounding)
    }
    stbi_image_free(ref);
}
} // namespace

int main(int argc, char** argv) {
    const char* only = nullptr;
    const char* outDir = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--scene=", 8) == 0) only = argv[i] + 8;
        else if (std::strncmp(argv[i], "--out=", 6) == 0) outDir = argv[i] + 6;
        else if (std::strcmp(argv[i], "--theme=light") == 0) gTheme = w::ThemeVariant::Light;
        else if (std::strcmp(argv[i], "--theme=contrast") == 0) gTheme = w::ThemeVariant::HighContrast;
    }
    bool ran = false;
    for (const Scene& s : kScenes)
        if (!only || std::strcmp(only, s.name) == 0) {
            compare(s, outDir);
            ran = true;
        }
    if (!ran) {
        std::fprintf(stderr, "unknown scene %s\n", only);
        return 2;
    }
    return finish("fuse_ui_widget_golden");
}
