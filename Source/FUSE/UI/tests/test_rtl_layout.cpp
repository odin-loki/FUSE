// Gate fuse_ui_rtl_layout: `direction: rtl` mirrors layout and widgets. Behaviour checks (box rows / justify / align /
// grid / flow mirrored with physical margins, inheritance, text-align start = right, slider pointer + keys, tab bar,
// tree view, scroll container bar side + horizontal start, menus open right-aligned and submenus to the left; Tier
// B2 / C / D: splitter drag + keys, dock region side + close box, table columns / sort / resize / keyboard column /
// bar side, grid view, calendar columns + chevrons + keys, chart axis + keys, colour picker keys + pointer, node
// graph pins / link drag / keys; tooltip and popup panel placement, left-to-right islands (code, date-time, time
// axes); FULL: paragraph direction from `direction`, line edit caret in visual order over bidi text), pixel checks
// (text-free widgets in RTL render as the mirror image of LTR, Tier A and Tier B2 / C / D) and a CPU-raster golden
// of the RTL sample tree (tests/golden/rtl_layout.png, rtl_layout_full.png in FULL; regenerate with
// FUSE_UI_UPDATE_GOLDEN=1).
#include "test_util.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdlib>

using namespace fuse::ui;
using namespace fuse::ui::test;
using namespace fuse::ui::widgets;

