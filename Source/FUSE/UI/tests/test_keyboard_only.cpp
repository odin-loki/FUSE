// Gate fuse_ui_keyboard_only: keyboard-only operation of every interactive Tier A–D widget. One form holds them all;
// it is driven only through UiContext::keyDown / keyUp / textInput (no pointer events). Checks: Tab reaches every
// focusable widget and Shift+Tab walks the same order back; each widget is operable (its value / state changes as
// its keyboard model says: buttons Enter / Space, radio arrows, ranges arrows / Home / End / Page keys, combo box
// Alt+Down + arrows + Enter / Escape, menu bar F10 / arrows / Escape, views, editors, pro widgets, dialogs trap focus
// and Escape cancels); the focused node paints differently from the unfocused one (a visible focus ring) in the
// dark and the light theme (CPU raster pixels of the node's rect).
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
using namespace fuse::ui::widgets;

namespace {

void press(UiContext& ui, Key k, u8 mods = 0) {
    ui.keyDown(k, mods);
    ui.keyUp(k, mods);
    ui.update(1.f / 60.f);
}
void typeText(UiContext& ui, std::string_view s) {
    ui.textInput(s);
    ui.update(1.f / 60.f);
}
bool isA(const UiContext& ui, NodeHandle n, std::string_view cls) { return ui.alive(n) && ui.classOf(n) == ui.registry().find(cls); }
std::string nameOf(const UiContext& ui, NodeHandle n) {
    if (!ui.alive(n)) return "(none)";
    std::string s = ui.registry().get(ui.classOf(n)).name;
    if (!ui.idOf(n).empty()) s += "#" + std::string(ui.idOf(n));
    return s;
}
/// Tab (Ctrl+Tab out of the code editor, which indents on Tab) until `n` has focus.
bool tabTo(UiContext& ui, NodeHandle n) {
    for (int i = 0; i < 400 && ui.focused() != n; ++i) press(ui, Key::Tab, isA(ui, ui.focused(), "codeedit") ? kModCtrl : 0);
    if (ui.focused() != n) std::fprintf(stderr, "tabTo: %s never focused\n", nameOf(ui, n).c_str());
    return ui.focused() == n;
}
bool inside(const UiContext& ui, NodeHandle n, NodeHandle scope) {
    for (; ui.alive(n); n = ui.parent(n))
        if (n == scope) return true;
    return false;
}

struct Counter {
    int n = 0;
    f32 value = 0.f;
    std::string text;
};
void count(UiContext& ui, NodeHandle n, SignalKind k, Counter& c) {
    ui.connect(n, k, [&c](UiContext&, NodeHandle, const SignalArgs& a) { ++c.n; c.value = a.value; c.text = std::string(a.text); });
}

struct Form {
    NodeHandle bar, file, edit, btn, tip, primary, tool, link, check, r1, r2, r3, sw, slider, sbar, spin, combo, line, dial,
        list, tree, tabs, split, handle, dock, dockTitle, table, grid, props, propGroup, propBool, propNum, propEnum, propText,
        picker, cal, chart, font, graph, curve, tl, vp, code, dt, video, openFile;
};

Form build(UiContext& ui, const ItemModel* model) {
    install(ui);
    (void)ui.addStyleSheet("#form > * { flex-shrink: 0; }"); // wrapped lines keep every widget at its size
    Form f;
    f.bar = menuBar(ui, ui.root(), "menubar");
    f.file = menuBarMenu(ui, f.bar, "File", "New\nOpen\n-\nQuit", "menu.file");
    f.edit = menuBarMenu(ui, f.bar, "Edit", "Undo\nRedo", "menu.edit");
    const NodeHandle form = panel(ui, ui.root(), "form");
    for (auto [p, v] : {std::pair{"layout", "flow"}, {"align-items", "start"}, {"gap", "10px"}, {"padding", "8px"}, {"flex-grow", "1"}})
        ui.setCss(form, p, v);
    f.btn = button(ui, form, "Button", "btn");
    f.tip = setTooltip(ui, f.btn, "A tooltip");
    f.primary = button(ui, form, "Primary", "primary");
    ui.addClass(f.primary, "primary");
    f.tool = toolButton(ui, form, "Tool", "tool");
    f.link = link(ui, form, "A link", "link");
    f.check = checkBox(ui, form, "Check", false, "check");
    const NodeHandle radios = panel(ui, form, "radios");
    ui.addClass(radios, "row");
    f.r1 = radioButton(ui, radios, "Low", true, "r1");
    f.r2 = radioButton(ui, radios, "Mid", false, "r2");
    f.r3 = radioButton(ui, radios, "High", false, "r3");
    f.sw = toggleSwitch(ui, form, "Switch", false, "switch");
    f.slider = slider(ui, form, 0.f, 100.f, 50.f, 1.f, "slider");
    f.sbar = scrollBar(ui, form, false, "sbar");
    ui.setCss(f.sbar, "width", "120px");
    f.spin = spinBox(ui, form, 0.f, 10.f, 5.f, 1.f, "spin");
    f.combo = comboBox(ui, form, "Alpha\nBeta\nGamma", 0, "combo");
    f.line = lineEdit(ui, form, "", "line");
    f.dial = dial(ui, form, 0.f, 100.f, 50.f, "dial");
    ui.setRange(f.dial, 0.f, 100.f, 1.f);
    std::string items;
    for (int i = 0; i < 20; ++i) items += (i ? "\nItem " : "Item ") + std::to_string(i);
    f.list = listView(ui, form, items, -1, "list");
    f.tree = treeView(ui, form, "Root\n\tChild A\n\tChild B\nOther", "tree");
    f.tabs = tabBar(ui, form, "One\nTwo\nThree", 0, "tabs");
    f.split = splitter(ui, form, false, "split");
    ui.setCss(f.split, "width", "200px");
    ui.setCss(f.split, "height", "60px");
    splitterPane(ui, f.split, 80.f, "split.a");
    splitterPane(ui, f.split, 0.f, "split.b");
    f.handle = ui.nextSibling(ui.firstChild(f.split));
    const NodeHandle area = dockArea(ui, form, "dockarea");
    ui.setCss(area, "width", "320px");
    ui.setCss(area, "height", "160px");
    ui.setCss(area, "flex-grow", "0");
    f.dock = dockWidget(ui, area, "Outline", DockSide::Left, "dock");
    f.dockTitle = ui.firstChild(f.dock);
    f.table = tableView(ui, form, model, "table");
    f.grid = gridView(ui, form, model, 72.f, 56.f, "grid");
    f.props = propertyGrid(ui, form, "props");
    propertyRow(ui, f.props, {.name = "Transform", .kind = PropKind::Group});
    f.propGroup = ui.firstChild(f.props);
    f.propBool = propertyRow(ui, f.props, {.name = "Visible", .kind = PropKind::Bool}, "false");
    f.propNum = propertyRow(ui, f.props, {.name = "Scale", .kind = PropKind::Number, .lo = 0.f, .hi = 10.f, .step = 1.f}, "2");
    f.propEnum = propertyRow(ui, f.props, {.name = "Mode", .kind = PropKind::Enum, .options = "Fast\nGood\nBest"}, "0");
    f.propText = propertyRow(ui, f.props, {.name = "Name", .kind = PropKind::Text}, "cube");
    f.picker = colorPicker(ui, form, Color::rgb(0x3080C0), "picker");
    f.cal = calendar(ui, form, 2026, 11, 5, "cal");
    f.chart = chart(ui, form, ChartKind::Line, "chart");
    const f32 pts[] = {1.f, 3.f, 2.f, 5.f, 4.f};
    chartSetSeries(ui, f.chart, 0, "a", pts, Color::rgb(0x6BCB77));
    f.font = fontPicker(ui, form, "Open Sans\nSerif\nMono", 14.f, "font");
    f.graph = nodeGraph(ui, form, "graph");
    ui.setCss(f.graph, "width", "300px");
    ui.setCss(f.graph, "height", "160px");
    const GraphPin out[] = {{"out", 0}}, in[] = {{"in", 0}};
    nodeGraphAddNode(ui, f.graph, "Source", {10.f, 10.f}, {}, out);
    nodeGraphAddNode(ui, f.graph, "Sink", {170.f, 40.f}, in, {});
    nodeGraphConnect(ui, f.graph, 0, 0, 1, 0);
    f.curve = curveEditor(ui, form, 0.f, 1.f, 0.f, 1.f, "curve");
    ui.setCss(f.curve, "width", "240px");
    ui.setCss(f.curve, "height", "120px");
    curveAddKey(ui, f.curve, 0.f, 0.f);
    curveAddKey(ui, f.curve, 0.5f, 0.5f);
    curveAddKey(ui, f.curve, 1.f, 1.f);
    f.tl = timeline(ui, form, 10.f, "timeline");
    ui.setCss(f.tl, "width", "360px");
    timelineAddClip(ui, f.tl, timelineAddTrack(ui, f.tl, "Track"), 1.f, 3.f, "clip", Color::rgb(0xFFD93D));
    f.vp = viewportWidget(ui, form, 0, 64, 64, "viewport");
    ui.setCss(f.vp, "width", "160px");
    ui.setCss(f.vp, "height", "100px");
    f.code = codeEdit(ui, form, "local a = 1", CodeLanguage::Lua, "code");
    ui.setCss(f.code, "width", "260px");
    ui.setCss(f.code, "height", "100px");
    f.dt = dateTimeEdit(ui, form, 2026, 11, 5, 10, 30, "dt");
    f.video = videoWidget(ui, form, &testPatternSource(), "video");
    ui.setCss(f.video, "width", "200px");
    f.openFile = button(ui, form, "Open file...", "openfile");
    ui.update(1.f / 60.f);
    return f;
}

// ---- Tab / Shift+Tab ------------------------------------------------------------------------------------------------
void tabOrder(UiContext& ui, const Form& f) {
    std::vector<NodeHandle> order;
    ui.setFocus({});
    ui.tabOrder(order);
    const NodeHandle want[] = {f.file, f.edit, f.btn, f.primary, f.tool, f.link, f.check, f.r1, f.r2, f.r3, f.sw, f.slider,
                               f.sbar, f.spin, f.combo, f.line, f.dial, f.list, f.tree, f.tabs, f.handle, f.dockTitle, f.table,
                               f.grid, f.propGroup, f.propBool, f.propNum, f.propEnum, f.propText, f.picker, f.cal, f.chart,
                               f.graph, f.curve, f.tl, f.vp, f.code, f.dt, f.video, f.openFile};
    for (NodeHandle n : want)
        if (std::find(order.begin(), order.end(), n) == order.end()) {
            std::fprintf(stderr, "not in the tab order: %s\n", nameOf(ui, n).c_str());
            UI_CHECK(false);
        }
    // forward: Tab from nothing visits the whole order (Ctrl+Tab leaves the code editor) and wraps
    std::vector<NodeHandle> seen;
    for (std::size_t i = 0; i < order.size(); ++i) {
        press(ui, Key::Tab, isA(ui, ui.focused(), "codeedit") ? kModCtrl : 0);
        seen.push_back(ui.focused());
        if (!(ui.states(ui.focused()) & kStateFocusVisible)) {
            std::fprintf(stderr, "Tab focus not visible on %s\n", nameOf(ui, ui.focused()).c_str());
            UI_CHECK(false);
        }
    }
    UI_CHECK(seen == order);
    press(ui, Key::Tab);
    UI_CHECK(ui.focused() == order.front()); // wraps
    // backward: Shift+Tab walks the same order in reverse (the code editor lets Shift+Tab through)
    std::vector<NodeHandle> back;
    for (std::size_t i = 0; i < order.size(); ++i) {
        press(ui, Key::Tab, kModShift);
        back.push_back(ui.focused());
    }
    std::vector<NodeHandle> rev(order.rbegin(), order.rend());
    UI_CHECK(back == rev);
    if (back != rev)
        for (std::size_t i = 0; i < std::min(back.size(), rev.size()); ++i)
            if (back[i] != rev[i]) { std::fprintf(stderr, "Shift+Tab #%zu: %s, want %s\n", i, nameOf(ui, back[i]).c_str(), nameOf(ui, rev[i]).c_str()); break; }
    std::printf("tab order: %zu stops, forward and back\n", order.size());
}

// ---- focus ring: focused paint != unfocused paint ---------------------------------------------------------------------
int ringFailures(UiContext& ui, const char* theme) {
    std::vector<NodeHandle> order;
    ui.setFocus({});
    ui.update();
    ui.tabOrder(order);
    Image img;
    img.resize(int(ui.viewport().w), int(ui.viewport().h), 0xFF000000u);
    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    auto grab = [&](Rect box, std::vector<u32>& out) {
        const int x0 = std::max(0, int(box.x)), y0 = std::max(0, int(box.y));
        const int x1 = std::min(img.width, int(std::ceil(box.right()))), y1 = std::min(img.height, int(std::ceil(box.bottom())));
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) img.pixels[std::size_t(y) * std::size_t(img.width) + std::size_t(x)] = 0xFF000000u;
        r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        r.render(ui.drawList(), img, box);
        out.clear();
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) out.push_back(img.at(x, y));
    };
    int bad = 0;
    std::vector<u32> a, b;
    for (NodeHandle n : order) {
        ui.setFocus({});
        ui.update();
        const Rect rc = ui.rect(n);
        const Rect box{rc.x - 2.f, rc.y - 2.f, rc.w + 4.f, rc.h + 4.f};
        if (rc.w <= 0.f || rc.right() > ui.viewport().w || rc.bottom() > ui.viewport().h) {
            std::fprintf(stderr, "[%s] %s outside the viewport (%g %g %g %g)\n", theme, nameOf(ui, n).c_str(), double(rc.x), double(rc.y),
                         double(rc.w), double(rc.h));
            ++bad;
            continue;
        }
        grab(box, a);
        ui.setFocus(n, FocusReason::Keyboard);
        ui.update();
        grab(box, b);
        u32 diff = 0;
        for (std::size_t i = 0; i < a.size(); ++i) diff += a[i] != b[i];
        if (diff < 8) {
            std::fprintf(stderr, "[%s] no visible focus ring on %s (%u px differ; rect %g %g %g %g)\n", theme, nameOf(ui, n).c_str(), diff,
                         double(rc.x), double(rc.y), double(rc.w), double(rc.h));
            ++bad;
        }
    }
    ui.setFocus({});
    ui.update();
    std::printf("focus ring [%s]: %zu focusable widgets, %d without a visible ring\n", theme, order.size(), bad);
    return bad;
}

