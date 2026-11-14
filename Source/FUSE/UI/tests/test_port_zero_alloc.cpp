// Gate fuse_ui_port_zero_alloc: the ported widgets and options keep the house rule of fuse_ui_zero_alloc_steady -
// after warm-up, frames with hover / press / value / focus changes, caret moves and repaints perform zero heap
// allocations (global operator new is counted). Edits of text (typing, paste) are not steady state and not covered. Covered: line edits with options (placeholder, password mask, input mask), spin
// boxes with options, an editable combo box, tab bars with options (closable, scrolled, shrunk), LCD number, key
// sequence edit, tool box, MDI area (a sub-window dragged), graphics view (items moved, a rubber band dragged,
// ellipse / polygon paths), the on-screen keyboard, split docks (a split handle moved), a timeline clip dragged.
#include "test_alloc_hooks.hpp"
#include "test_util.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#include <atomic>
#include <new>

namespace {
std::atomic<long> g_allocs{0};
bool g_counting = false;
} // namespace

// Every global allocation form is replaced and counted (test_alloc_hooks.hpp).
void fuse::ui::test::countAlloc(std::size_t) noexcept {
    if (g_counting) g_allocs.fetch_add(1, std::memory_order_relaxed);
}

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
/// Allocations per step of a frame (reported when the gate fails, to find the widget that allocates).
struct Step { const char* name; long count; };
Step g_steps[16];
int g_stepCount = 0;
long g_mark = 0;
void step(const char* name) {
    const long now = g_allocs.load();
    int k = 0;
    while (k < g_stepCount && g_steps[k].name != name) ++k;
    if (k == g_stepCount && g_stepCount < 16) g_steps[g_stepCount++] = {name, 0};
    if (k < 16) g_steps[k].count += now - g_mark;
    g_mark = now;
}
} // namespace