namespace {

u8 ch(u32 p, int c) { return u8((p >> (8 * c)) & 0xFF); }
f32 mirrored(const Rect& r, const Rect& box) { return 2.f * box.x + box.w - r.x - r.w; }

// A 300 px wide row of three buttons of different widths inside a padded container.
struct Row3 {
    NodeHandle row, a, b, c;
};
Row3 buildRow(UiContext& ui, bool rtl) {
    widgets::install(ui);
    if (rtl) ui.setCss(ui.root(), "direction", "rtl");
    const NodeHandle row = panel(ui, ui.root(), "row");
    ui.addClass(row, "row");
    ui.setCss(row, "width", "300px");
    ui.setCss(row, "padding", "10px");
    return {row, button(ui, row, "One", "a"), button(ui, row, "Second", "b"), button(ui, row, "3", "c")};
}

void boxRow() {
    UiContext l({.viewport = {400, 120}, .font = &font()}), r({.viewport = {400, 120}, .font = &font()});
    const Row3 L = buildRow(l, false), R = buildRow(r, true);
    l.update();
    r.update();
    UI_CHECK(r.computed(R.a).rtl);  // inherited from the root
    UI_CHECK(!l.computed(L.a).rtl);
    const Rect box = r.contentRect(R.row), lbox = l.contentRect(L.row);
    const Rect lrow = l.rect(L.row);
    UI_CHECK_RECT(r.rect(R.row), mirrored(lrow, {0.f, 0.f, 400.f, 120.f}), lrow.y, lrow.w, lrow.h); // the row itself too
    for (auto [ln, rn] : {std::pair{L.a, R.a}, std::pair{L.b, R.b}, std::pair{L.c, R.c}}) {
        const Rect lr = l.rect(ln), rr = r.rect(rn);
        UI_CHECK_RECT(rr, mirrored(lr, lbox) + (box.x - lbox.x), lr.y, lr.w, lr.h);
    }
    UI_CHECK(r.rect(R.a).x > r.rect(R.b).x && r.rect(R.b).x > r.rect(R.c).x); // first child on the right
    UI_CHECK_EQ(r.rect(R.a).right(), box.right());

    // justify-content: start / end follow the inline direction
    r.setCss(R.row, "justify-content", "end");
    r.update();
    UI_CHECK_EQ(r.rect(R.c).x, box.x);
    r.setCss(R.row, "justify-content", "start");
    // margins stay physical: margin-left of the middle button is the gap to its left neighbour (c in RTL)
    r.setCss(R.b, "margin-left", "20px");
    r.update();
    UI_CHECK_EQ(r.rect(R.b).x - r.rect(R.c).right(), 8.f + 20.f);
    UI_CHECK_EQ(r.rect(R.a).x - r.rect(R.b).right(), 8.f);
    r.setCss(R.b, "margin-left", "0px");
    // column: align-items start = the right edge, `direction: ltr` on a child subtree switches back
    r.setCss(R.row, "flex-direction", "column");
    r.setCss(R.row, "align-items", "start");
    r.update();
    UI_CHECK_EQ(r.rect(R.a).right(), box.right());
    UI_CHECK_EQ(r.rect(R.c).right(), box.right());
    r.setCss(R.row, "direction", "ltr");
    r.update();
    UI_CHECK(!r.computed(R.a).rtl);
    UI_CHECK_EQ(r.rect(R.a).x, box.x);
}

void gridAndFlow() {
    UiContext ui({.viewport = {400, 300}, .font = &font()});
    widgets::install(ui);
    const NodeHandle g = panel(ui, ui.root(), "grid");
    ui.setCss(g, "direction", "rtl");
    ui.setCss(g, "layout", "grid");
    ui.setCss(g, "grid-template-columns", "100px 50px 1fr");
    ui.setCss(g, "width", "300px");
    NodeHandle cell[3];
    for (int k = 0; k < 3; ++k) cell[k] = button(ui, g, "x");
    const NodeHandle f = panel(ui, ui.root(), "flow");
    ui.setCss(f, "direction", "rtl");
    ui.setCss(f, "layout", "flow");
    ui.setCss(f, "width", "200px");
    ui.setCss(f, "gap", "4px");
    NodeHandle chip[4];
    for (int k = 0; k < 4; ++k) {
        chip[k] = panel(ui, f);
        ui.setCss(chip[k], "width", "60px");
        ui.setCss(chip[k], "height", "10px");
    }
    ui.update();
    const Rect gc = ui.contentRect(g);
    UI_CHECK_EQ(ui.rect(cell[0]).right(), gc.right()); // column 1 is the rightmost (100 px)
    UI_CHECK_EQ(ui.rect(cell[0]).w, 100.f);
    UI_CHECK_EQ(ui.rect(cell[1]).right(), gc.right() - 100.f);
    UI_CHECK_EQ(ui.rect(cell[2]).x, gc.x);
    const Rect fc = ui.contentRect(f);
    UI_CHECK_EQ(ui.rect(chip[0]).right(), fc.right()); // lines fill from the right
    UI_CHECK_EQ(ui.rect(chip[1]).right(), fc.right() - 64.f);
    UI_CHECK_EQ(ui.rect(chip[3]).right(), fc.right()); // second line starts at the right again
    UI_CHECK(ui.rect(chip[3]).y > ui.rect(chip[0]).y);
}

void textAlign() {
    UiContext ui({.viewport = {400, 200}, .font = &font()});
    widgets::install(ui);
    ui.setCss(ui.root(), "direction", "rtl");
    auto make = [&](const char* align) {
        const NodeHandle n = label(ui, ui.root(), "Hi");
        ui.setCss(n, "width", "200px");
        if (align) ui.setCss(n, "text-align", align);
        return n;
    };
    const NodeHandle start = make(nullptr), end = make("end"), left = make("left"), right = make("right");
    ui.update();
    auto firstX = [&](NodeHandle n) {
        const auto* tl = ui.textLayout(n);
        return tl && !tl->glyphs.empty() ? tl->glyphs[0].x : -1.f;
    };
    UI_CHECK(firstX(start) > 150.f); // start = right in RTL
    UI_CHECK(firstX(right) > 150.f);
    UI_CHECK(firstX(end) >= 0.f && firstX(end) < 10.f);
    UI_CHECK(firstX(left) >= 0.f && firstX(left) < 10.f);
    ui.setCss(ui.root(), "direction", "ltr");
    ui.update();
    UI_CHECK(firstX(start) >= 0.f && firstX(start) < 10.f);
    UI_CHECK(firstX(right) > 150.f);
}

void sliderAndTabs() {
    UiContext ui({.viewport = {400, 200}, .font = &font()});
    widgets::install(ui);
    ui.setCss(ui.root(), "direction", "rtl");
    const NodeHandle s = slider(ui, ui.root(), 0.f, 10.f, 5.f, 1.f, "s");
    ui.setCss(s, "width", "200px");
    const NodeHandle tabs = tabBar(ui, ui.root(), "One\nTwo\nThree", 0, "tabs");
    ui.setCss(tabs, "align-self", "start"); // sized to its tabs, at the start (right) edge
    ui.update();
    UiTestDriver d(ui);
    ui.setFocus(s, FocusReason::Keyboard);
    d.key(Key::Left);
    UI_CHECK_EQ(ui.value(s), 6.f); // Left increases (moves the knob left, towards the maximum)
    d.key(Key::Right);
    d.key(Key::Right);
    UI_CHECK_EQ(ui.value(s), 4.f);
    const Rect c = ui.contentRect(s);
    d.clickAt({c.right() - 8.f, c.y + c.h * 0.5f}); // the right end is the minimum
    UI_CHECK(ui.value(s) <= 0.5f);
    d.clickAt({c.x + 8.f, c.y + c.h * 0.5f});
    UI_CHECK(ui.value(s) >= 9.5f);
    // tab bar: the first tab on the right, Left selects the next one
    const Rect tr = ui.contentRect(tabs);
    d.clickAt({tr.x + 4.f, tr.y + tr.h * 0.5f});
    UI_CHECK_EQ(ui.value(tabs), 2.f);
    d.clickAt({tr.right() - 4.f, tr.y + tr.h * 0.5f});
    UI_CHECK_EQ(ui.value(tabs), 0.f);
    ui.setFocus(tabs, FocusReason::Keyboard);
    d.key(Key::Left);
    UI_CHECK_EQ(ui.value(tabs), 1.f);
}

void treeKeys() {
    UiContext ui({.viewport = {400, 300}, .font = &font()});
    widgets::install(ui);
    ui.setCss(ui.root(), "direction", "rtl");
    const NodeHandle t = treeView(ui, ui.root(), "Root\n\tChild\n\tOther", "tree");
    setTreeExpanded(ui, t, 0, false);
    ui.update();
    ui.setFocus(t, FocusReason::Keyboard);
    UiTestDriver d(ui);
    d.key(Key::Down); // selects Root
    UI_CHECK_EQ(ui.value(t), 0.f);
    d.key(Key::Left); // expands in RTL
    UI_CHECK(treeExpanded(ui, t, 0));
    d.key(Key::Right);
    UI_CHECK(!treeExpanded(ui, t, 0));
    // the expander sits at the right edge
    const Rect c = ui.contentRect(t);
    d.clickAt({c.right() - 9.f, c.y + 6.f});
    UI_CHECK(treeExpanded(ui, t, 0));
}

void scrollContainer() {
    UiContext ui({.viewport = {400, 300}, .font = &font()});
    widgets::install(ui);
    ui.setCss(ui.root(), "direction", "rtl");
    const NodeHandle sa = scrollArea(ui, ui.root(), "sa");
    ui.setCss(sa, "width", "200px");
    ui.setCss(sa, "height", "100px");
    ui.setCss(sa, "flex-direction", "row");
    const NodeHandle wide = panel(ui, sa, "wide");
    ui.setCss(wide, "width", "500px");
    ui.setCss(wide, "height", "400px");
    ui.setCss(wide, "flex-shrink", "0");
    ui.update();
    const Rect r = ui.rect(sa), c = ui.contentRect(sa);
    UI_CHECK_EQ(ui.rect(wide).right(), c.right()); // horizontal overflow goes to the left
    // the vertical bar is on the left: a press there scrolls (jump to the pointer)
    ui.pointerMove({r.x + 4.f, r.bottom() - 20.f});
    ui.pointerDown({r.x + 4.f, r.bottom() - 20.f});
    ui.pointerUp({r.x + 4.f, r.bottom() - 20.f});
    ui.update();
    UI_CHECK(ui.scroll(sa).y > 100.f);
    // horizontal: scroll.x counts from the right edge; the content moves right as it grows
    ui.scrollTo(sa, {50.f, 0.f});
    ui.update();
    UI_CHECK_EQ(ui.rect(wide).right(), c.right() + 50.f);
    // a horizontal bar press near the left end scrolls to the far (left) end of the content
    ui.pointerDown({r.x + 20.f, r.bottom() - 4.f});
    ui.pointerUp({r.x + 20.f, r.bottom() - 4.f});
    ui.update();
    UI_CHECK(ui.scroll(sa).x > 250.f);
}

void menus() {
    UiContext ui({.viewport = {600, 400}, .font = &font()});
    widgets::install(ui);
    ui.setCss(ui.root(), "direction", "rtl");
    const NodeHandle bar = menuBar(ui, ui.root(), "bar");
    const NodeHandle file = menuBarMenu(ui, bar, "File", "Open\nRecent\n\tA.txt\n\tB.txt\nQuit", "file");
    const NodeHandle edit = menuBarMenu(ui, bar, "Edit", "Undo", "edit");
    ui.update();
    UI_CHECK(ui.rect(file).x > ui.rect(edit).x); // menu bar order: the first title on the right
    UI_CHECK_EQ(ui.rect(file).right(), ui.contentRect(bar).right());
    UiTestDriver d(ui);
    d.click("file");
    ui.update();
    const NodeHandle menu = ui.parent(ui.focused());
    UI_CHECK(ui.alive(menu));
    UI_CHECK(ui.computed(menu).rtl);
    UI_CHECK_EQ(ui.rect(menu).right(), ui.rect(file).right()); // right-aligned to its button
    d.key(Key::Down); // Recent
    UI_CHECK_EQ(ui.text(ui.focused()).substr(0, 6), std::string_view("Recent"));
    d.key(Key::Left); // opens the submenu (to the left)
    ui.update();
    const NodeHandle sub = ui.parent(ui.focused());
    UI_CHECK(ui.alive(sub) && sub != menu);
    UI_CHECK(ui.rect(sub).right() <= ui.rect(menu).x + 4.f);
    UI_CHECK(ui.rect(sub).x < ui.rect(menu).x);
    d.key(Key::Right); // back to the parent menu
    ui.update();
    UI_CHECK(ui.parent(ui.focused()) == menu);
}

// Text-free widgets: the RTL frame is the LTR frame mirrored.
void buildPlain(UiContext& ui, bool rtl) {
    widgets::install(ui);
    if (rtl) ui.setCss(ui.root(), "direction", "rtl");
    ui.setCss(ui.root(), "padding", "12px 20px 12px 20px");
    ui.setCss(ui.root(), "gap", "8px");
    const NodeHandle r1 = panel(ui, ui.root());
    ui.addClass(r1, "row");
    checkBox(ui, r1, "", false);
    radioButton(ui, r1, "", true);
    toggleSwitch(ui, r1, "", true);
    toggleSwitch(ui, r1, "", false);
    const NodeHandle b = button(ui, r1, "");
    ui.setCss(b, "width", "60px");
    ui.setCss(b, "height", "20px");
    const NodeHandle s = slider(ui, ui.root(), 0.f, 1.f, 0.3f, 0.f);
    ui.setCss(s, "width", "180px");
    progressBar(ui, ui.root(), 0.35f);
    const NodeHandle sb = scrollBar(ui, ui.root(), false);
    ui.setCss(sb, "width", "150px");
    ui.setValue(sb, 0.25f);
    const NodeHandle sa = scrollArea(ui, ui.root());
    ui.setCss(sa, "height", "40px");
    ui.setCss(sa, "width", "120px");
    const NodeHandle tall = panel(ui, sa);
    ui.setCss(tall, "height", "200px");
    ui.update();
}

void mirrorPixels() {
    UiContext l({.viewport = {260, 200}, .font = &font()}), r({.viewport = {260, 200}, .font = &font()});
    buildPlain(l, false);
    buildPlain(r, true);
    Image a, b;
    render(l, a);
    render(r, b);
    int bad = 0;
    for (int y = 0; y < a.height; ++y)
        for (int x = 0; x < a.width; ++x) {
            const u32 p = a.at(a.width - 1 - x, y), q = b.at(x, y);
            for (int c = 0; c < 4; ++c)
                if (std::abs(int(ch(p, c)) - int(ch(q, c))) > 3) { ++bad; break; }
        }
    std::printf("mirror diff: %d pixels > 3\n", bad);
    UI_CHECK(bad <= a.width * a.height / 500);
}

// ---- Tier B2 / C / D widgets, tooltips, popup panels ----------------------------------------------------------------
bool near(f32 a, f32 b, f32 tol = 0.75f) { return std::abs(a - b) <= tol; }

UiContext& rtlContext(UiContext& ui) {
    widgets::install(ui);
    ui.setCss(ui.root(), "direction", "rtl");
    return ui;
}

void splitterAndDock() {
    UiContext ui({.viewport = {600, 400}, .font = &font()});
    rtlContext(ui);
    const NodeHandle split = splitter(ui, ui.root(), false, "split");
    ui.setCss(split, "width", "300px");
    ui.setCss(split, "height", "60px");
    const NodeHandle a = splitterPane(ui, split, 80.f, "a");
    splitterPane(ui, split, 0.f, "b");
    const NodeHandle h = ui.nextSibling(a);
    const NodeHandle area = dockArea(ui, ui.root(), "area");
    ui.setCss(area, "width", "400px");
    ui.setCss(area, "height", "160px");
    const NodeHandle dock = dockWidget(ui, area, "Outline", DockSide::Left, "dock");
    ui.update();
    UI_CHECK(near(ui.rect(a).right(), ui.contentRect(split).right())); // the first pane on the right
    UI_CHECK(ui.rect(h).right() <= ui.rect(a).x + 0.5f);
    UiTestDriver d(ui);
    ui.setFocus(h, FocusReason::Keyboard);
    d.key(Key::Left); // the handle moves left: the first (right) pane grows
    UI_CHECK_EQ(ui.rect(a).w, 96.f);
    d.key(Key::Right);
    UI_CHECK_EQ(ui.rect(a).w, 80.f);
    const Rect hr = ui.rect(h);
    const Vec2 hc{hr.x + hr.w * 0.5f, hr.y + hr.h * 0.5f};
    ui.pointerMove(hc);
    ui.pointerDown(hc);
    ui.pointerMove({hc.x - 20.f, hc.y});
    ui.pointerUp({hc.x - 20.f, hc.y});
    ui.update();
    UI_CHECK_EQ(ui.rect(a).w, 100.f);
    // dock area: the Left region is on the right (as Qt's), the title's close box at its left end
    UI_CHECK(ui.rect(dock).x + ui.rect(dock).w * 0.5f > ui.rect(area).x + ui.rect(area).w * 0.5f);
    const Rect tr = ui.rect(ui.firstChild(dock));
    d.clickAt({tr.right() - 6.f, tr.y + tr.h * 0.5f}); // the title text: nothing
    UI_CHECK(!ui.hasClass(dock, "dock-closed"));
    d.clickAt({tr.x + 6.f, tr.y + tr.h * 0.5f});
    UI_CHECK(ui.hasClass(dock, "dock-closed"));
}

void tableAndGrid() {
    UiContext ui({.viewport = {700, 500}, .font = &font()});
    rtlContext(ui);
    StringTableModel model("Name\tSize");
    for (int i = 0; i < 30; ++i) model.addRow("row " + std::to_string(i) + "\t" + std::to_string(i));
    const NodeHandle t = tableView(ui, ui.root(), &model.model(), "table");
    ui.setCss(t, "width", "300px");
    ui.setCss(t, "height", "160px");
    const NodeHandle g = gridView(ui, ui.root(), &model.model(), 72.f, 56.f, "grid");
    ui.setCss(g, "width", "300px");
    ui.setCss(g, "height", "130px");
    ui.update();
    f32 sortCol = -1.f;
    int sorts = 0;
    ui.connect(t, SignalKind::Toggled, [&](UiContext&, NodeHandle, const SignalArgs& s) { sortCol = s.value; ++sorts; });
    UiTestDriver d(ui);
    const Rect c = ui.contentRect(t);
    const f32 w0 = tableColumnWidth(ui, t, 0);
    d.clickAt({c.right() - 20.f, c.y + 6.f}); // column 0 is the rightmost
    UI_CHECK(sorts == 1 && sortCol == 0.f);
    d.clickAt({c.right() - w0 - 20.f, c.y + 6.f});
    UI_CHECK(sorts == 2 && sortCol == 1.f);
    // resize column 0 from its left edge: dragging left widens it
    const f32 edge = c.right() - w0;
    ui.pointerMove({edge, c.y + 6.f});
    ui.pointerDown({edge, c.y + 6.f});
    ui.pointerMove({edge - 20.f, c.y + 6.f});
    ui.pointerUp({edge - 20.f, c.y + 6.f});
    ui.update();
    UI_CHECK_EQ(tableColumnWidth(ui, t, 0), w0 + 20.f);
    // keyboard: the resize made column 0 current; Left is the next column, Ctrl+Left widens it, Ctrl+Enter sorts
    ui.setFocus(t, FocusReason::Keyboard);
    const f32 w1 = tableColumnWidth(ui, t, 1);
    d.key(Key::Left);
    d.key(Key::Left, kModCtrl);
    UI_CHECK_EQ(tableColumnWidth(ui, t, 1), w1 + 8.f);
    d.key(Key::Enter, kModCtrl);
    UI_CHECK(sorts == 3 && sortCol == 1.f);
    // row clicks still select by row; the scroll bar is on the left
    d.clickAt({c.right() - 20.f, c.bottom() - 4.f});
    const f32 rowBefore = ui.value(t);
    UI_CHECK(rowBefore > 0.f);
    d.clickAt({c.x + 3.f, c.bottom() - 3.f}); // the bar's track: jumps towards the end (no row change)
    UI_CHECK_EQ(ui.value(t), rowBefore);
    // grid view: tiles fill from the right; Left is the next tile
    const Rect gc = ui.contentRect(g);
    d.clickAt({gc.right() - 10.f, gc.y + 10.f});
    UI_CHECK_EQ(ui.value(g), 0.f);
    d.clickAt({gc.right() - 72.f - 10.f, gc.y + 10.f});
    UI_CHECK_EQ(ui.value(g), 1.f);
    ui.setFocus(g, FocusReason::Keyboard);
    d.key(Key::Left);
    UI_CHECK_EQ(ui.value(g), 2.f);
    d.key(Key::Right);
    UI_CHECK_EQ(ui.value(g), 1.f);
}

void calendarAndChart() {
    UiContext ui({.viewport = {700, 500}, .font = &font()});
    rtlContext(ui);
    const NodeHandle cal = calendar(ui, ui.root(), 2026, 11, 5, "cal");
    const NodeHandle ch = chart(ui, ui.root(), ChartKind::Line, "chart");
    const f32 pts[] = {1.f, 3.f, 2.f, 5.f, 4.f};
    chartSetSeries(ui, ch, 0, "a", pts, Color::rgb(0x6BCB77));
    ui.update();
    UiTestDriver d(ui);
    const Rect c = ui.contentRect(cal);
    d.clickAt({c.right() - 3.f, c.bottom() - 3.f}); // the last week's first day (Monday) is on the right
    const f32 first = ui.value(cal);
    d.clickAt({c.x + 3.f, c.bottom() - 3.f});
    UI_CHECK_EQ(ui.value(cal) - first, 6.f);
    // the previous-month chevron is on the right (a day in the third week keeps the shown month: 26 px rows)
    const f32 week3 = c.bottom() - 3.5f * 26.f;
    d.clickAt({c.right() - 3.f, week3});
    const f32 mid = ui.value(cal);
    d.clickAt({c.right() - 3.f, c.y + 3.f});
    d.clickAt({c.right() - 3.f, week3});
    UI_CHECK(ui.value(cal) <= mid - 28.f && ui.value(cal) >= mid - 35.f);
    ui.setFocus(cal, FocusReason::Keyboard);
    const f32 v = ui.value(cal);
    d.key(Key::Left); // the next day (to the left)
    UI_CHECK_EQ(ui.value(cal), v + 1.f);
    // chart: the first point on the right
    const Rect cc = ui.contentRect(ch);
    ui.pointerMove({cc.right() - 41.f, cc.y + cc.h * 0.5f});
    ui.update();
    UI_CHECK_EQ(ui.value(ch), 0.f);
    ui.pointerMove({cc.x + 7.f, cc.y + cc.h * 0.5f});
    ui.update();
    UI_CHECK_EQ(ui.value(ch), 4.f);
    ui.setFocus(ch, FocusReason::Keyboard);
    d.key(Key::Home);
    d.key(Key::Left);
    UI_CHECK_EQ(ui.value(ch), 1.f);
}

void pickerAndGraph() {
    // colour picker: Left raises saturation as Right does left-to-right
    UiContext l({.viewport = {300, 260}, .font = &font()}), r({.viewport = {300, 260}, .font = &font()});
    widgets::install(l);
    rtlContext(r);
    const NodeHandle pl = colorPicker(l, l.root(), Color::rgb(0x3080C0), "p"), pr = colorPicker(r, r.root(), Color::rgb(0x3080C0), "p");
    l.update();
    r.update();
    UiTestDriver dl(l), dr(r);
    l.setFocus(pl, FocusReason::Keyboard);
    r.setFocus(pr, FocusReason::Keyboard);
    dl.key(Key::Right);
    dr.key(Key::Left);
    UI_CHECK(l.text(pl) == r.text(pr) && l.text(pl) != "#3080C0FF");
    const Rect rc = r.contentRect(pr);
    dr.clickAt({rc.x + 30.f, rc.y + 2.f}); // near the far left of the square: high saturation
    UI_CHECK(colorPickerColor(r, pr).r < 0.5f && colorPickerColor(r, pr).b > 0.9f);

    // node graph: data flows right to left; pins and drags mirror, the stored positions do not
    UiContext ui({.viewport = {500, 300}, .font = &font()});
    rtlContext(ui);
    const NodeHandle g = nodeGraph(ui, ui.root(), "graph");
    ui.setCss(g, "width", "300px");
    ui.setCss(g, "height", "160px");
    const GraphPin out[] = {{"out", 0}}, in[] = {{"in", 0}};
    nodeGraphAddNode(ui, g, "Source", {10.f, 10.f}, {}, out);
    nodeGraphAddNode(ui, g, "Sink", {170.f, 40.f}, in, {});
    ui.update();
    const Rect gc = ui.contentRect(g);
    const Vec2 po = nodeGraphPinPos(ui, g, 0, 0, true), pi = nodeGraphPinPos(ui, g, 1, 0, false);
    UI_CHECK(near(po.x, gc.right() - 160.f) && near(pi.x, gc.right() - 170.f)); // the output on the node's left
    ui.pointerMove(po);
    ui.pointerDown(po);
    ui.pointerMove(pi);
    ui.pointerUp(pi);
    ui.update();
    UI_CHECK_EQ(nodeGraphLinkCount(ui, g), 1u);
    UiTestDriver d(ui);
    ui.setFocus(g, FocusReason::Keyboard);
    d.key(Key::PageDown);
    d.key(Key::Left); // moves left on the screen
    UI_CHECK_EQ(nodeGraphNodePos(ui, g, 0).x, 20.f);
    UI_CHECK(near(nodeGraphPinPos(ui, g, 0, 0, true).x, po.x - 10.f));
}

void popupsAndIslands() {
    UiContext ui({.viewport = {700, 500}, .font = &font()});
    rtlContext(ui);
    const NodeHandle b = button(ui, ui.root(), "Hover me", "b");
    const NodeHandle tip = setTooltip(ui, b, "Tip", 0.05f);
    const NodeHandle dt = dateTimeEdit(ui, ui.root(), 2026, 11, 5, 10, 30, "dt");
    const NodeHandle code = codeEdit(ui, ui.root(), "a = 1", CodeLanguage::Lua, "code");
    const NodeHandle curve = curveEditor(ui, ui.root(), 0.f, 1.f, 0.f, 1.f, "curve");
    const NodeHandle tl = timeline(ui, ui.root(), 10.f, "tl");
    ui.update();
    // left-to-right islands: code, the ISO date-time field, time axes
    for (NodeHandle n : {dt, code, curve, tl}) UI_CHECK(!ui.computed(n).rtl);
    // the tooltip takes the node's direction and hangs from its right edge
    ui.setFocus(b, FocusReason::Keyboard);
    for (int i = 0; i < 10; ++i) ui.update(0.02f);
    UI_CHECK(ui.computed(tip).visible && ui.computed(tip).rtl);
    UI_CHECK(near(ui.rect(tip).right(), ui.rect(b).right()));
    // the date-time calendar popup (popupPanel) follows the surroundings: right-to-left, right-aligned to the field
    ui.setFocus(dt, FocusReason::Keyboard);
    UiTestDriver d(ui);
    d.key(Key::Down, kModAlt);
    ui.update();
    const NodeHandle cal = ui.focused();
    UI_CHECK(ui.alive(cal) && ui.computed(cal).rtl);
    const NodeHandle card = ui.parent(cal);
    UI_CHECK(near(ui.rect(card).right(), ui.rect(dt).right()));
}

// Paragraph direction and the line edit caret over bidi text (FULL: the bidi pass exists).
void bidiText() {
#if FUSE_UI_TEST_TEXT_FULL
    const char* heb = "\xD7\x90\xD7\x91\xD7\x92"; // alef bet gimel
    {   // `direction` set: the paragraph takes it (neutrals at the ends follow it); unset: the first strong character
        UiContext ui({.viewport = {400, 200}, .font = &font()});
        widgets::install(ui);
        const NodeHandle mixed = label(ui, ui.root(), "abc!");
        const NodeHandle h = label(ui, ui.root(), heb);
        ui.update();
        auto clusterX = [&](NodeHandle n, u32 cl) {
            for (const auto& gl : ui.textLayout(n)->glyphs)
                if (gl.cluster == cl) return gl.x;
            return -1.f;
        };
        UI_CHECK(clusterX(mixed, 3) > clusterX(mixed, 0)); // LTR (auto): "abc!"
        UI_CHECK(ui.textLayout(h)->lines[0].rtl);           // auto: Hebrew makes the paragraph RTL
        ui.setCss(ui.root(), "direction", "ltr");
        ui.update();
        UI_CHECK(!ui.textLayout(h)->lines[0].rtl);          // given: LTR
        ui.setCss(ui.root(), "direction", "rtl");
        ui.update();
        UI_CHECK(clusterX(mixed, 3) < clusterX(mixed, 0)); // given RTL: "!abc"
        UI_CHECK(ui.textLayout(h)->lines[0].rtl);
    }
    {   // the caret moves in visual order: x grows with every Right, the logical position does not
        UiContext ui({.viewport = {400, 200}, .font = &font()});
        widgets::install(ui);
        const std::string text = std::string("abc ") + heb + " def";
        const NodeHandle le = lineEdit(ui, ui.root(), text, "le");
        ui.setCss(le, "width", "300px");
        ui.setFocus(le, FocusReason::Keyboard);
        ui.update();
        UiTestDriver d(ui);
        d.key(Key::Home);
        f32 x = ui.textCaret().x;
        bool increasing = true, logicalBack = false;
        std::size_t prev = ui.editBuffer(le).cursor();
        for (int i = 0; i < 24 && prev != text.size(); ++i) {
            d.key(Key::Right);
            increasing = increasing && ui.textCaret().x > x;
            logicalBack = logicalBack || ui.editBuffer(le).cursor() < prev;
            x = ui.textCaret().x;
            prev = ui.editBuffer(le).cursor();
        }
        UI_CHECK(increasing);
        UI_CHECK(logicalBack); // inside the Hebrew run the logical position goes down while the caret goes right
        UI_CHECK_EQ(prev, text.size()); // ends at the visual right end = the logical end
        d.key(Key::Right);
        UI_CHECK_EQ(ui.editBuffer(le).cursor(), text.size());
        // a click lands on the nearest stop: the caret is drawn where the click was
        const Rect c = ui.contentRect(le);
        d.clickAt({c.x + 60.f, c.y + c.h * 0.5f});
        UI_CHECK(std::abs(ui.textCaret().x - (c.x + 60.f)) < 6.f);
    }
    {   // right-to-left field: Left moves the caret left (forwards through Hebrew)
        UiContext ui({.viewport = {400, 200}, .font = &font()});
        rtlContext(ui);
        const std::string text = std::string(heb) + " abc";
        const NodeHandle le = lineEdit(ui, ui.root(), text, "le");
        ui.setCss(le, "width", "300px");
        ui.setFocus(le, FocusReason::Keyboard);
        ui.update();
        UiTestDriver d(ui);
        d.key(Key::Home);
        f32 x = ui.textCaret().x;
        bool decreasing = true;
        for (int i = 0; i < 3; ++i) {
            d.key(Key::Left);
            decreasing = decreasing && ui.textCaret().x < x;
            x = ui.textCaret().x;
        }
        UI_CHECK(decreasing);
        UI_CHECK_EQ(ui.editBuffer(le).cursor(), 6u); // three Hebrew letters forwards
    }
#endif
}

// Text-free Tier B2 / C / D widgets: the RTL frame is the LTR frame mirrored.
void buildPlainB2CD(UiContext& ui, bool rtl, const ItemModel* empty) {
    widgets::install(ui);
    if (rtl) ui.setCss(ui.root(), "direction", "rtl");
    ui.setCss(ui.root(), "padding", "10px 14px 10px 14px");
    ui.setCss(ui.root(), "gap", "8px");
    colorPicker(ui, ui.root(), Color::rgb(0x3080C0));
    const NodeHandle split = splitter(ui, ui.root(), false);
    ui.setCss(split, "height", "30px");
    ui.setCss(split, "width", "200px");
    splitterPane(ui, split, 60.f);
    splitterPane(ui, split, 0.f);
    const NodeHandle g = nodeGraph(ui, ui.root());
    ui.setCss(g, "width", "260px");
    ui.setCss(g, "height", "110px");
    const GraphPin out[] = {{"", 1}, {"", 2}}, in[] = {{"", 1}, {"", 2}};
    nodeGraphAddNode(ui, g, "", {6.f, 6.f}, {}, out);
    nodeGraphAddNode(ui, g, "", {130.f, 30.f}, in, {});
    nodeGraphConnect(ui, g, 0, 0, 1, 1);
    nodeGraphConnect(ui, g, 0, 1, 1, 0);
    const NodeHandle gv = gridView(ui, ui.root(), empty, 50.f, 40.f);
    ui.setCss(gv, "height", "44px");
    ui.setCss(gv, "width", "220px");
    ui.update();
}

void mirrorPixelsB2CD() {
    StringTableModel empty("");
    for (int i = 0; i < 3; ++i) empty.addRow("");
    UiContext l({.viewport = {300, 560}, .font = &font()}), r({.viewport = {300, 560}, .font = &font()});
    buildPlainB2CD(l, false, &empty.model());
    buildPlainB2CD(r, true, &empty.model());
    Image a, b;
    render(l, a);
    render(r, b);
    int bad = 0;
    for (int y = 0; y < a.height; ++y)
        for (int x = 0; x < a.width; ++x) {
            const u32 p = a.at(a.width - 1 - x, y), q = b.at(x, y);
            for (int c = 0; c < 4; ++c)
                if (std::abs(int(ch(p, c)) - int(ch(q, c))) > 3) { ++bad; break; }
        }
    std::printf("tier B2 / C / D mirror diff: %d pixels > 3\n", bad);
    UI_CHECK(bad <= a.width * a.height / 500);
}

// The Tier A sample in RTL plus a menu bar, tab bar, tree and a scroll area.
void golden(const std::string& path) {
    UiContext ui({.viewport = {488, 560}, .font = &font()});
    widgets::install(ui);
    ui.setCss(ui.root(), "direction", "rtl");
    buildSample(ui);
    const NodeHandle card = ui.find("card");
    const NodeHandle bar = menuBar(ui, card);
    menuBarMenu(ui, bar, "File", "Open");
    menuBarMenu(ui, bar, "Edit", "Undo");
    menuBarMenu(ui, bar, "View", "Zoom");
    ui.moveTo(bar, card, ui.firstChild(card));
    tabBar(ui, card, "General\nAdvanced\nAbout", 1);
    const NodeHandle row = panel(ui, card);
    ui.addClass(row, "row");
    const NodeHandle tree = treeView(ui, row, "Scene\n\tCamera\n\tLights\n\t\tSun\nAssets");
    ui.setCss(tree, "height", "90px");
    ui.setValue(tree, 1.f);
    const NodeHandle sa = scrollArea(ui, row);
    ui.setCss(sa, "height", "90px");
    ui.setCss(sa, "flex-grow", "1");
    for (const char* t : {"Alpha", "Beta", "Gamma", "Delta", "Epsilon", "Zeta", "Eta", "Theta"}) label(ui, sa, t);
    ui.setFocus(ui.find("name"), FocusReason::Keyboard); // caret at the end of the line edit
    ui.update();
    ui.update();
    Image img;
    render(ui, img);
    if (std::getenv("FUSE_UI_UPDATE_GOLDEN")) {
        stbi_write_png(path.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("golden written: %s\n", path.c_str());
        return;
    }
    int w = 0, h = 0, n = 0;
    unsigned char* ref = stbi_load(path.c_str(), &w, &h, &n, 4);
    UI_CHECK(ref != nullptr);
    if (!ref) return;
    UI_CHECK_EQ(w, img.width);
    UI_CHECK_EQ(h, img.height);
    if (w == img.width && h == img.height) {
        int bad = 0, maxd = 0;
        for (int i = 0; i < w * h; ++i)
            for (int c = 0; c < 4; ++c) {
                const int d = std::abs(int(ref[i * 4 + c]) - int(ch(img.pixels[std::size_t(i)], c)));
                maxd = std::max(maxd, d);
                if (d > 3) { ++bad; break; }
            }
        std::printf("golden diff: %d pixels > 3, max channel diff %d\n", bad, maxd);
        UI_CHECK(bad <= w * h / 1000);
    }
    stbi_image_free(ref);
}

} // namespace

int main(int argc, char** argv) {
    boxRow();
    gridAndFlow();
    textAlign();
    sliderAndTabs();
    treeKeys();
    scrollContainer();
    menus();
    mirrorPixels();
    splitterAndDock();
    tableAndGrid();
    calendarAndChart();
    pickerAndGraph();
    popupsAndIslands();
    bidiText();
    mirrorPixelsB2CD();
#if FUSE_UI_TEST_TEXT_FULL
    golden(argc > 1 ? argv[1] : "golden/rtl_layout_full.png"); // FreeType coverage differs from stb_truetype
#else
    golden(argc > 1 ? argv[1] : "golden/rtl_layout.png");
#endif
    return finish("fuse_ui_rtl_layout");
}
