// FUSE UI2 gates: fuse_ui_core_tests <gate>. See FUSE_UI_PLAN.md §9 UI2.
#include <fuse/ui/context.hpp>
#include <fuse/ui/cpu_raster.hpp>
#include <fuse/ui/immediate.hpp>
#include <fuse/ui/test_driver.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>
#include <vector>

// ---- allocation counter (zero_alloc_steady) ------------------------------------------------------------------
static std::atomic<long> g_allocs{0};
static bool g_counting = false;
void* operator new(std::size_t n) {
    if (g_counting) ++g_allocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace fuse;
using namespace fuse::ui;

namespace {
int g_failures = 0;
void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}
bool near(f32 a, f32 b) { return std::fabs(a - b) < 0.01f; }
bool rectIs(const Rect& r, f32 x, f32 y, f32 w, f32 h) { return near(r.x, x) && near(r.y, y) && near(r.w, w) && near(r.h, h); }

void gateLayout() {
    UiContext ui;
    ui.setViewport({400.f, 300.f});
    const NodeId row = ui.create("Panel", ui.root(), "row");
    {
        LayoutProps& L = ui.layout(row);
        L.kind = LayoutKind::Row;
        L.gap = 10.f;
        L.padding = Insets::all(5.f);
        L.height = 50.f;
    }
    const NodeId a = ui.create("Panel", row, "a");
    ui.layout(a).width = 40.f;
    ui.layout(a).height = 20.f;
    const NodeId b = ui.create("Panel", row, "b");
    ui.layout(b).grow = 1.f;
    const NodeId c = ui.create("Panel", row, "c");
    ui.layout(c).width = 60.f;
    ui.layout(c).height = 20.f;
    const NodeId hidden = ui.create("Panel", row, "hidden");
    ui.layout(hidden).width = 500.f;
    ui.setVisible(hidden, false);

    const NodeId col = ui.create("Panel", ui.root(), "col");
    ui.layout(col).height = 100.f;
    ui.layout(col).align = Align::Center;
    ui.layout(col).justify = Justify::Center;
    const NodeId centred = ui.create("Panel", col, "centred");
    ui.layout(centred).width = 50.f;
    ui.layout(centred).height = 40.f;

    const NodeId hud = ui.create("Panel", ui.root(), "hud");
    ui.layout(hud).kind = LayoutKind::Absolute;
    ui.layout(hud).width = 200.f;
    ui.layout(hud).height = 100.f;
    const NodeId corner = ui.create("Panel", hud, "corner");
    ui.layout(corner).width = 30.f;
    ui.layout(corner).height = 30.f;
    ui.layout(corner).anchors = {.right = true, .bottom = true, .margin = Insets::all(10.f)};
    const NodeId placed = ui.create("Panel", hud, "placed");
    ui.layout(placed).position = {7.f, 9.f};
    ui.layout(placed).width = 5.f;
    ui.layout(placed).height = 5.f;
    ui.update();

    expect(rectIs(ui.rect(row), 0, 0, 400, 50), "layout: row stretches across the root");
    expect(rectIs(ui.rect(a), 5, 5, 40, 20), "layout: fixed child keeps its size at the padded origin");
    expect(rectIs(ui.rect(b), 55, 5, 270, 40), "layout: grow child takes the spare space and stretches across");
    expect(rectIs(ui.rect(c), 335, 5, 60, 20), "layout: trailing fixed child after gap");
    expect(ui.rect(hidden).w == 0.f || !ui.visible(hidden), "layout: hidden child is skipped");
    expect(rectIs(ui.rect(centred), 175, 80, 50, 40), "layout: column justify/align centre");
    expect(rectIs(ui.rect(corner), 160, 210, 30, 30), "layout: right/bottom anchors with margin");
    expect(rectIs(ui.rect(placed), 7, 159, 5, 5), "layout: absolute position");

    // Scroll view: 10 x 30 px rows in a 100 px viewport.
    UiContext s;
    s.setViewport({200.f, 200.f});
    const NodeId sv = s.create("ScrollView", s.root(), "sv");
    s.layout(sv).height = 100.f;
    for (int i = 0; i < 10; ++i) {
        const NodeId r = s.create("Panel", sv, "r" + std::to_string(i));
        s.layout(r).height = 30.f;
    }
    UiTestDriver d(s);
    d.frame();
    expect(near(s.rect(s.find("r0")).y, 0.f), "scroll: first row at the top");
    d.wheel("sv", -1.f);
    d.frame();
    expect(near(s.scrollOffset(sv), 40.f) && near(s.rect(s.find("r0")).y, -40.f), "scroll: wheel scrolls 40 px");
    for (int i = 0; i < 10; ++i) d.wheel("sv", -1.f);
    d.frame();
    expect(near(s.scrollOffset(sv), 200.f), "scroll: clamped to content - viewport");
    bool clipped = false;
    for (const DrawCmd& cmd : s.drawList().commands()) clipped |= cmd.op == DrawOp::PushClip;
    expect(clipped, "scroll: children are clipped");
}

void gateStyle() {
    UiContext ui;
    std::string err;
    expect(ui.styleSheet().parse(R"(
        Button { background: #ff0000; }
        Button.primary { background: #00ff00; }
        Button#ok { background: #0000ff; radius: 8; }
        Button.primary:hover { background: #ffffff; }
        Label.big, Label#title { text-scale: 3; }
    )", &err), "style: sheet parses");
    ui.invalidateStyle();
    const NodeId plain = ui.create("Button", ui.root(), "plain");
    const NodeId primary = ui.create("Button", ui.root(), "primary");
    ui.setClasses(primary, "wide primary");
    const NodeId ok = ui.create("Button", ui.root(), "ok");
    ui.setClasses(ok, "primary");
    const NodeId title = ui.create("Label", ui.root(), "title");
    ui.setText(title, "Hi");
    ui.update();
    expect(ui.style(plain).background == Color::rgb(0xff0000), "style: type rule (later rule of equal specificity wins over theme)");
    expect(ui.style(primary).background == Color::rgb(0x00ff00), "style: .class beats type");
    expect(ui.style(ok).background == Color::rgb(0x0000ff) && ui.style(ok).radius == 8.f, "style: #name beats .class");
    expect(ui.style(title).textScale == 3, "style: selector list");
    const f32 h3 = ui.rect(title).h;
    UiTestDriver d(ui);
    d.move(d.centre("primary"));
    const UpdateStats st = d.frame();
    expect(ui.style(primary).background == Color::rgb(0xffffff), "style: :hover state rule");
    expect(st.styled && !st.laidOut, "style: a hover restyles without relayout");
    Style o{};
    o.background = Color::rgb(0x123456);
    ui.setStyleOverride(plain, o, kStyleBackground);
    d.frame();
    expect(ui.style(plain).background == Color::rgb(0x123456), "style: per-node override");
    expect(h3 == 21.f, "style: text-scale sizes the label (7 px glyphs x 3)");
    StyleSheet bad;
    expect(!bad.parse("Button { colour: #fff; }", &err) && !err.empty(), "style: unknown property is an error");
    expect(!bad.parse("Button:wobble { color: #ffffff; }", &err), "style: unknown state is an error");
}

void gateEvents() {
    UiContext ui;
    ui.setViewport({300.f, 200.f});
    ui.layout(ui.root()).align = Align::Start;
    const NodeId panel = ui.create("Panel", ui.root(), "panel");
    ui.layout(panel).padding = Insets::all(10.f);
    const NodeId btn = ui.create("Button", panel, "btn");
    ui.setText(btn, "Go");
    const NodeId off = ui.create("Button", panel, "off");
    ui.setText(off, "Off");
    ui.setEnabled(off, false);
    std::vector<std::string> log;
    ui.addEventFilter(panel, [&](UiContext&, NodeId, const Event& e, Phase p) {
        if (e.type == EventType::PointerDown) log.push_back(p == Phase::Capture ? "panel:capture" : p == Phase::Bubble ? "panel:bubble" : "panel:target");
        return false;
    });
    int clicks = 0, offClicks = 0;
    ui.onClicked(btn, [&](UiContext&, NodeId) { ++clicks; });
    ui.onClicked(off, [&](UiContext&, NodeId) { ++offClicks; });
    UiTestDriver d(ui);
    d.frame();
    expect(d.click("btn") && clicks == 1, "events: click emits clicked and is consumed");
    expect(log.size() == 1 && log[0] == "panel:capture", "events: ancestor sees capture, button consumes before bubble");
    log.clear();
    d.click("off");
    expect(offClicks == 0, "events: disabled button does not click");
    expect(!log.empty() && log.back() == "panel:bubble", "events: unconsumed event bubbles to the ancestor");
    expect(!d.click(Vec2{290.f, 190.f}), "events: empty root area passes input to the game");
    expect(d.click(Vec2{2.f, 2.f}), "events: a UI panel consumes pointer input");
    // Press inside, release outside: no click (pointer capture).
    const Rect r = ui.rect(btn);
    ui.dispatch({EventType::PointerDown, {r.x + 2.f, r.y + 2.f}});
    ui.dispatch({EventType::PointerUp, {299.f, 199.f}});
    expect(clicks == 1, "events: release outside cancels the click");
    expect(ui.hasState(btn, kStatePressed) == false, "events: pressed state cleared on release");
    // Modal dialog blocks everything outside it.
    const NodeId dialog = ui.create("Panel", ui.root(), "dialog");
    const NodeId inDialog = ui.create("Button", dialog, "yes");
    ui.setText(inDialog, "Yes");
    ui.setModal(dialog, true);
    int yes = 0;
    ui.onClicked(inDialog, [&](UiContext&, NodeId) { ++yes; });
    d.frame();
    expect(d.click("btn") && clicks == 1, "events: modal blocks (and consumes) clicks outside it");
    d.click("yes");
    expect(yes == 1, "events: modal content still clicks");
    // Slider drag.
    ui.destroy(dialog);
    const NodeId sl = ui.create("Slider", ui.root(), "sl");
    ui.setRange(sl, 0.f, 100.f);
    d.frame();
    const Rect sr = ui.rect(sl);
    d.drag({sr.x, sr.y + 4.f}, {sr.x + sr.w * 0.75f, sr.y + 4.f});
    expect(std::fabs(ui.value(sl) - 75.f) < 1.f, "events: slider drag sets the value");
    expect(!ui.alive(dialog) && !ui.alive(inDialog), "events: destroy kills the subtree (stale ids stop resolving)");
}

void gateFocus() {
    UiContext ui;
    ui.setViewport({300.f, 300.f});
    const NodeId a = ui.create("Button", ui.root(), "a");
    const NodeId b = ui.create("Button", ui.root(), "b");
    const NodeId off = ui.create("Button", ui.root(), "off");
    ui.setEnabled(off, false);
    const NodeId edit = ui.create("LineEdit", ui.root(), "edit");
    ui.create("Label", ui.root(), "label");
    UiTestDriver d(ui);
    d.frame();
    d.tab();
    expect(ui.focused() == a, "focus: Tab starts at the first focusable");
    d.tab();
    d.tab();
    expect(ui.focused() == edit, "focus: Tab skips disabled widgets");
    d.tab();
    expect(ui.focused() == a, "focus: Tab wraps");
    d.tab(true);
    expect(ui.focused() == edit, "focus: Shift+Tab goes backwards");
    d.type("hello");
    d.key(Key::Left);
    d.key(Key::Backspace);
    d.type("L");
    expect(ui.text(edit) == "helLo", "focus: typing edits the focused line edit (caret, backspace)");
    int undo = 0;
    ui.addShortcut(Key::Z, kModCtrl, [&](UiContext&) { ++undo; });
    expect(d.key(Key::Z, kModCtrl) && undo == 1, "focus: shortcut runs when the focus widget ignores the key");
    d.click("b");
    expect(ui.focused() == b, "focus: click focuses");
    d.frame();
    expect(ui.hasState(b, kStateFocus) && ui.style(b).border == Color::rgb(0x3d8bfd), "focus: :focus style applies");
    const NodeId dialog = ui.create("Panel", ui.root(), "dialog");
    const NodeId ok = ui.create("Button", dialog, "ok");
    const NodeId cancel = ui.create("Button", dialog, "cancel");
    ui.setModal(dialog, true);
    d.frame();
    d.tab();
    d.tab();
    expect(ui.focused() == cancel, "focus: Tab moves into the modal scope");
    d.tab();
    expect(ui.focused() == ok, "focus: Tab wraps inside the modal scope");
}

void gateRaster() {
    DrawList dl;
    dl.fillRect({8, 8, 16, 16}, Color::rgb(0xff0000));
    dl.fillRect({32, 8, 24, 24}, Color::rgb(0x00ff00), 12.f);
    dl.strokeRect({8, 32, 20, 20}, Color::rgb(0x0000ff), 2.f);
    dl.pushClip({0, 56, 16, 8});
    dl.fillRect({0, 56, 64, 8}, Color::rgb(0xffffff));
    dl.popClip();
    dl.text({34, 40}, "I", Color::rgb(0xffff00), 2);
    dl.fillRect({8, 8, 16, 16}, Color::rgb(0x0000ff, 128));
    Image img;
    img.resize(64, 64);
    rasterize(dl, img);
    const Color c1 = img.at(10, 10);
    expect(c1.r == 127 && c1.b == 128 && c1.a == 255, "raster: src-over 50% blue over red");
    expect(img.at(30, 10).a == 0, "raster: outside shapes stays clear");
    expect(img.at(44, 20) == Color::rgb(0x00ff00), "raster: rounded rect centre is solid");
    expect(img.at(32, 8).a == 0, "raster: rounded corner is cut");
    expect(img.at(8, 40) == Color::rgb(0x0000ff) && img.at(18, 42).a == 0, "raster: stroke draws the edge, not the inside");
    expect(img.at(4, 60).a == 255 && img.at(30, 60).a == 0, "raster: clip rect limits drawing");
    int ink = 0;
    for (u32 y = 40; y < 54; ++y) for (u32 x = 34; x < 44; ++x) ink += img.at(x, y) == Color::rgb(0xffff00);
    expect(ink == 2 * 2 * 11, "raster: 5x7 'I' at scale 2 (11 ink cells)");
    Image again;
    again.resize(64, 64);
    rasterize(dl, again);
    expect(img.hash() == again.hash(), "raster: deterministic");
    Image damaged;
    damaged.resize(64, 64);
    rasterize(dl, damaged, {0, 0, 16, 16});
    expect(damaged.at(10, 10) == img.at(10, 10) && damaged.at(44, 20).a == 0, "raster: damage rect limits the update");

    UiContext ui;
    ui.setViewport({200.f, 80.f});
    const NodeId b = ui.create("Button", ui.root(), "b");
    ui.setText(b, "OK");
    ui.update();
    Image frame;
    frame.resize(200, 80);
    rasterize(ui.drawList(), frame);
    const Rect r = ui.rect(b);
    expect(frame.at(u32(r.x + 3), u32(r.y + 3)) == Color::rgb(0x2b2d31), "raster: themed button background");
}

void gateZeroAlloc() {
    UiContext ui;
    ui.setViewport({640.f, 480.f});
    const NodeId sl = ui.create("Slider", ui.root(), "sl");
    for (int i = 0; i < 50; ++i) ui.setText(ui.create("Label", ui.root()), "row " + std::to_string(i));
    for (int i = 0; i < 3; ++i) ui.update(1.f / 60.f);
    ui.setValue(sl, 0.5f);
    ui.update(); // warm the draw-list capacity for the value change
    g_allocs = 0;
    g_counting = true;
    for (int i = 0; i < 100; ++i) ui.update(1.f / 60.f);
    g_counting = false;
    expect(g_allocs == 0, "zero_alloc: idle frames allocate nothing");
    g_counting = true;
    UpdateStats st{};
    for (int i = 0; i < 10; ++i) {
        ui.setValue(sl, (i % 2) ? 0.25f : 0.75f);
        st = ui.update(1.f / 60.f);
    }
    g_counting = false;
    expect(g_allocs == 0 && st.painted && !st.laidOut, "zero_alloc: paint-only changes allocate nothing and skip layout");
    expect(!st.damage.empty() && st.damage.w <= ui.rect(sl).w + 0.5f, "zero_alloc: damage covers only the changed widget");
    std::printf("     allocations: %ld\n", g_allocs.load());
}

void gateBench() {
    UiContext ui;
    ui.setViewport({1280.f, 720.f});
    const NodeId sv = ui.create("ScrollView", ui.root(), "list");
    ui.layout(sv).grow = 1.f;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; ++i) ui.setText(ui.create("Label", sv), "item " + std::to_string(i));
    const UpdateStats full = ui.update();
    const auto t1 = std::chrono::steady_clock::now();
    UpdateStats idle{};
    for (int i = 0; i < 100; ++i) idle = ui.update(1.f / 60.f);
    const auto t2 = std::chrono::steady_clock::now();
    const double buildMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double idleUs = std::chrono::duration<double, std::micro>(t2 - t1).count() / 100.0;
    std::printf("     10k labels: build+style+layout+paint %.1f ms, idle update %.2f us, %zu draw cmds\n", buildMs, idleUs,
                ui.drawList().commands().size());
    expect(full.nodesLaidOut >= 10000, "bench: all nodes laid out");
    expect(!idle.styled && !idle.laidOut && !idle.painted, "bench: idle update does no work");
    expect(buildMs < 2000.0, "bench: 10k-node first frame under 2 s (debug-safe bound)");
    expect(idleUs < 1000.0, "bench: idle update under 1 ms");
}

void gateA11y() {
    UiContext ui;
    const NodeId form = ui.create("Panel", ui.root(), "form");
    ui.setText(ui.create("Label", form), "Name");
    const NodeId edit = ui.create("LineEdit", form, "name");
    ui.setAccessibleName(edit, "Name");
    const NodeId cb = ui.create("CheckBox", form, "agree");
    ui.setText(cb, "Agree");
    ui.setChecked(cb, true);
    const NodeId ok = ui.create("Button", form, "ok");
    ui.setText(ok, "OK");
    ui.setEnabled(ok, false);
    const NodeId sl = ui.create("Slider", form, "vol");
    ui.setAccessibleName(sl, "Volume");
    ui.setValue(sl, 0.5f);
    ui.setFocus(edit);
    ui.update();
    const std::string want = "group \"root\"\n"
                             "  group \"form\"\n"
                             "    label \"Name\"\n"
                             "    textbox \"Name\" focused\n"
                             "    checkbox \"Agree\" checked\n"
                             "    button \"OK\" disabled\n"
                             "    slider \"Volume\" value=0.5\n";
    const std::string got = ui.dumpAccessibility();
    expect(got == want, "a11y: tree dump matches");
    if (got != want) std::printf("%s", got.c_str());
    expect(ui.dumpTree().find("Button#ok [") != std::string::npos, "a11y: layout tree dump names classes and rects");
}

void gateImmediate() {
    UiContext ui;
    ui.setViewport({400.f, 300.f});
    im::ImmediateUi gui(ui);
    UiTestDriver d(ui);
    bool vsync = false;
    f32 exposure = 0.f;
    std::string name = "fuse";
    int saves = 0;
    bool showExtra = true;
    auto frame = [&] {
        gui.begin();
        gui.label("Settings");
        if (gui.button("Save##save")) ++saves;
        gui.checkbox("VSync", vsync);
        gui.slider("Exposure", exposure, -4.f, 4.f);
        gui.inputText("Name", name);
        if (showExtra) gui.label("Extra");
        gui.end();
        d.frame();
    };
    frame();
    expect(gui.widgetCount() == 6, "immediate: widgets created");
    const NodeId saveNode = gui.nodeFor("Save##save");
    expect(ui.text(saveNode) == "Save", "immediate: '##id' is hidden from the label");
    d.click(ui.rect(saveNode).x + 2.f > 0 ? Vec2{ui.rect(saveNode).x + 4.f, ui.rect(saveNode).y + 4.f} : Vec2{});
    frame();
    expect(saves == 1, "immediate: click reported by the next frame's button()");
    frame();
    expect(saves == 1 && gui.nodeFor("Save##save") == saveNode, "immediate: nodes persist; one click = one true");
    const NodeId vs = gui.nodeFor("VSync");
    d.click(Vec2{ui.rect(vs).x + 4.f, ui.rect(vs).y + 4.f});
    frame();
    expect(vsync, "immediate: checkbox writes back");
    exposure = 2.f;
    frame();
    expect(near(ui.value(gui.nodeFor("Exposure")), 2.f), "immediate: program value flows into the slider");
    d.click(d.centre("Name"));
    d.type("!");
    frame();
    expect(name == "fuse!", "immediate: input text writes back");
    showExtra = false;
    frame();
    expect(gui.widgetCount() == 5 && !gui.nodeFor("Extra").isValid(), "immediate: widgets not called are removed");
    showExtra = true;
    frame();
    expect(gui.widgetCount() == 6, "immediate: re-added widget comes back");
}

void gateTimers() {
    UiContext ui;
    std::atomic<int> posted{0};
    std::thread t([&] { for (int i = 0; i < 100; ++i) ui.post([&](UiContext&) { ++posted; }); });
    t.join();
    expect(posted == 0 && ui.needsUpdate(), "timers: posted commands wait for update()");
    ui.update();
    expect(posted == 100, "timers: update() drains commands posted from another thread");
    int fired = 0, ticks = 0;
    ui.addTimer(0.5f, [&](UiContext&) { ++fired; });
    const u32 rep = ui.addTimer(0.1f, [&](UiContext&) { ++ticks; }, true);
    for (int i = 0; i < 31; ++i) ui.update(1.f / 60.f);
    expect(fired == 1, "timers: one-shot fires after its delay");
    expect(ticks == 5, "timers: repeating timer fires every interval");
    ui.cancelTimer(rep);
    for (int i = 0; i < 30; ++i) ui.update(1.f / 60.f);
    expect(ticks == 5 && fired == 1, "timers: cancelled / one-shot timers stay quiet");
    const NodeId bar = ui.create("ProgressBar", ui.root(), "bar");
    ui.animateValue(bar, 1.f, 0.25f);
    ui.update(0.125f);
    const f32 mid = ui.value(bar);
    ui.update(0.125f);
    expect(mid > 0.5f && mid < 1.f && ui.value(bar) == 1.f && !ui.animating(), "timers: value tween eases to its target");
    ui.animateBackground(bar, Color::rgb(0xff0000), 0.1f);
    for (int i = 0; i < 10; ++i) ui.update(0.02f);
    expect(ui.style(bar).background == Color::rgb(0xff0000), "timers: colour tween reaches its target");
}
} // namespace

int main(int argc, char** argv) {
    const std::string gate = argc > 1 ? argv[1] : "all";
    const bool all = gate == "all";
    if (all || gate == "layout_conformance") gateLayout();
    if (all || gate == "style_cascade") gateStyle();
    if (all || gate == "event_routing") gateEvents();
    if (all || gate == "focus_order") gateFocus();
    if (all || gate == "cpu_raster_golden") gateRaster();
    if (all || gate == "zero_alloc_steady") gateZeroAlloc();
    if (all || gate == "bench_10k") gateBench();
    if (all || gate == "a11y_tree_dump") gateA11y();
    if (all || gate == "immediate") gateImmediate();
    if (all || gate == "timers_commands") gateTimers();
    std::printf("fuse_ui %s: %s (%d failure(s))\n", gate.c_str(), g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