int main() {
    UiContext ui({.viewport = {1000, 900}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "8px");
    ui.setCss(ui.root(), "gap", "6px");
    // options
    const NodeHandle pw = w::lineEdit(ui, ui.root(), "", "pw");
    w::setLineEditOptions(ui, pw, {.echo = w::EchoMode::Password, .placeholder = "a placeholder that is longer than a small string"});
    w::setLineEditText(ui, pw, "a password longer than the small string buffer");
    const NodeHandle mask = w::lineEdit(ui, ui.root(), "", "mask");
    w::setLineEditOptions(ui, mask, {.inputMask = "000.000.000.000;_"});
    const NodeHandle plainOpt = w::lineEdit(ui, ui.root(), "", "plainopt");
    w::setLineEditOptions(ui, plainOpt, {.placeholder = "placeholder shown while the field is empty, also long"});
    const NodeHandle spin = w::spinBox(ui, ui.root(), 0.f, 1000.f, 20.f, 1.f, "spin");
    w::setSpinBoxOptions(ui, spin, {.prefix = "a long prefix: $ ", .suffix = " each, a long suffix", .specialValueText = "nothing at all, free of charge"});
    const NodeHandle combo = w::editableComboBox(ui, ui.root(), "Red\nGreen\nBlue", 0, w::InsertPolicy::NoInsert, "combo");
    const NodeHandle tabs = w::tabBar(ui, ui.root(), "main.cpp\nwidgets.hpp\nREADME.md\ntheme.css", 0, "tabs");
    w::setTabsClosable(ui, tabs, true);
    w::setTabsMovable(ui, tabs, true);
    std::string many;
    for (int i = 0; i < 20; ++i) many += (i ? "\nDocument " : "Document ") + std::to_string(i);
    const NodeHandle scroll = w::tabBar(ui, ui.root(), many, 0, "scroll");
    w::setTabsClosable(ui, scroll, true);
    const NodeHandle fit = w::tabBar(ui, ui.root(), many, 0, "fit");
    w::setTabUsesScrollButtons(ui, fit, false);
    // extras
    const NodeHandle lcd = w::lcdNumber(ui, ui.root(), 8, "lcd");
    const NodeHandle keys = w::keySequenceEdit(ui, ui.root(), Shortcut{Key::S, kModCtrl | kModShift}, "keys");
    const NodeHandle box = w::toolBox(ui, ui.root(), "General\nAdvanced", "box");
    w::label(ui, w::toolBoxPage(ui, box, 0), "page 0");
    w::label(ui, w::toolBoxPage(ui, box, 1), "page 1");
    const NodeHandle row = w::panel(ui, ui.root());
    ui.addClass(row, "row");
    const NodeHandle area = w::mdiArea(ui, row, "area");
    ui.setCss(area, "height", "180px");
    ui.setCss(area, "width", "400px");
    const NodeHandle sub = w::mdiSubWindow(ui, area, "Document", {10, 10, 200, 120}, "sub");
    w::label(ui, w::mdiBody(ui, sub), "body");
    static w::GraphicsScene scene;
    const u32 rectItem = scene.add({.rect = {10, 10, 60, 40}, .text = "box"});
    scene.add({.shape = w::GraphicsShape::Ellipse, .rect = {100, 10, 90, 40}, .fill = Color::rgb(0x81C784), .z = 2});
    scene.add({.shape = w::GraphicsShape::Polygon, .z = 1, .points = {{220, 10}, {300, 30}, {250, 90}}});
    scene.add({.shape = w::GraphicsShape::Line, .rect = {10, 100, 150, 30}});
    const NodeHandle view = w::graphicsView(ui, row, &scene, "view");
    ui.setCss(view, "height", "180px");
    const NodeHandle kb = w::virtualKeyboard(ui, ui.root(), "kb");
    const NodeHandle tl = w::timeline(ui, ui.root(), 10.f, "tl");
    const u32 track = w::timelineAddTrack(ui, tl, "pos");
    w::timelineAddClip(ui, tl, track, 1.f, 3.f, "clip", Color::rgb(0x64B5F6));
    w::timelineSetView(ui, tl, 0.f, 10.f);
    const NodeHandle dockArea = w::dockArea(ui, ui.root(), "docks");
    ui.setCss(dockArea, "height", "160px");
    const NodeHandle d0 = w::dockWidget(ui, dockArea, "Outline", w::DockSide::Left, "d0");
    const NodeHandle d1 = w::dockWidget(ui, dockArea, "Assets", w::DockSide::Left, "d1");
    w::label(ui, w::dockBody(ui, d0), "outline");
    w::label(ui, w::dockBody(ui, d1), "assets");
    w::dockSplit(ui, d1, d0, w::DockEdge::Bottom);
    ui.update();
    const NodeHandle handle = ui.nextSibling(ui.parent(d0));
    UI_CHECK(ui.classOf(handle) == ui.registry().find("splithandle"));

    auto frame = [&](int i) {
        const bool odd = i % 2;
        // line edits: caret moves, focus changes (password mask <-> placeholder repaint), typed characters replaced
        ui.setFocus(pw, FocusReason::Keyboard);
        ui.keyDown(odd ? Key::Home : Key::End);
        ui.setFocus(mask, FocusReason::Keyboard);
        ui.keyDown(odd ? Key::Home : Key::End);
        ui.keyDown(Key::Right, kModShift); // (typing is an edit: it records undo state, as in every text widget)
        ui.setFocus(plainOpt, FocusReason::Keyboard);
        step("line edits");
        // spin box with options: value, special value, focus (selection) and a step
        ui.setValue(spin, odd ? 0.f : 250.f);
        ui.setFocus(spin, FocusReason::Keyboard);
        ui.keyDown(Key::Up);
        step("spin box");
        // editable combo: step the current item with the keyboard
        ui.setFocus(w::comboLineEdit(ui, combo), FocusReason::Keyboard);
        ui.keyDown(odd ? Key::Down : Key::Up);
        step("editable combo");
        // tab bars: selection (scrolls the overflowing one), hover over a close box
        ui.setValue(tabs, f32(i % 4), true);
        ui.setValue(scroll, f32(odd ? 17 : 2), true);
        ui.setValue(fit, f32(i % 20), true);
        ui.pointerMove({ui.rect(tabs).x + 60.f + f32(i % 3), ui.rect(tabs).y + 8.f});
        step("tab bars");
        // LCD value / key sequence
        ui.setValue(lcd, f32(1000 + i * 37));
        w::setKeySequence(ui, keys, Shortcut{odd ? Key::S : Key::O, kModCtrl});
        w::setToolBoxIndex(ui, box, u32(i % 2));
        step("lcd / key sequence / tool box");
        // MDI: drag the sub-window by its title
        const Rect title = ui.rect(ui.firstChild(sub));
        const Vec2 tp{title.x + 20.f, title.y + title.h * 0.5f};
        ui.pointerMove(tp);
        ui.pointerDown(tp);
        ui.pointerMove({tp.x + (odd ? 30.f : -30.f), tp.y + (odd ? 10.f : -10.f)});
        ui.pointerUp({tp.x + (odd ? 30.f : -30.f), tp.y + (odd ? 10.f : -10.f)});
        step("mdi");
        // graphics view: drag an item, then a rubber band over empty space
        const Rect ib = scene.item(rectItem)->rect;
        const Vec2 ip = w::sceneToView(ui, view, {ib.x + 10.f, ib.y + 10.f});
        ui.pointerMove(ip);
        ui.pointerDown(ip);
        ui.pointerMove({ip.x + (odd ? 12.f : -12.f), ip.y});
        ui.update(1.f / 60.f);
        ui.pointerUp({ip.x + (odd ? 12.f : -12.f), ip.y});
        const Vec2 b0 = w::sceneToView(ui, view, {320, 100}), b1 = w::sceneToView(ui, view, {380, 150});
        ui.pointerMove(b0);
        ui.pointerDown(b0);
        ui.pointerMove(b1);
        ui.update(1.f / 60.f);
        ui.pointerUp(b1);
        step("graphics view");
        // on-screen keyboard: a key pressed (it types into the last text field)
        const NodeHandle key = w::virtualKeyboardKey(ui, kb, "?123");
        ui.pointerMove(ctr(ui.rect(key)));
        ui.pointerDown(ctr(ui.rect(key)));
        ui.pointerUp(ctr(ui.rect(key)));
        step("keyboard");
        // timeline: drag the clip
        const Rect tc = ui.contentRect(tl);
        const Vec2 cp{tc.x + 96.f + (tc.w - 96.f) * 0.2f, tc.y + 22.f + 14.f};
        ui.pointerMove(cp);
        ui.pointerDown(cp);
        ui.pointerMove({cp.x + (odd ? 20.f : -20.f), cp.y});
        ui.pointerUp({cp.x + (odd ? 20.f : -20.f), cp.y});
        step("timeline");
        // docks: the split handle between the two docks, by keyboard
        ui.setFocus(handle, FocusReason::Keyboard);
        ui.keyDown(odd ? Key::Down : Key::Up);
        step("dock split handle");
        ui.update(1.f / 60.f);
        ui.update(1.f / 60.f); // idle frame
        step("update + paint");
    };
    for (int i = 0; i < 12; ++i) frame(i); // warm-up: atlas, style table, scratch capacity
    g_counting = true;
    for (int i = 0; i < 50; ++i) frame(i);
    g_counting = false;
    std::printf("allocations in 50 steady frames of the ported widgets: %ld\n", g_allocs.load());
    if (g_allocs.load())
        for (int k = 0; k < g_stepCount; ++k) std::printf("  %-32s %ld\n", g_steps[k].name, g_steps[k].count);
    UI_CHECK_NO_ALLOC(g_allocs.load());
    w::setGraphicsScene(ui, view, nullptr); // (the static scene outlives the context)
    return finish("fuse_ui_port_zero_alloc");
}