// ---- operation ---------------------------------------------------------------------------------------------------------
void tierA(UiContext& ui, const Form& f) {
    Counter clicks, toolClicks, linkClicks;
    count(ui, f.btn, SignalKind::Clicked, clicks);
    count(ui, f.tool, SignalKind::Clicked, toolClicks);
    count(ui, f.link, SignalKind::Clicked, linkClicks);
    // tooltip on keyboard focus
    UI_CHECK(tabTo(ui, f.btn));
    ui.update(0.6f);
    ui.update(0.f);
    UI_CHECK(ui.computed(f.tip).visible);
    press(ui, Key::Enter);
    press(ui, Key::Space);
    UI_CHECK_EQ(clicks.n, 2);
    press(ui, Key::Tab);
    UI_CHECK(!ui.computed(f.tip).visible);
    UI_CHECK(tabTo(ui, f.tool));
    press(ui, Key::Space);
    UI_CHECK_EQ(toolClicks.n, 1);
    UI_CHECK(tabTo(ui, f.link));
    press(ui, Key::Enter);
    UI_CHECK_EQ(linkClicks.n, 1);
    UI_CHECK(tabTo(ui, f.check));
    press(ui, Key::Space);
    UI_CHECK(ui.checked(f.check));
    press(ui, Key::Space);
    UI_CHECK(!ui.checked(f.check));
    // radio group: arrows move and check, wrapping
    UI_CHECK(tabTo(ui, f.r1));
    press(ui, Key::Down);
    UI_CHECK(ui.focused() == f.r2 && ui.checked(f.r2) && !ui.checked(f.r1));
    press(ui, Key::Right);
    UI_CHECK(ui.focused() == f.r3 && ui.checked(f.r3) && !ui.checked(f.r2));
    press(ui, Key::Down);
    UI_CHECK(ui.focused() == f.r1 && ui.checked(f.r1) && !ui.checked(f.r3));
    press(ui, Key::Up);
    UI_CHECK(ui.focused() == f.r3 && ui.checked(f.r3));
    // a disabled radio refuses focus: Tab and the arrows skip it
    ui.setDisabled(f.r2, true);
    ui.update();
    std::vector<NodeHandle> order;
    ui.tabOrder(order);
    UI_CHECK(std::find(order.begin(), order.end(), f.r2) == order.end());
    press(ui, Key::Down);
    UI_CHECK(ui.focused() == f.r1 && ui.checked(f.r1));
    press(ui, Key::Down);
    UI_CHECK(ui.focused() == f.r3 && ui.checked(f.r3) && !ui.checked(f.r2));
    ui.setDisabled(f.r2, false);
    ui.update();
    UI_CHECK(tabTo(ui, f.sw));
    press(ui, Key::Space);
    UI_CHECK(ui.checked(f.sw));
    // ranges
    UI_CHECK(tabTo(ui, f.slider));
    press(ui, Key::Right);
    UI_CHECK_EQ(ui.value(f.slider), 51.f);
    press(ui, Key::Home);
    UI_CHECK_EQ(ui.value(f.slider), 0.f);
    press(ui, Key::PageUp);
    UI_CHECK_EQ(ui.value(f.slider), 10.f);
    press(ui, Key::End);
    UI_CHECK_EQ(ui.value(f.slider), 100.f);
    press(ui, Key::Left);
    UI_CHECK_EQ(ui.value(f.slider), 99.f);
    UI_CHECK(tabTo(ui, f.sbar));
    press(ui, Key::End);
    UI_CHECK_EQ(ui.value(f.sbar), 1.f);
    press(ui, Key::Home);
    UI_CHECK_EQ(ui.value(f.sbar), 0.f);
    press(ui, Key::Right);
    const f32 line = ui.value(f.sbar);
    UI_CHECK(line > 0.f);
    press(ui, Key::PageDown);
    UI_CHECK(ui.value(f.sbar) > line);
    UI_CHECK(tabTo(ui, f.spin));
    press(ui, Key::Up);
    UI_CHECK_EQ(ui.value(f.spin), 6.f);
    press(ui, Key::Down);
    press(ui, Key::Down);
    UI_CHECK_EQ(ui.value(f.spin), 4.f);
    press(ui, Key::End);
    UI_CHECK_EQ(ui.value(f.spin), 10.f);
    press(ui, Key::Home);
    UI_CHECK_EQ(ui.value(f.spin), 0.f);
    UI_CHECK(tabTo(ui, f.dial));
    press(ui, Key::Up);
    UI_CHECK_EQ(ui.value(f.dial), 51.f);
    press(ui, Key::End);
    UI_CHECK_EQ(ui.value(f.dial), 100.f);
    // combo box: arrows pick in place; Alt+Down / Space / F4 open the list, arrows + Enter pick, Escape closes
    UI_CHECK(tabTo(ui, f.combo));
    press(ui, Key::Down);
    UI_CHECK_EQ(comboText(ui, f.combo), std::string_view("Beta"));
    press(ui, Key::Down, kModAlt);
    UI_CHECK(isA(ui, ui.focused(), "menuitem") && ui.text(ui.focused()) == "Beta");
    press(ui, Key::Down);
    press(ui, Key::Enter);
    UI_CHECK_EQ(comboText(ui, f.combo), std::string_view("Gamma"));
    UI_CHECK(ui.focused() == f.combo && ui.focusVisible());
    press(ui, Key::Space);
    UI_CHECK(isA(ui, ui.focused(), "menuitem"));
    press(ui, Key::Up);
    press(ui, Key::Escape);
    UI_CHECK(ui.focused() == f.combo && ui.focusVisible());
    UI_CHECK_EQ(comboText(ui, f.combo), std::string_view("Gamma"));
    press(ui, Key::F4);
    UI_CHECK(isA(ui, ui.focused(), "menuitem"));
    press(ui, Key::Home);
    press(ui, Key::Enter);
    UI_CHECK_EQ(comboText(ui, f.combo), std::string_view("Alpha"));
    // line edit
    Counter submitted;
    count(ui, f.line, SignalKind::Submitted, submitted);
    UI_CHECK(tabTo(ui, f.line));
    typeText(ui, "abc");
    press(ui, Key::Backspace);
    UI_CHECK_EQ(ui.text(f.line), std::string_view("ab"));
    press(ui, Key::Home);
    typeText(ui, "x");
    press(ui, Key::Enter);
    UI_CHECK_EQ(submitted.n, 1);
    UI_CHECK_EQ(submitted.text, std::string("xab"));
}

