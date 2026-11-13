// fuse_ui_tab_options: QTabBar / QTabWidget options on main's tab bar (widgets/edit_options.hpp): closable tabs
// (tabCloseRequested), movable tabs (live reorder, pages follow), disabled tabs, scroll buttons on overflow and
// shrink-to-fit, wheel / Home / End, add / remove pages, accessibility of the tabs.
// `--gate=<name>` runs one group; `--out=<png>` writes the scene as an image.
#include "test_util.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_write_impl.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
bool near(f32 a, f32 b, f32 eps = 0.75f) { return std::abs(a - b) <= eps; }
void click(UiContext& ui, Vec2 p) {
    ui.pointerMove(p);
    ui.pointerDown(p);
    ui.pointerUp(p);
    ui.update(1.f / 60.f);
}
void key(UiContext& ui, Key k) {
    ui.keyDown(k);
    ui.keyUp(k);
    ui.update(1.f / 60.f);
}
struct Fixture {
    UiContext ui{{.viewport = {520, 300}, .font = &font()}};
    Fixture() {
        w::install(ui);
        ui.setCss(ui.root(), "padding", "10px");
    }
};
struct Log {
    std::vector<std::string> events;
    std::vector<int> values, values2, current;
};
Log& log() {
    static Log l;
    return l;
}
void listen(UiContext& ui, NodeHandle bar) {
    log() = {};
    ui.connect(bar, SignalKind::Event, [](UiContext&, NodeHandle, const SignalArgs& a) {
        if (a.text.empty()) return; // (the core offers unconsumed input events here too)
        log().events.emplace_back(a.text);
        log().values.push_back(int(a.value));
        log().values2.push_back(int(a.value2));
    });
    ui.connect(bar, SignalKind::ValueChanged, [](UiContext&, NodeHandle, const SignalArgs& a) { log().current.push_back(int(a.value)); });
}
std::vector<A11ySnapshotNode> tabItems(UiContext& ui, NodeHandle bar) {
    std::vector<A11ySnapshotNode> snap, out;
    ui.update();
    ui.snapshotA11y(snap);
    for (auto& s : snap)
        if (s.node == bar && s.child != 0) out.push_back(s);
    return out;
}

