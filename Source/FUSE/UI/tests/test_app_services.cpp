// fuse_app platform-service gates (plan §4.13), headless, one executable per gate (FUSE_APP_GATE):
//   cursor_resolution    `cursor:` style + WidgetClass::cursor hooks -> UiContext::cursor() (text fields, links,
//                        disabled widgets, splitter handles, header / table column edges, drags, override)
//   menu_description     MenuDesc -> in-window widgets::menuBar; clicks, shortcuts, checkable / disabled items
//   dpi_scale            scale detection maths (Xft.dpi, env overrides, NativeOptions::scale) + pixel mapping
//   theme_restyle        system theme change -> WindowEvent + token override sheet restyles the window
//   window_state         window management API on the headless backend, events, close guard, backend forwarding
//   file_dialog_fallback toolkit file dialog fallback + zenity / kdialog command lines
#include "fuse/app/app.hpp"
#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace fuse::ui;
namespace app = fuse::app;
namespace w = fuse::ui::widgets;

namespace {
struct VClock { double t = 0.0; };
double vnow(void* u) { return static_cast<VClock*>(u)->t; }
bool vpump(void* u, app::Application&, f32 wait) {
    static_cast<VClock*>(u)->t += std::isinf(wait) ? 0.25 : double(wait);
    return true;
}
struct Rig {
    VClock clock;
    app::Application a{{.name = "gate_services", .loadSettings = false}};
    Rig() {
        a.setClock({&clock, &vnow});
        a.setEventSource({&clock, &vpump});
    }
    void settle(int n = 3) { for (int i = 0; i < n; ++i) a.step(0.016f); }
};
Vec2 centre(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }

void cursorResolution() {
    Rig rig;
    app::AppWindow& win = rig.a.createWindow({.title = "cursors", .width = 640, .height = 480});
    UiContext& ui = win.ui();
    const NodeHandle root = ui.root();
    const NodeHandle lbl = w::label(ui, root, "plain", "lbl");
    const NodeHandle edit = w::lineEdit(ui, root, "text", "edit");
    const NodeHandle lnk = w::link(ui, root, "a link", "lnk");
    const NodeHandle btn = w::button(ui, root, "enabled", "btn");
    const NodeHandle off = w::button(ui, root, "disabled", "off");
    ui.setDisabled(off, true);
    const NodeHandle custom = w::panel(ui, root, "custom");
    UI_CHECK(ui.setCss(custom, "cursor", "crosshair"));
    ui.setCss(custom, "padding", "6px");
    const NodeHandle inner = w::label(ui, custom, "inherits the panel's cursor", "inner");
    const NodeHandle split = w::splitter(ui, root, false, "split");
    ui.setCss(split, "height", "60px");
    const NodeHandle paneA = w::splitterPane(ui, split, 200.f);
    w::splitterPane(ui, split);
    const NodeHandle vsplit = w::splitter(ui, root, true, "vsplit");
    ui.setCss(vsplit, "height", "80px");
    w::splitterPane(ui, vsplit, 30.f);
    w::splitterPane(ui, vsplit);
    const NodeHandle head = w::headerView(ui, root, "Name\nSize\nDate", 100.f, "head");
    w::StringTableModel model("A\tB\tC");
    model.addRow("1\t2\t3");
    const NodeHandle table = w::tableView(ui, root, &model.model(), "table");
    ui.setCss(table, "height", "100px");
    rig.settle();

    auto at = [&](Vec2 p) { ui.pointerMove(p); return ui.cursor(); };
    UI_CHECK(at({-5.f, -5.f}) == Cursor::Arrow);                    // nothing hovered
    UI_CHECK(at(centre(ui.rect(lbl))) == Cursor::Arrow);
    UI_CHECK(at(centre(ui.rect(edit))) == Cursor::IBeam);           // theme: lineedit { cursor: text }
    UI_CHECK(at(centre(ui.rect(lnk))) == Cursor::PointingHand);     // theme: link { cursor: pointer }
    UI_CHECK(at(centre(ui.rect(btn))) == Cursor::Arrow);
    UI_CHECK(at(centre(ui.rect(off))) == Cursor::NotAllowed);       // theme: *:disabled
    UI_CHECK(at({ui.rect(custom).x + 2.f, ui.rect(custom).y + 2.f}) == Cursor::Crosshair); // inline style
    UI_CHECK(at(centre(ui.rect(inner))) == Cursor::Crosshair);      // child with cursor: auto -> the ancestor's
    UI_CHECK(ui.computed(inner).cursor == Cursor::Auto);            // ... resolved by walking up, not by inheritance
    // a disabled text field: the state rule beats the type rule
    ui.setDisabled(edit, true);
    rig.settle();
    UI_CHECK(at(centre(ui.rect(edit))) == Cursor::NotAllowed);
    ui.setDisabled(edit, false);
    rig.settle();
    UI_CHECK(at(centre(ui.rect(edit))) == Cursor::IBeam);

    // splitter handle (class hook): direction follows the orientation; panes keep the arrow
    const NodeHandle handle = ui.nextSibling(paneA);
    UI_CHECK(ui.registry().get(ui.classOf(handle)).name == "splithandle");
    UI_CHECK(at(centre(ui.rect(handle))) == Cursor::ResizeH);
    UI_CHECK(at(centre(ui.rect(paneA))) == Cursor::Arrow);
    const NodeHandle vhandle = ui.nextSibling(ui.firstChild(vsplit));
    UI_CHECK(at(centre(ui.rect(vhandle))) == Cursor::ResizeV);
    // a drag keeps the pressed widget's cursor even when the pointer leaves it
    const Vec2 hc = centre(ui.rect(handle));
    ui.pointerMove(hc);
    ui.pointerDown(hc);
    ui.pointerMove(centre(ui.rect(lbl)));
    UI_CHECK(ui.cursor() == Cursor::ResizeH);
    ui.pointerUp(centre(ui.rect(lbl)));
    rig.settle();
    UI_CHECK(at(centre(ui.rect(lbl))) == Cursor::Arrow);

    // header: section edges resize, section bodies do not
    const Rect hr = ui.contentRect(head);
    UI_CHECK(at({hr.x + 50.f, hr.y + hr.h * 0.5f}) == Cursor::Arrow);
    UI_CHECK(at({hr.x + 100.f, hr.y + hr.h * 0.5f}) == Cursor::ResizeH);
    UI_CHECK(at({hr.x + 198.f, hr.y + hr.h * 0.5f}) == Cursor::ResizeH);
    // table: column edges of the header row only
    const Rect tr = ui.contentRect(table);
    const f32 edge = tr.x + w::tableColumnWidth(ui, table, 0);
    UI_CHECK(at({edge, tr.y + 6.f}) == Cursor::ResizeH);
    UI_CHECK(at({edge - 20.f, tr.y + 6.f}) == Cursor::Arrow);
    UI_CHECK(at({edge, tr.y + tr.h - 4.f}) == Cursor::Arrow);

    // override (busy cursor) beats everything; Auto releases it
    ui.setCursorOverride(Cursor::Wait);
    UI_CHECK(at(centre(ui.rect(edit))) == Cursor::Wait);
    ui.setCursorOverride(Cursor::Auto);
    UI_CHECK(ui.cursor() == Cursor::IBeam);

    // the property registry: every keyword parses and round-trips
    for (int c = 0; c < kCursorCount; ++c) {
        PropValue v;
        UI_CHECK(parsePropValue(Prop::Cursor, cursorCssName(Cursor(c)), v));
        UI_CHECK(v.e == u8(c));
    }
    PropValue bad;
    UI_CHECK(!parsePropValue(Prop::Cursor, "banana", bad));
    UI_CHECK(parsePropValue(Prop::Cursor, "col-resize", bad) && Cursor(bad.e) == Cursor::ResizeH);
    // style sheet rules, including a state-dependent one
    UI_CHECK(ui.addStyleSheet("#lbl { cursor: move; } #btn:pressed { cursor: wait; }").has_value());
    rig.settle();
    UI_CHECK(at(centre(ui.rect(lbl))) == Cursor::Move);
    ui.pointerMove(centre(ui.rect(btn)));
    ui.pointerDown(centre(ui.rect(btn)));
    rig.settle();
    UI_CHECK(ui.cursor() == Cursor::Wait);
    ui.pointerUp(centre(ui.rect(btn)));
    rig.settle();
    UI_CHECK(ui.cursor() == Cursor::Arrow);
}

struct MenuLog {
    std::vector<u32> ids;
};
void onMenu(void* u, app::AppWindow&, u32 id) { static_cast<MenuLog*>(u)->ids.push_back(id); }
app::MenuDesc sampleMenu() {
    using app::MenuItem;
    app::MenuDesc d;
    MenuItem file{.title = "File"};
    file.items.push_back({.title = "New", .id = 1, .key = Key::N, .mods = kModCtrl});
    file.items.push_back({.title = "Open...", .id = 2, .key = Key::O, .mods = kModCtrl});
    MenuItem recent{.title = "Recent"};
    recent.items.push_back({.title = "a.txt", .id = 10});
    recent.items.push_back({.title = "b.txt", .id = 11});
    file.items.push_back(recent);
    file.items.push_back(MenuItem::sep());
    file.items.push_back({.title = "Save", .id = 3, .key = Key::S, .mods = kModCtrl, .enabled = false});
    file.items.push_back({.title = "Quit", .id = 4, .key = Key::Q, .mods = kModCtrl});
    MenuItem view{.title = "View"};
    view.items.push_back({.title = "Grid", .id = 20, .key = Key::G, .mods = u8(kModCtrl | kModShift), .checkable = true, .checked = true});
    view.items.push_back({.title = "Zoom In", .id = 21, .key = Key::F5});
    d.menus = {file, view};
    return d;
}
void menuDescription() {
    // text form: nesting, separators, shortcuts, check marks, line -> id
    std::vector<u32> ids;
    const app::MenuDesc d = sampleMenu();
    const std::string fileText = app::menuItemsText(d.menus[0], ids);
    UI_CHECK_EQ(fileText, std::string("New    Ctrl+N\nOpen...    Ctrl+O\nRecent\n\ta.txt\n\tb.txt\n-\nSave    Ctrl+S\nQuit    Ctrl+Q"));
    UI_CHECK(ids == (std::vector<u32>{1, 2, 0, 10, 11, 0, 3, 4}));
    UI_CHECK_EQ(app::menuItemsText(d.menus[1], ids), std::string("[x] Grid    Ctrl+Shift+G\nZoom In    F5"));
    UI_CHECK_EQ(app::shortcutText(Key::Delete, u8(kModAlt | kModSuper)), std::string("Alt+Super+Del"));
    UI_CHECK(app::shortcutText(Key::None, kModCtrl).empty());

    Rig rig;
    app::AppWindow& win = rig.a.createWindow({.title = "menu", .width = 640, .height = 480});
    UiContext& ui = win.ui();
    w::label(ui, ui.root(), "content", "content");
    MenuLog log;
    win.setMenu(d, {&log, &onMenu});
    rig.settle();
    UI_CHECK(!win.nativeMenu());
    const NodeHandle bar = win.menuBarNode();
    UI_CHECK(ui.alive(bar));
    UI_CHECK(ui.firstChild(ui.root()) == bar); // above the content
    UI_CHECK_EQ(ui.childCount(bar), 2u);
    UI_CHECK(ui.rect(bar).y < ui.rect(ui.find("content")).y);
    const NodeHandle fileBtn = ui.firstChild(bar), viewBtn = ui.nextSibling(fileBtn);
    UI_CHECK(ui.registry().get(ui.classOf(fileBtn)).name == "menubutton");
    // accessibility: a menu bar of expandable buttons named by their titles
    {
        std::vector<A11ySnapshotNode> snap;
        ui.snapshotA11y(snap);
        u32 bars = 0, buttons = 0;
        for (const A11ySnapshotNode& s : snap) {
            bars += s.node == bar && s.role == A11yRole::MenuBar;
            buttons += (s.node == fileBtn && s.name == "File") || (s.node == viewBtn && s.name == "View" && s.expandable && s.hasAction(A11yAction::Expand));
        }
        UI_CHECK(bars == 1u && buttons == 2u);
    }

    // open a menu from the bar and click the item whose label starts with `itemText` (submenus: hover path first)
    auto findItem = [&](std::string_view itemText) {
        NodeHandle hit;
        for (u32 i = 0; i < 4096 && !hit.valid(); ++i) {
            const NodeHandle n = ui.handleOf(i);
            if (ui.alive(n) && ui.registry().get(ui.classOf(n)).name == "menuitem" && ui.computed(n).visible) {
                const std::string_view t = ui.text(n);
                if (t.substr(0, t.find('\n')).substr(0, itemText.size()) == itemText) hit = n;
            }
        }
        return hit;
    };
    auto openAndPick = [&](NodeHandle button, std::string_view itemText) {
        win.click(centre(ui.rect(button)));
        rig.settle();
        const NodeHandle hit = findItem(itemText);
        UI_CHECK(hit.valid());
        if (!hit.valid()) return;
        win.click(centre(ui.rect(hit)));
        rig.settle();
    };
    openAndPick(fileBtn, "Open...");
    UI_CHECK(log.ids == (std::vector<u32>{2}));
    // a disabled item does not fire
    openAndPick(fileBtn, "Save");
    UI_CHECK(log.ids == (std::vector<u32>{2}));
    // checkable item toggles and the bar text follows
    UI_CHECK(win.menuItemChecked(20));
    openAndPick(viewBtn, "[x] Grid");
    UI_CHECK(log.ids == (std::vector<u32>{2, 20}));
    UI_CHECK(!win.menuItemChecked(20));
    UI_CHECK(std::string(ui.text(viewBtn)).find("[ ] Grid") != std::string::npos);
    // a submenu item: open File, open Recent, pick b.txt
    win.click(centre(ui.rect(fileBtn)));
    rig.settle();
    if (const NodeHandle recent = findItem("Recent"); recent.valid()) {
        win.click(centre(ui.rect(recent)));
        rig.settle();
        const NodeHandle b = findItem("b.txt");
        UI_CHECK(b.valid());
        if (b.valid()) win.click(centre(ui.rect(b)));
        rig.settle();
        UI_CHECK(log.ids == (std::vector<u32>{2, 20, 11}));
    } else {
        UI_CHECK(false);
    }
    log.ids.clear();

    // shortcuts: enabled items only, exact modifiers
    UI_CHECK(win.menuShortcut(Key::N, kModCtrl));
    UI_CHECK(!win.menuShortcut(Key::N, 0));
    UI_CHECK(!win.menuShortcut(Key::S, kModCtrl)); // disabled
    UI_CHECK(win.menuShortcut(Key::F5, 0));
    UI_CHECK(log.ids == (std::vector<u32>{1, 21}));
    UI_CHECK(win.setMenuItemEnabled(3, true));
    UI_CHECK(win.menuShortcut(Key::S, kModCtrl));
    UI_CHECK(log.ids.back() == 3u);
    UI_CHECK(!win.activateMenuItem(999));
    UI_CHECK(!win.setMenuItemEnabled(999, true));
    UI_CHECK(win.activateMenuItem(10)); // nested item by id
    UI_CHECK(log.ids.back() == 10u);

    // a backend that takes the menu (the macOS bar): the in-window bar goes away, activation still reaches the handler
    struct Native { int sets = 0; } native;
    win.setWindowBackend({&native, [](void* u, app::AppWindow&, app::WindowRequest r) {
                              if (r != app::WindowRequest::Menu) return false;
                              ++static_cast<Native*>(u)->sets;
                              return true;
                          }});
    rig.settle();
    UI_CHECK(win.nativeMenu());
    UI_CHECK(native.sets == 1);
    UI_CHECK(!ui.alive(bar));
    UI_CHECK(!win.menuBarNode().valid());
    UI_CHECK(win.activateMenuItem(4));
    UI_CHECK(log.ids.back() == 4u);
    win.setMenuItemChecked(20, true);
    UI_CHECK(native.sets == 2); // the native bar is rebuilt on a change
    win.setWindowBackend({});
    rig.settle();
    UI_CHECK(!win.nativeMenu() && ui.alive(win.menuBarNode())); // back in the window
}

bool near(f32 a, f32 b) { return std::abs(a - b) < 1e-4f; }
void dpiScale() {
    namespace dpi = app::dpi;
    UI_CHECK(near(dpi::scaleFromDpi(96.f), 1.f));
    UI_CHECK(near(dpi::scaleFromDpi(120.f), 1.25f));
    UI_CHECK(near(dpi::scaleFromDpi(192.f), 2.f));
    UI_CHECK(near(dpi::scaleFromDpi(24.f), 0.5f));    // clamped
    UI_CHECK(near(dpi::scaleFromDpi(9600.f), 8.f));   // clamped
    UI_CHECK(dpi::scaleFromDpi(0.f) == 0.f && dpi::scaleFromDpi(-3.f) == 0.f);
    const std::string rm = "Xcursor.size:\t24\nXft.antialias:\t1\nXft.dpi:\t144\nXft.hinting:\t1\n";
    UI_CHECK(near(dpi::xftDpi(rm), 144.f));
    UI_CHECK(dpi::xftDpi("Xft.antialias:\t1\n") == 0.f);
    UI_CHECK(dpi::xftDpi("Xft.dpi:\tabc\n") == 0.f);
    UI_CHECK(dpi::xftDpi("") == 0.f);
    UI_CHECK(near(dpi::xftDpi("Xft.dpi: 96.5"), 96.5f)); // no trailing newline, space separator
    // precedence: FUSE_UI_SCALE > Xft.dpi > GDK_SCALE > 1
    UI_CHECK(near(dpi::x11Scale(rm, "2", "3"), 3.f));
    UI_CHECK(near(dpi::x11Scale(rm, "2", ""), 1.5f));
    UI_CHECK(near(dpi::x11Scale("", "2", ""), 2.f));
    UI_CHECK(near(dpi::x11Scale("", "", ""), 1.f));
    UI_CHECK(near(dpi::x11Scale("", "junk", "-1"), 1.f));
    // NativeOptions::scale: 0 = auto, 1 = physical pixels, anything else forces it
    UI_CHECK(near(dpi::resolveScale(0.f, 1.5f), 1.5f));
    UI_CHECK(near(dpi::resolveScale(1.f, 1.5f), 1.f));
    UI_CHECK(near(dpi::resolveScale(2.f, 1.5f), 2.f));
    UI_CHECK(near(dpi::resolveScale(0.f, 0.f), 1.f));
    // pixel mapping, as the native backends feed AppWindow::resizePixels
    const Size px = dpi::toPixels({400.f, 300.f}, 1.5f);
    UI_CHECK(px.w == 600.f && px.h == 450.f);
    UI_CHECK(dpi::toPixels({0.2f, 0.2f}, 1.f).w == 1.f);
    const Vec2 lp = dpi::toLogical({300.f, 150.f}, 1.5f);
    UI_CHECK(near(lp.x, 200.f) && near(lp.y, 100.f));

    Rig rig;
    app::AppWindow& win = rig.a.createWindow({.title = "dpi", .width = 400, .height = 300});
    struct Log { int n = 0; f32 scale = 0.f; } log;
    win.setEventHandler({&log, [](void* u, app::AppWindow&, const app::WindowEvent& e) {
                             if (e.type != app::WindowEvent::Type::ScaleChanged) return;
                             ++static_cast<Log*>(u)->n;
                             static_cast<Log*>(u)->scale = e.scale;
                         }});
    win.resizePixels(u32(px.w), u32(px.h), 1.5f); // what a backend does on a 144 dpi display
    UI_CHECK(near(win.size().w, 400.f) && near(win.size().h, 300.f));
    UI_CHECK(win.pixelSize().w == 600.f && win.pixelSize().h == 450.f);
    UI_CHECK(log.n == 1 && near(log.scale, 1.5f));
    win.resizePixels(600, 450, 1.5f);
    UI_CHECK(log.n == 1); // unchanged scale: no event
    const NodeHandle b = w::button(win.ui(), win.ui().root(), "hit", "b");
    rig.settle();
    // a device-pixel pointer position maps onto the logical layout
    const Rect r = win.ui().rect(b);
    const Vec2 devicePos{(r.x + r.w * 0.5f) * 1.5f, (r.y + r.h * 0.5f) * 1.5f};
    UI_CHECK(win.ui().hitTest(dpi::toLogical(devicePos, win.scale())) == b);
    UI_CHECK(win.rasterize({}).width == 600);
}

void themeRestyle() {
    UI_CHECK(app::schemeFromGtkTheme("Adwaita:dark") == app::ColorScheme::Dark);
    UI_CHECK(app::schemeFromGtkTheme("Adwaita") == app::ColorScheme::Light);
    UI_CHECK(app::schemeFromGtkTheme("") == app::ColorScheme::Unknown);
    UI_CHECK(app::schemeFromGtkTheme("HighContrastInverse") == app::ColorScheme::Dark);
    UI_CHECK(app::highContrastFromGtkTheme("HighContrast") && app::highContrastFromGtkTheme("Breeze-High-Contrast"));
    UI_CHECK(!app::highContrastFromGtkTheme("Adwaita-dark") && !app::highContrastFromGtkTheme(""));
    UI_CHECK(app::schemeFromGsettings("'prefer-dark'\n") == app::ColorScheme::Dark);
    UI_CHECK(app::schemeFromGsettings("'default'\n") == app::ColorScheme::Light);
    UI_CHECK(app::schemeFromGsettings("No such schema") == app::ColorScheme::Unknown);

    Rig rig;
    app::AppWindow& win = rig.a.createWindow({.title = "theme", .width = 320, .height = 200});
    app::AppWindow& other = rig.a.createWindow({.title = "theme2", .width = 320, .height = 200});
    UiContext& ui = win.ui();
    const NodeHandle b = w::button(ui, ui.root(), "Button", "b");
    w::button(other.ui(), other.ui().root(), "Button", "b");
    rig.settle();
    const Color darkText = ui.computed(b).color, darkRoot = ui.computed(ui.root()).background;
    const u32 darkPixel = win.rasterize({}).pixels[5];
    struct Log { int n = 0; app::SystemTheme last; } log;
    win.setEventHandler({&log, [](void* u, app::AppWindow&, const app::WindowEvent& e) {
                             if (e.type != app::WindowEvent::Type::ThemeChanged) return;
                             ++static_cast<Log*>(u)->n;
                             static_cast<Log*>(u)->last = e.theme;
                         }});
    win.setFollowSystemTheme(true); // unknown so far: nothing changes
    rig.settle();
    UI_CHECK(ui.computed(b).color == darkText);

    const app::SystemTheme light{.scheme = app::ColorScheme::Light, .hasAccent = true, .accent = Color::rgb(0xCC3366)};
    rig.a.setSystemTheme(light);
    rig.settle();
    UI_CHECK(log.n == 1 && log.last == light);
    UI_CHECK(rig.a.systemTheme() == light);
    // the following window is restyled: light surface, dark text, the system accent
    const Color lightText = ui.computed(b).color, lightRoot = ui.computed(ui.root()).background;
    UI_CHECK(!(lightText == darkText) && !(lightRoot == darkRoot));
    UI_CHECK(lightRoot.r > 0.8f && lightText.r < 0.3f);
    UI_CHECK(win.rasterize({}).pixels[5] != darkPixel);
    ui.addClass(b, "primary");
    rig.settle();
    UI_CHECK(std::abs(ui.computed(b).background.r - 0.8f) < 0.01f && std::abs(ui.computed(b).background.g - 0.2f) < 0.01f);
    // the window that does not follow keeps its colours (its handler could react instead)
    UI_CHECK(other.ui().computed(other.ui().find("b")).color == darkText);
    rig.a.setSystemTheme(light);
    UI_CHECK(log.n == 1); // no change, no event
    // and back to dark (stock tokens, no accent)
    rig.a.setSystemTheme({.scheme = app::ColorScheme::Dark});
    rig.settle();
    UI_CHECK(log.n == 2);
    ui.removeClass(b, "primary");
    rig.settle();
    UI_CHECK(ui.computed(b).color == darkText);
    UI_CHECK(ui.computed(ui.root()).background == darkRoot);
    // a window created later adopts the current theme at once
    rig.a.setSystemTheme(light);
    app::AppWindow& late = rig.a.createWindow({.title = "late", .width = 100, .height = 100});
    late.setFollowSystemTheme(true);
    rig.settle();
    UI_CHECK(late.ui().computed(late.ui().root()).background.r > 0.8f);
}

struct EventLog {
    std::vector<app::WindowEvent::Type> types;
    app::WindowEvent last;
    bool allowClose = false;
    int guardCalls = 0;
};
void windowState() {
    using T = app::WindowEvent::Type;
    Rig rig;
    app::AppWindow& win = rig.a.createWindow({.title = "state", .width = 400, .height = 300});
    EventLog log;
    win.setEventHandler({&log, [](void* u, app::AppWindow&, const app::WindowEvent& e) {
                             static_cast<EventLog*>(u)->types.push_back(e.type);
                             static_cast<EventLog*>(u)->last = e;
                         }});
    UI_CHECK(win.mode() == app::WindowMode::Normal && win.visible() && !win.active());
    win.setTitle("renamed");
    UI_CHECK_EQ(win.title(), std::string("renamed"));
    // mode: maximise / minimise / restore / fullscreen
    win.maximize();
    UI_CHECK(win.mode() == app::WindowMode::Maximized);
    UI_CHECK(log.types.back() == T::ModeChanged && log.last.mode == app::WindowMode::Maximized);
    win.minimize();
    UI_CHECK(win.mode() == app::WindowMode::Minimized);
    win.restore();
    UI_CHECK(win.mode() == app::WindowMode::Maximized); // what it was before it was minimised
    win.restore();
    UI_CHECK(win.mode() == app::WindowMode::Normal);
    win.setFullscreen(true);
    UI_CHECK(win.fullscreen());
    win.setFullscreen(false);
    UI_CHECK(win.mode() == app::WindowMode::Normal);
    const std::size_t n = log.types.size();
    win.setMode(app::WindowMode::Normal);
    UI_CHECK(log.types.size() == n); // no change, no event
    // show / hide
    win.setVisible(false);
    UI_CHECK(!win.visible() && log.types.back() == T::Hidden);
    win.setVisible(true);
    UI_CHECK(win.visible() && log.types.back() == T::Shown);
    // position
    win.setPosition({120.f, 80.f});
    UI_CHECK(win.position().x == 120.f && win.position().y == 80.f);
    UI_CHECK(log.types.back() == T::Moved && log.last.position.x == 120.f);
    // focus
    win.activate();
    UI_CHECK(win.active() && log.types.back() == T::FocusGained);
    win.notifyActive(false); // what a backend reports when another window takes focus
    UI_CHECK(!win.active() && log.types.back() == T::FocusLost);
    // size limits clamp the headless window
    win.setMinimumSize({500.f, 350.f});
    UI_CHECK(win.size().w == 500.f && win.size().h == 350.f);
    win.setMaximumSize({520.f, 0.f}); // 0 = unlimited on that axis
    win.resize(900, 900);
    UI_CHECK(win.size().w == 900.f); // resize() is what the OS reports: not clamped
    const Size c = win.clampSize({900.f, 900.f});
    UI_CHECK(c.w == 520.f && c.h == 900.f);
    UI_CHECK(win.minimumSize().w == 500.f && win.maximumSize().w == 520.f);

    // close guard: veto, then allow
    win.setCloseGuard({&log, [](void* u, app::AppWindow&) {
                           ++static_cast<EventLog*>(u)->guardCalls;
                           return static_cast<EventLog*>(u)->allowClose;
                       }});
    UI_CHECK(!win.tryClose());
    UI_CHECK(!win.closing() && log.guardCalls == 1 && log.types.back() == T::CloseVetoed);
    rig.settle();
    UI_CHECK_EQ(rig.a.windowCount(), 1u);
    log.allowClose = true;
    UI_CHECK(win.tryClose());
    UI_CHECK(win.closing() && log.types.back() == T::CloseRequested);
    rig.a.step(0.f);
    UI_CHECK_EQ(rig.a.windowCount(), 0u);

    // with an OS backend the requests are forwarded and the state follows what the OS reports
    app::Application a2{{.name = "gate_services2", .loadSettings = false}};
    app::AppWindow& w2 = a2.createWindow({.title = "os", .width = 200, .height = 100});
    struct Os { std::vector<app::WindowRequest> seen; bool can = true; } os;
    w2.setWindowBackend({&os, [](void* u, app::AppWindow&, app::WindowRequest r) {
                             static_cast<Os*>(u)->seen.push_back(r);
                             return static_cast<Os*>(u)->can;
                         }});
    w2.setTitle("t2");
    w2.maximize();
    UI_CHECK(w2.requestedMode() == app::WindowMode::Maximized);
    UI_CHECK(w2.mode() == app::WindowMode::Normal); // not until the OS says so
    w2.notifyMode(app::WindowMode::Maximized);
    UI_CHECK(w2.mode() == app::WindowMode::Maximized);
    w2.setPosition({5.f, 6.f});
    UI_CHECK(w2.position().x == 0.f && w2.requestedPosition().x == 5.f);
    w2.setVisible(false);
    UI_CHECK(w2.visible());
    w2.activate();
    w2.setMinimumSize({300.f, 300.f});
    UI_CHECK(w2.size().w == 200.f); // the OS resizes (and reports), not the window itself
    using R = app::WindowRequest;
    UI_CHECK(os.seen == (std::vector<R>{R::Title, R::Mode, R::Position, R::Visible, R::Activate, R::SizeLimits}));
    os.can = false; // a fullscreen-only platform: nothing changes
    w2.setFullscreen(true);
    UI_CHECK(w2.mode() == app::WindowMode::Maximized);
}

struct DialogResult {
    int calls = 0;
    std::string path;
};
void fileDialogFallback() {
    app::FileDialogOptions o{.title = "Open image", .directory = "/tmp", .filter = "*.png;*.jpg", .filterName = "Images"};
    UI_CHECK(app::zenityArgs(o) == (std::vector<std::string>{"zenity", "--file-selection", "--title=Open image", "--filename=/tmp/",
                                                             "--file-filter=Images | *.png *.jpg"}));
    UI_CHECK(app::kdialogArgs(o) == (std::vector<std::string>{"kdialog", "--title", "Open image", "--getopenfilename", "/tmp/",
                                                              "Images (*.png *.jpg)"}));
    app::FileDialogOptions s{.title = "Save", .directory = "/home/u", .defaultName = "a.txt", .save = true};
    UI_CHECK(app::zenityArgs(s) == (std::vector<std::string>{"zenity", "--file-selection", "--title=Save", "--save", "--confirm-overwrite",
                                                             "--filename=/home/u/a.txt"}));
    UI_CHECK(app::kdialogArgs(s) == (std::vector<std::string>{"kdialog", "--title", "Save", "--getsavefilename", "/home/u/a.txt"}));
    app::FileDialogOptions dir{.title = "Folder", .filter = "*.x", .pickDirectory = true};
    UI_CHECK(app::zenityArgs(dir) == (std::vector<std::string>{"zenity", "--file-selection", "--title=Folder", "--directory"}));
    UI_CHECK(app::kdialogArgs(dir) == (std::vector<std::string>{"kdialog", "--title", "Folder", "--getexistingdirectory", "."}));

    // the toolkit fallback: a modal widgets::fileDialog in the window; the callback gets the accepted path
    Rig rig;
    app::AppWindow& win = rig.a.createWindow({.title = "dlg", .width = 800, .height = 600});
    UiContext& ui = win.ui();
    DialogResult res;
    const app::FileDialogCallback cb{&res, [](void* u, app::AppWindow&, std::string_view path) {
                                         ++static_cast<DialogResult*>(u)->calls;
                                         static_cast<DialogResult*>(u)->path = path;
                                     }};
    const NodeHandle dlg = app::openToolkitFileDialog(win, {.title = "Pick", .directory = FUSE_UI_TEST_DATA}, cb);
    rig.settle();
    UI_CHECK(ui.alive(dlg) && w::popupOpen(ui, ui.parent(dlg))); // the dialog card sits in a modal popup scope
    const u32 count = w::fileDialogEntryCount(ui, dlg);
    UI_CHECK(count > 0);
    std::string accepted;
    for (u32 i = 0; i < count && accepted.empty(); ++i) {
        const std::string e = w::fileDialogEntry(ui, dlg, i);
        if (!e.empty() && e.back() != '/') accepted = w::fileDialogActivate(ui, dlg, i);
    }
    rig.settle();
    UI_CHECK(!accepted.empty());
    UI_CHECK(res.calls == 1 && res.path == accepted);
    UI_CHECK(!w::popupOpen(ui, ui.parent(dlg)));
}
} // namespace

int main() {
    const std::string g = FUSE_APP_GATE;
    if (g == "cursor_resolution") cursorResolution();
    else if (g == "menu_description") menuDescription();
    else if (g == "dpi_scale") dpiScale();
    else if (g == "theme_restyle") themeRestyle();
    else if (g == "window_state") windowState();
    else if (g == "file_dialog_fallback") fileDialogFallback();
    else {
        std::fprintf(stderr, "unknown gate %s\n", g.c_str());
        return 2;
    }
    const int f = fuse::ui::test::failures();
    std::printf("fuse_app_%s: %s\n", g.c_str(), f ? "FAILED" : "OK");
    return f ? 1 : 0;
}