void menus(UiContext& ui, const Form& f) {
    Counter picked;
    count(ui, f.file, SignalKind::Submitted, picked);
    // F10 from a field focuses the bar; Escape (or F10) goes back
    UI_CHECK(tabTo(ui, f.line));
    press(ui, Key::F10);
    UI_CHECK(ui.focused() == f.file && ui.focusVisible());
    press(ui, Key::Right);
    UI_CHECK(ui.focused() == f.edit);
    press(ui, Key::Right);
    UI_CHECK(ui.focused() == f.file); // wraps
    press(ui, Key::Escape);
    UI_CHECK(ui.focused() == f.line);
    press(ui, Key::F10);
    UI_CHECK(ui.focused() == f.file);
    press(ui, Key::F10);
    UI_CHECK(ui.focused() == f.line);
    // open with Down, Right switches to the next menu, Escape closes back to its title
    press(ui, Key::F10);
    press(ui, Key::Down);
    UI_CHECK(isA(ui, ui.focused(), "menuitem") && ui.text(ui.focused()) == "New");
    press(ui, Key::Right);
    UI_CHECK(isA(ui, ui.focused(), "menuitem") && ui.text(ui.focused()) == "Undo");
    press(ui, Key::Escape);
    UI_CHECK(ui.focused() == f.edit && ui.focusVisible());
    press(ui, Key::Left);
    UI_CHECK(ui.focused() == f.file);
    // Enter opens, Down + Enter picks "Open" (the separator is skipped by End / Up)
    press(ui, Key::Enter);
    press(ui, Key::Down);
    UI_CHECK(ui.text(ui.focused()) == "Open");
    press(ui, Key::Enter);
    UI_CHECK_EQ(picked.n, 1);
    UI_CHECK_EQ(picked.text, std::string("Open"));
    UI_CHECK(ui.focused() == f.file);
    press(ui, Key::Space);
    press(ui, Key::End);
    UI_CHECK(ui.text(ui.focused()) == "Quit");
    press(ui, Key::Escape);
    press(ui, Key::Escape);
    UI_CHECK(ui.focused() == f.line);
}