// ---- closable, disabled, wheel / Home / End -------------------------------------------------------------------------
void gateClosable() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle bar = w::tabBar(ui, ui.root(), "One\nTwo\nThree\nFour", 0, "bar");
    ui.update();
    // without options: the defaults, and the plain geometry
    UI_CHECK(!w::tabsClosable(ui, bar) && !w::tabsMovable(ui, bar) && w::tabEnabled(ui, bar, 2) && w::tabUsesScrollButtons(ui, bar));
    const Rect plain1 = w::tabBarTabRect(ui, bar, 1);
    UI_CHECK(plain1.w > 20.f && near(plain1.x, w::tabBarTabRect(ui, bar, 0).right()));
    const f32 plainW = ui.rect(bar).w;
    listen(ui, bar);
    w::setTabsClosable(ui, bar, true);
    ui.update();
    UI_CHECK(w::tabsClosable(ui, bar));
    const Rect r1 = w::tabBarTabRect(ui, bar, 1);
    UI_CHECK(near(r1.w, plain1.w + 18.f) && r1.x > plain1.x); // every tab grew by the close box
    UI_CHECK(w::tabBarTabRect(ui, bar, 9).empty());
    (void)plainW;
    // a click on the title selects, a click on the close box asks to close (and does not select)
    click(ui, {r1.x + 14.f, r1.y + r1.h * 0.5f});
    UI_CHECK(u32(ui.value(bar)) == 1 && log().events.empty() && log().current == std::vector<int>{1});
    const Rect r2 = w::tabBarTabRect(ui, bar, 2);
    click(ui, {r2.right() - 13.f, r2.y + r2.h * 0.5f});
    UI_CHECK(u32(ui.value(bar)) == 1);
    UI_CHECK(log().events == std::vector<std::string>{"tabCloseRequested"} && log().values == std::vector<int>{2});
    // disabled tab: not clickable, skipped by the keys and the wheel, no close request
    w::setTabEnabled(ui, bar, 2, false);
    ui.update();
    UI_CHECK(!w::tabEnabled(ui, bar, 2) && w::tabEnabled(ui, bar, 1));
    click(ui, ctr(w::tabBarTabRect(ui, bar, 2)));
    click(ui, {r2.right() - 13.f, r2.y + r2.h * 0.5f});
    UI_CHECK(u32(ui.value(bar)) == 1 && log().events.size() == 1);
    ui.setFocus(bar);
    key(ui, Key::Right);
    UI_CHECK(u32(ui.value(bar)) == 3); // over the disabled tab
    key(ui, Key::Right);
    UI_CHECK(u32(ui.value(bar)) == 3);
    key(ui, Key::Left);
    UI_CHECK(u32(ui.value(bar)) == 1);
    key(ui, Key::End);
    UI_CHECK(u32(ui.value(bar)) == 3);
    key(ui, Key::Home);
    UI_CHECK(u32(ui.value(bar)) == 0);
    ui.wheel(ctr(ui.rect(bar)), {0.f, -1.f});
    ui.wheel(ctr(ui.rect(bar)), {0.f, -1.f});
    UI_CHECK(u32(ui.value(bar)) == 3); // 0 -> 1 -> (2 disabled) 3
    ui.wheel(ctr(ui.rect(bar)), {0.f, 1.f});
    UI_CHECK(u32(ui.value(bar)) == 1);
    // disabling the current tab moves to the next enabled one
    w::setTabEnabled(ui, bar, 1, false);
    UI_CHECK(u32(ui.value(bar)) == 3 && log().current.back() == 3);
    w::setTabEnabled(ui, bar, 3, false); // nothing after it: the previous enabled one
    UI_CHECK(u32(ui.value(bar)) == 0);
    // accessibility: four tabs with their real bounds; disabled ones say so and refuse Select
    const std::vector<A11ySnapshotNode> items = tabItems(ui, bar);
    UI_CHECK(items.size() == 4);
    if (items.size() == 4) {
        UI_CHECK(items[0].role == A11yRole::Tab && items[0].name == "One" && items[0].selected && !items[0].disabled);
        UI_CHECK(items[2].disabled && !items[2].hasAction(A11yAction::Select) && items[0].hasAction(A11yAction::Select));
        const Rect b = w::tabBarTabRect(ui, bar, 2);
        UI_CHECK(near(items[2].bounds.x, b.x) && near(items[2].bounds.w, b.w));
    }
    UI_CHECK(!ui.performA11yAction(bar, A11yAction::Select, 0.f, 3) && u32(ui.value(bar)) == 0);
    w::setTabEnabled(ui, bar, 1, true);
    UI_CHECK(ui.performA11yAction(bar, A11yAction::Select, 0.f, 2) && u32(ui.value(bar)) == 1);
    // not a tab bar: ignored
    const NodeHandle lbl = w::label(ui, ui.root(), "x");
    w::setTabsClosable(ui, lbl, true);
    UI_CHECK(!w::tabsClosable(ui, lbl) && w::tabBarTabRect(ui, lbl, 0).empty());
}