void tierB(UiContext& ui, const Form& f) {
    Counter listSub, tableSub, gridSub, treeSub;
    count(ui, f.list, SignalKind::Submitted, listSub);
    count(ui, f.table, SignalKind::Submitted, tableSub);
    count(ui, f.grid, SignalKind::Submitted, gridSub);
    count(ui, f.tree, SignalKind::Submitted, treeSub);
    UI_CHECK(tabTo(ui, f.list));
    press(ui, Key::Down);
    UI_CHECK_EQ(ui.value(f.list), 0.f);
    press(ui, Key::End);
    UI_CHECK_EQ(ui.value(f.list), 19.f);
    press(ui, Key::Home);
    press(ui, Key::PageDown);
    UI_CHECK(ui.value(f.list) > 1.f);
    press(ui, Key::PageUp);
    UI_CHECK_EQ(ui.value(f.list), 0.f);
    press(ui, Key::Enter);
    UI_CHECK_EQ(listSub.n, 1);
    // tree: rows Root, Child A, Child B, Other; Left collapses, Right expands
    UI_CHECK(tabTo(ui, f.tree));
    press(ui, Key::Down);
    UI_CHECK_EQ(ui.value(f.tree), 0.f);
    press(ui, Key::Left);
    UI_CHECK(!treeExpanded(ui, f.tree, 0));
    press(ui, Key::Down);
    UI_CHECK_EQ(ui.value(f.tree), 3.f); // collapsed children skipped
    press(ui, Key::Up);
    press(ui, Key::Right);
    UI_CHECK(treeExpanded(ui, f.tree, 0));
    press(ui, Key::Down);
    UI_CHECK_EQ(ui.value(f.tree), 1.f);
    press(ui, Key::End);
    UI_CHECK_EQ(ui.value(f.tree), 3.f);
    press(ui, Key::Home);
    UI_CHECK_EQ(ui.value(f.tree), 0.f);
    press(ui, Key::Enter);
    UI_CHECK_EQ(treeSub.n, 1);
    UI_CHECK(tabTo(ui, f.tabs));
    press(ui, Key::Right);
    UI_CHECK_EQ(ui.value(f.tabs), 1.f);
    press(ui, Key::End);
    UI_CHECK_EQ(ui.value(f.tabs), 2.f);
    press(ui, Key::Home);
    UI_CHECK_EQ(ui.value(f.tabs), 0.f);
    // splitter handle: arrows resize the first pane
    UI_CHECK(tabTo(ui, f.handle));
    const f32 w0 = ui.rect(ui.firstChild(f.split)).w;
    press(ui, Key::Right);
    ui.update();
    UI_CHECK_EQ(ui.rect(ui.firstChild(f.split)).w, w0 + 16.f);
    press(ui, Key::Left);
    ui.update();
    UI_CHECK_EQ(ui.rect(ui.firstChild(f.split)).w, w0);
    // table / grid
    UI_CHECK(tabTo(ui, f.table));
    press(ui, Key::Down);
    UI_CHECK_EQ(ui.value(f.table), 0.f);
    press(ui, Key::End);
    UI_CHECK_EQ(ui.value(f.table), 29.f);
    press(ui, Key::PageUp);
    UI_CHECK(ui.value(f.table) < 29.f);
    press(ui, Key::Home);
    UI_CHECK_EQ(ui.value(f.table), 0.f);
    press(ui, Key::Enter);
    UI_CHECK_EQ(tableSub.n, 1);
    UI_CHECK(tabTo(ui, f.grid));
    press(ui, Key::Right);
    UI_CHECK_EQ(ui.value(f.grid), 0.f);
    press(ui, Key::Right);
    press(ui, Key::Down);
    UI_CHECK_EQ(ui.value(f.grid), f32(1 + gridColumns(ui, f.grid)));
    press(ui, Key::End);
    UI_CHECK_EQ(ui.value(f.grid), 29.f);
    press(ui, Key::Home);
    press(ui, Key::Enter);
    UI_CHECK_EQ(gridSub.n, 1);
}

void tierCD(UiContext& ui, const Form& f) {
    Counter props;
    count(ui, f.props, SignalKind::Submitted, props);
    // property grid: the group row folds with Space, editors take their own keys
    UI_CHECK(tabTo(ui, f.propGroup));
    press(ui, Key::Space);
    UI_CHECK(!ui.computed(ui.parent(f.propBool)).visible);
    press(ui, Key::Enter);
    UI_CHECK(ui.computed(ui.parent(f.propBool)).visible);
    UI_CHECK(tabTo(ui, f.propBool));
    press(ui, Key::Space);
    UI_CHECK_EQ(propertyValue(ui, f.props, 1), std::string("true"));
    UI_CHECK(tabTo(ui, f.propNum));
    press(ui, Key::Up);
    UI_CHECK_EQ(propertyValue(ui, f.props, 2), std::string("3"));
    UI_CHECK(tabTo(ui, f.propEnum));
    press(ui, Key::Down);
    UI_CHECK_EQ(propertyValue(ui, f.props, 3), std::string("1"));
    UI_CHECK(tabTo(ui, f.propText));
    typeText(ui, "s");
    UI_CHECK_EQ(propertyValue(ui, f.props, 4), std::string("cubes"));
    UI_CHECK_EQ(props.n, 4);
    // colour picker: arrows saturation / value, Shift+Left / Right alpha, Page keys hue
    UI_CHECK(tabTo(ui, f.picker));
    const std::string c0(ui.text(f.picker));
    press(ui, Key::Right);
    const std::string c1(ui.text(f.picker));
    UI_CHECK(c1 != c0);
    press(ui, Key::Left, kModShift);
    UI_CHECK(colorPickerColor(ui, f.picker).a < 0.99f);
    const f32 hue = ui.value(f.picker);
    press(ui, Key::PageDown);
    UI_CHECK(ui.value(f.picker) != hue);
    // calendar: 2026-11-05
    UI_CHECK(tabTo(ui, f.cal));
    int y, m, d;
    press(ui, Key::Right);
    calendarDate(ui, f.cal, y, m, d);
    UI_CHECK(y == 2026 && m == 11 && d == 6);
    press(ui, Key::Down);
    calendarDate(ui, f.cal, y, m, d);
    UI_CHECK(d == 13);
    press(ui, Key::End);
    calendarDate(ui, f.cal, y, m, d);
    UI_CHECK(d == 30);
    press(ui, Key::PageDown);
    calendarDate(ui, f.cal, y, m, d);
    UI_CHECK(m == 12 && d == 30);
    press(ui, Key::Home);
    calendarDate(ui, f.cal, y, m, d);
    UI_CHECK(m == 12 && d == 1);
    // chart: the readout steps through the points
    UI_CHECK(tabTo(ui, f.chart));
    press(ui, Key::Right);
    UI_CHECK_EQ(ui.value(f.chart), 0.f);
    press(ui, Key::End);
    UI_CHECK_EQ(ui.value(f.chart), 4.f);
    press(ui, Key::Left);
    UI_CHECK_EQ(ui.value(f.chart), 3.f);
}