// ---- movable: live reorder, pages follow -----------------------------------------------------------------------------
void gateMovable() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle tabs = w::tabWidget(ui, ui.root(), "Alpha\nBeta\nGamma", "tabs");
    const NodeHandle bar = w::tabWidgetBar(ui, tabs);
    NodeHandle pages[3];
    for (u32 i = 0; i < 3; ++i) {
        pages[i] = w::tabPage(ui, tabs, i);
        w::label(ui, pages[i], i == 0 ? "page alpha" : i == 1 ? "page beta" : "page gamma");
    }
    listen(ui, bar);
    w::setTabsMovable(ui, bar, true);
    w::setTabEnabled(ui, bar, 2, false);
    ui.update();
    UI_CHECK(w::tabsMovable(ui, bar));
    // drag Alpha past the middle of Beta: they swap, Alpha stays current, its page follows
    const Rect a = w::tabBarTabRect(ui, bar, 0), b = w::tabBarTabRect(ui, bar, 1);
    ui.pointerMove(ctr(a));
    ui.pointerDown(ctr(a));
    ui.pointerMove({ctr(a).x + 6.f, ctr(a).y}); // inside the start distance: nothing moves
    ui.update();
    UI_CHECK(ui.text(bar) == "Alpha\nBeta\nGamma" && log().events.empty());
    ui.pointerMove({b.x + b.w * 0.5f + 4.f, ctr(a).y});
    ui.update();
    UI_CHECK(ui.text(bar) == "Beta\nAlpha\nGamma" && u32(ui.value(bar)) == 1);
    UI_CHECK(log().events == std::vector<std::string>{"tabMoved"} && log().values == std::vector<int>{1} && log().values2 == std::vector<int>{0});
    UI_CHECK(w::tabPage(ui, tabs, 1) == pages[0] && w::tabPage(ui, tabs, 0) == pages[1] && w::tabPage(ui, tabs, 2) == pages[2]);
    UI_CHECK(ui.computed(pages[0]).visible && !ui.computed(pages[1]).visible);
    // on past Gamma (a disabled tab still gives way), then release
    const Rect g = w::tabBarTabRect(ui, bar, 2);
    ui.pointerMove({g.x + g.w * 0.5f + 4.f, ctr(a).y});
    ui.update();
    UI_CHECK(ui.text(bar) == "Beta\nGamma\nAlpha" && u32(ui.value(bar)) == 2 && !w::tabEnabled(ui, bar, 1) && w::tabEnabled(ui, bar, 2));
    ui.pointerUp({g.x + g.w * 0.5f + 4.f, ctr(a).y});
    ui.update();
    UI_CHECK(w::tabPage(ui, tabs, 2) == pages[0] && ui.computed(pages[0]).visible);
    UI_CHECK(log().current.empty()); // the current tab never changed, only its index
    // after the release a move without a press does nothing
    ui.pointerMove(ctr(w::tabBarTabRect(ui, bar, 0)));
    ui.update();
    UI_CHECK(ui.text(bar) == "Beta\nGamma\nAlpha" && log().events.size() == 2);
    // programmatic move; out-of-range / no-op moves are ignored
    w::tabBarMoveTab(ui, bar, 2, 0);
    UI_CHECK(ui.text(bar) == "Alpha\nBeta\nGamma" && u32(ui.value(bar)) == 0 && w::tabPage(ui, tabs, 0) == pages[0]);
    UI_CHECK(!w::tabEnabled(ui, bar, 2) && log().events.size() == 3 && log().values.back() == 0 && log().values2.back() == 2);
    w::tabBarMoveTab(ui, bar, 1, 1);
    w::tabBarMoveTab(ui, bar, 7, 0);
    UI_CHECK(log().events.size() == 3);
    // a bar that is not movable does not reorder on a drag
    w::setTabsMovable(ui, bar, false);
    ui.update();
    const Rect a2 = w::tabBarTabRect(ui, bar, 0), b2 = w::tabBarTabRect(ui, bar, 1);
    ui.pointerMove(ctr(a2));
    ui.pointerDown(ctr(a2));
    ui.pointerMove({b2.right() - 2.f, ctr(a2).y});
    ui.pointerUp({b2.right() - 2.f, ctr(a2).y});
    ui.update();
    UI_CHECK(ui.text(bar) == "Alpha\nBeta\nGamma");
    // tabBarMoveTab also works on a plain tab bar (no options)
    const NodeHandle plain = w::tabBar(ui, ui.root(), "x\ny\nz", 2, "plain");
    w::tabBarMoveTab(ui, plain, 2, 0);
    UI_CHECK(ui.text(plain) == "z\nx\ny" && u32(ui.value(plain)) == 0);
}