void tierD(UiContext& ui, const Form& f) {
    // font picker: family list + size spin box
    Counter font;
    count(ui, f.font, SignalKind::Submitted, font);
    NodeHandle fontList = ui.firstChild(f.font);
    UI_CHECK(tabTo(ui, fontList));
    press(ui, Key::Down);
    UI_CHECK_EQ(fontPickerFamily(ui, f.font), std::string("Serif"));
    press(ui, Key::Tab);
    UI_CHECK(isA(ui, ui.focused(), "spinbox"));
    press(ui, Key::Up);
    UI_CHECK_EQ(fontPickerSize(ui, f.font), 15.f);
    UI_CHECK_EQ(font.n, 2);
    // node graph: select, move, delete
    UI_CHECK(tabTo(ui, f.graph));
    press(ui, Key::PageDown);
    UI_CHECK_EQ(ui.value(f.graph), 0.f);
    press(ui, Key::Right);
    press(ui, Key::Down, kModShift);
    const Vec2 p = nodeGraphNodePos(ui, f.graph, 0);
    UI_CHECK(p.x == 20.f && p.y == 11.f);
    press(ui, Key::PageDown);
    UI_CHECK_EQ(ui.value(f.graph), 1.f);
    press(ui, Key::Delete);
    UI_CHECK_EQ(nodeGraphNodeCount(ui, f.graph), 1u);
    UI_CHECK_EQ(nodeGraphLinkCount(ui, f.graph), 0u);
    press(ui, Key::PageUp);
    UI_CHECK_EQ(ui.value(f.graph), 0.f); // the dead node is skipped
    press(ui, Key::Escape);
    UI_CHECK_EQ(ui.value(f.graph), -1.f);
    // curve editor: select a key, move it, delete it
    UI_CHECK(tabTo(ui, f.curve));
    press(ui, Key::PageDown);
    press(ui, Key::PageDown);
    UI_CHECK_EQ(ui.value(f.curve), 1.f);
    press(ui, Key::Up);
    press(ui, Key::Right, kModShift);
    const Vec2 k = curveKey(ui, f.curve, 1);
    UI_CHECK(std::abs(k.x - 0.6f) < 1e-4f && std::abs(k.y - 0.51f) < 1e-4f);
    press(ui, Key::Delete);
    UI_CHECK_EQ(curveKeyCount(ui, f.curve), 2u);
    // timeline: the playhead
    UI_CHECK(tabTo(ui, f.tl));
    press(ui, Key::PageDown);
    UI_CHECK_EQ(ui.value(f.tl), 1.f);
    press(ui, Key::End);
    UI_CHECK_EQ(ui.value(f.tl), 10.f);
    press(ui, Key::Left);
    UI_CHECK(ui.value(f.tl) < 10.f);
    press(ui, Key::Home);
    UI_CHECK_EQ(ui.value(f.tl), 0.f);
    // viewport: keys are forwarded, Tab is not swallowed
    UI_CHECK(tabTo(ui, f.vp));
    const u32 before = viewportInputCount(ui, f.vp);
    press(ui, Key::W);
    UI_CHECK(viewportInputCount(ui, f.vp) > before && viewportLastInput(ui, f.vp).key == Key::W);
    press(ui, Key::Tab);
    UI_CHECK(ui.focused() == f.code);
    // code editor: types, Tab indents, Shift+Tab / Ctrl+Tab leave
    press(ui, Key::End, kModCtrl);
    typeText(ui, "0");
    press(ui, Key::Home);
    press(ui, Key::Tab);
    UI_CHECK_EQ(codeEditText(ui, f.code), std::string("    local a = 10"));
    UI_CHECK(ui.focused() == f.code);
    press(ui, Key::Tab, kModShift);
    UI_CHECK(ui.focused() == f.vp);
    press(ui, Key::Tab);
    press(ui, Key::Tab, kModCtrl);
    UI_CHECK(ui.focused() == f.dt);
    // date-time edit: fields, steps, Alt+Down calendar popup
    int yy, mo, dd, hh, mi;
    press(ui, Key::Up);
    dateTimeEditValue(ui, f.dt, yy, mo, dd, hh, mi);
    UI_CHECK_EQ(yy, 2027);
    press(ui, Key::End);
    press(ui, Key::Up);
    dateTimeEditValue(ui, f.dt, yy, mo, dd, hh, mi);
    UI_CHECK_EQ(mi, 31);
    press(ui, Key::Down, kModAlt);
    UI_CHECK(isA(ui, ui.focused(), "calendar"));
    press(ui, Key::Right);
    dateTimeEditValue(ui, f.dt, yy, mo, dd, hh, mi);
    UI_CHECK_EQ(dd, 6);
    press(ui, Key::Enter);
    UI_CHECK(ui.focused() == f.dt && ui.focusVisible());
    press(ui, Key::Space);
    UI_CHECK(isA(ui, ui.focused(), "calendar"));
    press(ui, Key::Escape);
    UI_CHECK(ui.focused() == f.dt);
    // video
    UI_CHECK(tabTo(ui, f.video));
    press(ui, Key::Space);
    UI_CHECK(videoPlaying(ui, f.video));
    press(ui, Key::Space);
    UI_CHECK(!videoPlaying(ui, f.video));
    press(ui, Key::End);
    UI_CHECK_EQ(videoFrame(ui, f.video), testPatternSource().frames - 1);
    press(ui, Key::Home);
    press(ui, Key::Right);
    UI_CHECK_EQ(videoFrame(ui, f.video), 1u);
}

// ---- dialogs: focus trap, Escape cancels, focus returns ----------------------------------------------------------------
void dialogs(UiContext& ui, const Form& f) {
    static NodeHandle dlg; // the slots outlive this function
    static Counter result;
    ui.connect(f.openFile, SignalKind::Clicked, [](UiContext& u, NodeHandle, const SignalArgs&) {
        if (!u.alive(dlg)) {
            dlg = fileDialog(u, "Open", "data", "*.ttf;*.fuseui");
            count(u, dlg, SignalKind::Submitted, result);
        }
        openDialog(u, dlg);
    });
    UI_CHECK(tabTo(ui, f.openFile));
    press(ui, Key::Enter);
    UI_CHECK(ui.alive(dlg) && popupOpen(ui, ui.parent(dlg)));
    UI_CHECK(inside(ui, ui.focused(), dlg));
    bool trapped = true;
    for (int i = 0; i < 24; ++i) {
        press(ui, Key::Tab, i & 1 ? kModShift : 0);
        press(ui, Key::Tab);
        trapped = trapped && inside(ui, ui.focused(), dlg);
    }
    UI_CHECK(trapped);
    // the listing: Down + Enter on a folder enters it
    NodeHandle list;
    for (int i = 0; i < 12 && !isA(ui, list, "listview"); ++i) { press(ui, Key::Tab); list = ui.focused(); }
    UI_CHECK(isA(ui, list, "listview"));
    const std::string dir0 = fileDialogDirectory(ui, dlg);
    press(ui, Key::Down);
    const std::string first = fileDialogEntry(ui, dlg, 0);
    press(ui, Key::Enter);
    if (!first.empty() && first.back() == '/') UI_CHECK(fileDialogDirectory(ui, dlg) != dir0);
    press(ui, Key::Escape);
    UI_CHECK(!popupOpen(ui, ui.parent(dlg)));
    UI_CHECK_EQ(result.n, 1);
    UI_CHECK(result.text.empty());
    UI_CHECK(ui.focused() == f.openFile && ui.focusVisible());
}