// ---- overflow: scroll buttons, current tab kept in view, shrink-to-fit ---------------------------------------------
void gateScroll() {
    Fixture f;
    UiContext& ui = f.ui;
    std::string titles;
    for (int i = 0; i < 12; ++i) titles += (i ? "\nDocument " : "Document ") + std::to_string(i);
    const NodeHandle bar = w::tabBar(ui, ui.root(), titles, 0, "bar");
    ui.setCss(bar, "width", "400px");
    w::setTabsClosable(ui, bar, true);
    ui.update();
    const Rect c = ui.contentRect(bar);
    UI_CHECK(near(c.w, 400.f, 1.f));
    UI_CHECK(w::tabBarScroll(ui, bar) == 0.f);
    const Rect last = w::tabBarTabRect(ui, bar, 11);
    UI_CHECK(last.x > c.right()); // far outside the view
    // the right scroll button (the last 18 px) scrolls by 80 px, the left one back
    click(ui, {c.right() - 9.f, c.y + c.h * 0.5f});
    UI_CHECK(near(w::tabBarScroll(ui, bar), 80.f) && u32(ui.value(bar)) == 0);
    UI_CHECK(near(w::tabBarTabRect(ui, bar, 11).x, last.x - 80.f));
    click(ui, {c.right() - 27.f, c.y + c.h * 0.5f});
    UI_CHECK(w::tabBarScroll(ui, bar) == 0.f);
    click(ui, {c.right() - 27.f, c.y + c.h * 0.5f}); // already at the start
    UI_CHECK(w::tabBarScroll(ui, bar) == 0.f);
    // selecting a tab out of view scrolls it in, fully left of the buttons
    ui.setValue(bar, 11.f, true);
    ui.update();
    const Rect in = w::tabBarTabRect(ui, bar, 11);
    UI_CHECK(w::tabBarScroll(ui, bar) > 100.f && in.x >= c.x - 0.5f && in.right() <= c.right() - 36.f + 0.75f);
    const f32 maxScroll = w::tabBarScroll(ui, bar);
    click(ui, {c.right() - 9.f, c.y + c.h * 0.5f}); // at the end already
    UI_CHECK(near(w::tabBarScroll(ui, bar), maxScroll));
    ui.setFocus(bar);
    key(ui, Key::Home);
    UI_CHECK(u32(ui.value(bar)) == 0 && w::tabBarScroll(ui, bar) == 0.f);
    // the area under the buttons belongs to them: a tab scrolled under it is not picked
    const u32 before = u32(ui.value(bar));
    click(ui, {c.right() - 20.f, c.y + c.h * 0.5f});
    UI_CHECK(u32(ui.value(bar)) == before);
    // a11y bounds follow the scroll
    click(ui, {c.right() - 9.f, c.y + c.h * 0.5f});
    const auto items = tabItems(ui, bar);
    UI_CHECK(items.size() == 12 && near(items[3].bounds.x, w::tabBarTabRect(ui, bar, 3).x));
    // no scroll buttons: the tabs shrink into the bar instead (each keeps its close box and an ellipsis), nothing scrolls
    const NodeHandle fit = w::tabBar(ui, ui.root(), "Long document 0\nLong document 1\nLong document 2\nLong document 3\nLong document 4", 0, "fit");
    ui.setCss(fit, "width", "320px");
    w::setTabsClosable(ui, fit, true);
    ui.update();
    const f32 natural = w::tabBarTabRect(ui, fit, 0).w;
    UI_CHECK(natural * 5.f > 330.f); // they do overflow
    w::setTabUsesScrollButtons(ui, fit, false);
    ui.update();
    UI_CHECK(!w::tabUsesScrollButtons(ui, fit) && w::tabBarScroll(ui, fit) == 0.f);
    const Rect fc = ui.contentRect(fit), s0 = w::tabBarTabRect(ui, fit, 0), s4 = w::tabBarTabRect(ui, fit, 4);
    UI_CHECK(near(s0.x, fc.x) && near(s4.right(), fc.right(), 1.5f) && s0.w < natural && s0.w >= 54.f);
    click(ui, {s4.x + 14.f, s4.y + s4.h * 0.5f}); // the far end is a tab, not a scroll button
    UI_CHECK(u32(ui.value(fit)) == 4 && w::tabBarScroll(ui, fit) == 0.f);
    ui.setValue(fit, 0.f, true);
    ui.update();
    UI_CHECK(w::tabBarScroll(ui, fit) == 0.f);
    // back to scroll buttons: natural widths again
    w::setTabUsesScrollButtons(ui, fit, true);
    ui.update();
    UI_CHECK(near(w::tabBarTabRect(ui, fit, 0).w, natural));
}

// ---- tab widget pages: add / remove -----------------------------------------------------------------------------------
void gatePages() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle tabs = w::tabWidget(ui, ui.root(), "A\nB", "tabs");
    const NodeHandle bar = w::tabWidgetBar(ui, tabs);
    listen(ui, bar);
    const NodeHandle pa = w::tabPage(ui, tabs, 0), pb = w::tabPage(ui, tabs, 1);
    const NodeHandle pc = w::tabWidgetAddPage(ui, tabs, "C");
    const NodeHandle pd = w::tabWidgetAddPage(ui, tabs, "D");
    ui.update();
    UI_CHECK(pc.valid() && pd.valid() && ui.text(bar) == "A\nB\nC\nD" && w::tabPage(ui, tabs, 2) == pc && w::tabPage(ui, tabs, 3) == pd);
    UI_CHECK(u32(ui.value(bar)) == 0 && ui.computed(pa).visible && !ui.computed(pc).visible && ui.rangeMax(bar) == 3.f);
    ui.setFocus(bar);
    key(ui, Key::Right);
    key(ui, Key::Right);
    key(ui, Key::Right); // reaches the added tabs
    UI_CHECK(u32(ui.value(bar)) == 3 && ui.computed(pd).visible && !ui.computed(pa).visible);
    // removing a tab before the current one: the same page stays current, no ValueChanged
    const std::size_t changes = log().current.size();
    w::tabWidgetRemovePage(ui, tabs, 1);
    ui.update();
    UI_CHECK(!ui.alive(pb) && ui.text(bar) == "A\nC\nD" && u32(ui.value(bar)) == 2 && ui.computed(pd).visible);
    UI_CHECK(log().current.size() == changes);
    // removing the current (last) tab: the previous one shows, ValueChanged
    w::tabWidgetRemovePage(ui, tabs, 2);
    ui.update();
    UI_CHECK(!ui.alive(pd) && ui.text(bar) == "A\nC" && u32(ui.value(bar)) == 1 && ui.computed(pc).visible);
    UI_CHECK(log().current.size() == changes + 1 && log().current.back() == 1);
    // removing the current first tab: the next one takes its index
    ui.setValue(bar, 0.f, true);
    w::tabWidgetRemovePage(ui, tabs, 0);
    ui.update();
    UI_CHECK(!ui.alive(pa) && ui.text(bar) == "C" && u32(ui.value(bar)) == 0 && ui.computed(pc).visible);
    w::tabWidgetRemovePage(ui, tabs, 5); // out of range: ignored
    UI_CHECK(ui.text(bar) == "C");
    w::tabWidgetRemovePage(ui, tabs, 0);
    ui.update();
    UI_CHECK(ui.text(bar).empty() && !ui.alive(pc) && !w::tabPage(ui, tabs, 0).valid());
    // a page added to the empty widget becomes current and shows
    const NodeHandle pe = w::tabWidgetAddPage(ui, tabs, "E");
    ui.update();
    UI_CHECK(ui.text(bar) == "E" && u32(ui.value(bar)) == 0 && ui.computed(pe).visible);
    // the closable round trip: the close request removes the page
    w::setTabsClosable(ui, bar, true);
    w::tabWidgetAddPage(ui, tabs, "F");
    ui.connect(bar, SignalKind::Event, [tabs](UiContext& u, NodeHandle, const SignalArgs& a) {
        if (a.text == "tabCloseRequested") w::tabWidgetRemovePage(u, tabs, u32(a.value));
    });
    ui.update();
    const Rect rf = w::tabBarTabRect(ui, bar, 1);
    click(ui, {rf.right() - 13.f, rf.y + rf.h * 0.5f});
    UI_CHECK(ui.text(bar) == "E" && ui.computed(pe).visible);
    // not a tab widget
    UI_CHECK(!w::tabWidgetAddPage(ui, bar, "x").valid());
}

NodeHandle buildScene(UiContext& ui) {
    const NodeHandle tabs = w::tabWidget(ui, ui.root(), "main.cpp\nwidgets.hpp\nREADME.md\ntheme.css\nnotes.txt\nCMakeLists.txt\nui.cpp", "tabs");
    const NodeHandle bar = w::tabWidgetBar(ui, tabs);
    ui.setCss(tabs, "width", "480px");
    w::setTabsClosable(ui, bar, true);
    w::setTabsMovable(ui, bar, true);
    w::setTabEnabled(ui, bar, 2, false);
    w::label(ui, w::tabPage(ui, tabs, 1), "closable, movable tabs; README.md is disabled; the rest scrolls");
    ui.setValue(bar, 1.f, true);
    const NodeHandle fit = w::tabBar(ui, ui.root(), "Overview\nPerformance\nMemory\nNetwork\nStorage\nSecurity\nAccessibility", 3, "fit");
    ui.setCss(fit, "width", "480px");
    w::setTabUsesScrollButtons(ui, fit, false);
    return tabs;
}
int demo(const char* out) {
    Fixture f;
    buildScene(f.ui);
    f.ui.update();
    Image img;
    render(f.ui, img);
    return stbi_write_png(out, img.width, img.height, 4, img.pixels.data(), img.width * 4) ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    struct Gate { const char* name; void (*fn)(); };
    static const Gate kGates[] = {{"closable", &gateClosable}, {"movable", &gateMovable}, {"scroll", &gateScroll}, {"pages", &gatePages}};
    const char* only = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--gate=", 7) == 0) only = argv[i] + 7;
        else if (std::strncmp(argv[i], "--out=", 6) == 0) return demo(argv[i] + 6);
    }
    bool ran = false;
    for (const Gate& g : kGates)
        if (!only || std::strcmp(only, g.name) == 0) {
            g.fn();
            ran = true;
        }
    if (!ran) {
        std::fprintf(stderr, "unknown gate %s\n", only);
        return 2;
    }
    return finish("fuse_ui_tab_options");
}