void dock(UiContext& ui, const Form& f) {
    Counter closed;
    count(ui, f.dock, SignalKind::Toggled, closed);
    UI_CHECK(tabTo(ui, f.dockTitle));
    press(ui, Key::Enter);
    UI_CHECK_EQ(closed.n, 1);
    UI_CHECK(ui.hasClass(f.dock, "dock-closed"));
    UI_CHECK(ui.focused() != f.dockTitle && ui.alive(ui.focused()));
    std::vector<NodeHandle> order;
    ui.tabOrder(order);
    UI_CHECK(std::find(order.begin(), order.end(), f.dockTitle) == order.end());
}

// ---- keyboard focus scrolls into view ----------------------------------------------------------------------------------
void scrollIntoView() {
    UiContext ui({.viewport = {300, 200}, .font = &font()});
    install(ui);
    const NodeHandle area = scrollArea(ui, ui.root(), "area");
    ui.setCss(area, "height", "120px");
    NodeHandle last;
    for (int i = 0; i < 12; ++i) last = button(ui, area, "Button " + std::to_string(i));
    ui.update();
    for (int i = 0; i < 12; ++i) press(ui, Key::Tab);
    ui.update();
    UI_CHECK(ui.focused() == last);
    const Rect c = ui.contentRect(area), r = ui.rect(last);
    UI_CHECK(ui.scroll(area).y > 0.f && r.y >= c.y - 0.5f && r.bottom() <= c.bottom() + 0.5f);
    press(ui, Key::Tab); // wraps to the first
    ui.update();
    UI_CHECK_EQ(ui.scroll(area).y, 0.f);
}

} // namespace

int main() {
    StringTableModel model("Name\tSize");
    for (int i = 0; i < 30; ++i) model.addRow("row " + std::to_string(i) + "\t" + std::to_string(i * 7));
    UiContext ui({.viewport = {1900, 1500}, .font = &font()});
    const Form f = build(ui, &model.model());

    tabOrder(ui, f);
    UI_CHECK_EQ(ringFailures(ui, "dark"), 0);
    (void)ui.setThemeStyleSheet(themeCss(ThemeVariant::Light));
    ui.update();
    UI_CHECK_EQ(ringFailures(ui, "light"), 0);
    (void)ui.setThemeStyleSheet(themeCss(ThemeVariant::Dark));
    ui.update();

    tierA(ui, f);
    menus(ui, f);
    tierB(ui, f);
    tierCD(ui, f);
    tierD(ui, f);
    dialogs(ui, f);
    dock(ui, f);
    scrollIntoView();
    return finish("fuse_ui_keyboard_only");
}
