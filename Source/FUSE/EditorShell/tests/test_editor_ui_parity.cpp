// fuse_editor_ui_* gates that closed the Qt-removal blockers of docs/editor/QT_PARITY.md: floating panels as OS windows,
// panel and hierarchy drag and drop, the hierarchy context menu, the colour dialog, OS file dialogs and the editor's
// keyboard (Tab) order. Real pointer / key events (UiContext, or posted OS input for the native gates).
//   fuse_editor_ui_parity_gates --gate=<float_window|dock_drag|dock_split|hierarchy_dnd|hierarchy_menu|color_dialog|native_dialogs|tab_order>
#include "fuse/editor/fui/editor_shell.hpp"
#include "scripted_services.hpp"

#include "fuse/app/native.hpp"
#include "fuse/app/platform.hpp"
#include <fuse/editor/editor_host.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(__APPLE__)
#include "dialog_driver_macos.hpp"
#endif

using namespace fuse::editor;
using fuse::f32;
using fuse::i32;
using fuse::u32;
using fuse::ui::Key;
using fuse::ui::NodeHandle;
using fuse::ui::Rect;
using fuse::ui::Vec2;
namespace w = fuse::ui::widgets;
namespace fs = std::filesystem;

namespace {
int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct RigOptions {
    bool dialogs = false; ///< DialogServices (real dialogs) instead of ScriptedServices
    bool native = false;  ///< show the window through NativePlatform (OS windows, OS input)
};

struct Rig {
    fuse::app::Application app;
    std::unique_ptr<fuse::app::NativePlatform> native; // after the app: destroyed before it
    fuse::app::AppWindow& win;
    EditorHost host;
    test::ScriptedServices sv;
    std::unique_ptr<fui::DialogServices> ds;
    EditorController c;
    fui::EditorShellWindow shell;
    explicit Rig(RigOptions o = {})
        : app({.name = "fuse_editor_ui_parity", .loadSettings = false}), native(o.native ? makeNative(app) : nullptr),
          win(app.createWindow({.title = "FUSE Editor", .width = 1280, .height = 760})),
          ds(o.dialogs ? std::make_unique<fui::DialogServices>(app, win, native.get()) : nullptr),
          c(host, ds ? static_cast<ShellServices&>(*ds) : static_cast<ShellServices&>(sv)), shell(win, c) {
        frames(2);
    }
    static std::unique_ptr<fuse::app::NativePlatform> makeNative(fuse::app::Application& a) {
        fuse::app::NativeOptions o;
        o.vulkan = false;   // software presenter: the gates look at windows, not at the GPU path
        o.osPopups = true;  // as the editor runs
        o.osWindows = true;
        return std::make_unique<fuse::app::NativePlatform>(a, o);
    }
    void frames(int n = 1) {
        for (int i = 0; i < n; ++i) {
            c.tick();
            shell.frame();
            app.step(native ? 0.005f : 0.f);
        }
    }
    fuse::ui::UiContext& ui() { return win.ui(); }
    NodeHandle find(std::string_view id) { return ui().find(id); }
    Rect rect(std::string_view id) { return ui().rect(find(id)); }
    static Vec2 centre(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
    void clickAt(Vec2 p, fuse::ui::u8 mods = 0) {
        ui().setModifiers(mods);
        win.click(p);
        ui().setModifiers(0);
        frames();
    }
    void clickId(std::string_view id) { clickAt(centre(rect(id))); }
    /// A pointer drag in `steps` moves (frames between them, like real input).
    void drag(Vec2 from, Vec2 to, int steps = 6, fuse::ui::u8 mods = 0, bool release = true) {
        ui().setModifiers(mods);
        ui().pointerMove(from);
        ui().pointerDown(from);
        frames();
        for (int k = 1; k <= steps; ++k) {
            const f32 t = f32(k) / f32(steps);
            ui().setModifiers(mods);
            ui().pointerMove({from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t});
            frames();
        }
        if (release) {
            ui().pointerUp(to);
            ui().setModifiers(0);
            frames();
        }
    }
    const EditorViewState& v() { return c.viewState(); }
};

bool displayed(Rig& r, std::string_view id) {
    const NodeHandle n = r.find(id);
    return r.ui().alive(n) && r.ui().rect(n).w > 0.f && r.ui().computed(n).visible;
}

i32 lineOf(Rig& r, EntityRef e) {
    const auto& h = r.v().hierarchy;
    for (u32 i = 0; i < h.size(); ++i)
        if (h[i].entity == e) return i32(i);
    return -1;
}
EntityRef parentOf(Rig& r, EntityRef e) {
    for (const HierarchyRowView& h : r.v().hierarchy)
        if (h.entity == e) return h.parent;
    return {};
}
Vec2 rowCentre(Rig& r, EntityRef e) {
    const i32 l = lineOf(r, e);
    if (l < 0) return {-1.f, -1.f};
    const Rect rr = w::treeLineRect(r.ui(), r.find("hierarchy.tree"), u32(l));
    return {rr.x + 40.f, rr.y + rr.h * 0.5f};
}
EntityRef create(Rig& r, ContextMenuAction a) {
    r.c.runEntityAction(a);
    r.frames(2);
    return r.c.lastCreated();
}
void settle(Rig& r, int n = 3) {
    for (int i = 0; i < n; ++i) r.frames();
}

/// Clicks the item `label` (prefix match) of the open popup `id`. False when not found.
bool pickMenu(Rig& r, std::string_view id, std::string_view label) {
    const NodeHandle scope = r.find(id);
    if (!r.ui().alive(scope)) return false;
    const NodeHandle panel = r.ui().firstChild(scope);
    for (NodeHandle it = r.ui().firstChild(panel); r.ui().alive(it); it = r.ui().nextSibling(it))
        if (r.ui().text(it).substr(0, label.size()) == label) {
            r.clickAt(Rig::centre(r.ui().rect(it)));
            return true;
        }
    return false;
}

// ---- blocker 1: floating panels are real OS windows -----------------------------------------------------------------
int gateFloatWindow() {
    Rig r({.native = true});
    if (!r.native->available()) {
        std::printf("fuse_editor_ui_float_window: no display, skipped\n");
        return 77;
    }
    auto& native = *r.native;
    native.show(r.win);
    for (int i = 0; i < 40; ++i) r.frames();
    const f32 s = r.win.scale();
    const unsigned long long main = native.nativeWindow(r.win);
    CHECK(main != 0 && r.ui().osWindows());

    // float the console: its frame leaves the main window's draw list and gets its own OS window
    CHECK(r.shell.setPanelFloating("dock.console", true));
    settle(r, 6);
    CHECK(native.toolWindowCount(r.win) == 1);
    const unsigned long long tool = native.toolWindow(r.win, 0);
    CHECK(tool != 0 && tool != main);
    const Rect fr = r.rect("float.console");
    const Vec2 origin = native.screenOrigin(r.win);
    Rect sr = native.toolWindowScreenRect(r.win, 0);
    std::printf("float_window: frame %.0f,%.0f %.0fx%.0f  OS window %.0f,%.0f %.0fx%.0f (main origin %.0f,%.0f, scale %.2f)\n", fr.x,
                fr.y, fr.w, fr.h, sr.x, sr.y, sr.w, sr.h, origin.x, origin.y, s);
    CHECK(std::fabs(sr.x - (origin.x + fr.x * s)) <= 1.f && std::fabs(sr.y - (origin.y + fr.y * s)) <= 1.f);
    CHECK(std::fabs(sr.w - std::ceil(fr.w * s)) <= 1.f && std::fabs(sr.h - std::ceil(fr.h * s)) <= 1.f);
    const u32 logIdx = r.find("console.log").index;
    bool inMain = false, inTool = false;
    for (const auto& cmd : r.ui().drawList().cmds()) inMain = inMain || cmd.node == logIdx;
    CHECK(r.ui().windowSurfaces().size() == 1);
    for (const auto& cmd : r.ui().windowSurfaces().front().draw.cmds()) inTool = inTool || cmd.node == logIdx;
    CHECK(!inMain && inTool);
    fuse::ui::Image shot;
    CHECK(native.captureToolWindow(r.win, 0, shot) && shot.width == int(sr.w) && shot.height == int(sr.h));
    {
        u32 distinct = 0, last = 0;
        for (u32 px : shot.pixels)
            if (px != last) { ++distinct; last = px; }
        std::printf("float_window: tool window capture %dx%d, %u colour runs\n", shot.width, shot.height, distinct);
        CHECK(distinct > 20); // title, log, input field: not a blank window
    }

    // drag the OS window by its title with posted OS input: it follows, and may leave the main window
    const Rect title = r.rect("float.console.caption");
    Vec2 grab{(title.x - fr.x + 12.f) * s, (title.y - fr.y + title.h * 0.5f) * s}; // tool-window local device px
    CHECK(native.postPointerButton(tool, grab, true));
    settle(r, 3);
    const f32 mainW = f32(r.win.pixelSize().w);
    // the pointer ends 30 px past the main window's right edge (outside every drop zone), the frame with it
    const f32 dx = origin.x + mainW + 30.f * s - (sr.x + grab.x);
    for (int k = 1; k <= 5; ++k) {
        const Rect now = native.toolWindowScreenRect(r.win, 0);
        const Vec2 wantScreen{sr.x + grab.x + dx * f32(k) / 5.f, sr.y + grab.y + 20.f * s * f32(k) / 5.f};
        CHECK(native.postPointerMove(tool, {wantScreen.x - now.x, wantScreen.y - now.y}));
        settle(r, 3);
    }
    {
        const Rect now = native.toolWindowScreenRect(r.win, 0);
        CHECK(native.postPointerButton(tool, {grab.x, grab.y}, false));
        settle(r, 3);
        sr = native.toolWindowScreenRect(r.win, 0);
        std::printf("float_window: dragged to %.0f,%.0f (main right edge %.0f)\n", sr.x, sr.y, origin.x + mainW);
        CHECK(std::fabs(now.x - sr.x) <= 2.f);
    }
    CHECK(r.shell.panelFloating("dock.console") && native.toolWindowCount(r.win) == 1);
    CHECK(sr.x + sr.w > origin.x + mainW); // a real top-level window: not clipped to the editor window
    { // read back when the window is fully on a monitor (X11 cannot read off-screen parts)
        bool onScreen = false;
        for (const fuse::app::DisplayInfo& d : native.displays())
            onScreen = onScreen || (sr.x >= d.bounds.x && sr.y >= d.bounds.y && sr.x + sr.w <= d.bounds.x + d.bounds.w &&
                                    sr.y + sr.h <= d.bounds.y + d.bounds.h);
        if (onScreen) CHECK(native.captureToolWindow(r.win, 0, shot) && shot.width == int(sr.w));
        else std::printf("float_window: dragged window extends past the screen, no read-back\n");
    }

    // OS click into the tool window focuses the console input; keys (through the main window) type into it
    {
        const Rect in = r.rect("console.input"), f2 = r.rect("float.console");
        CHECK(native.postClick(tool, {(in.x - f2.x + in.w * 0.5f) * s, (in.y - f2.y + in.h * 0.5f) * s}));
        settle(r, 4);
        CHECK(r.ui().focused() == r.find("console.input"));
        const std::size_t before = r.c.console().lines().size();
        r.win.text("help");
        r.win.key(Key::Enter);
        settle(r, 3);
        CHECK(r.c.console().lines().size() > before);
    }

    // drag the tool window back over the left dock: drop zones show in the main window, release docks it
    {
        const Rect fNow = r.rect("float.console");
        const Rect cap = r.rect("float.console.caption");
        grab = {(cap.x - fNow.x + 12.f) * s, (cap.y - fNow.y + cap.h * 0.5f) * s};
        CHECK(native.postPointerButton(tool, grab, true));
        settle(r, 3);
        const Rect left = r.rect("dock.left");
        const Vec2 target{origin.x + (left.x + left.w * 0.5f) * s, origin.y + (left.y + left.h * 0.5f) * s};
        const Rect start = native.toolWindowScreenRect(r.win, 0);
        const Vec2 from{start.x + grab.x, start.y + grab.y};
        for (int k = 1; k <= 6; ++k) {
            const Rect now = native.toolWindowScreenRect(r.win, 0);
            const Vec2 want{from.x + (target.x - from.x) * f32(k) / 6.f, from.y + (target.y - from.y) * f32(k) / 6.f};
            CHECK(native.postPointerMove(tool, {want.x - now.x, want.y - now.y}));
            settle(r, 3);
        }
        std::printf("float_window: dragging %d, drop zone %d\n", int(r.shell.panelDragging()), r.shell.dropZone());
        CHECK(r.shell.panelDragging() && r.shell.dropZone() == 0 && displayed(r, "dock.dropzone.left"));
        const Rect now = native.toolWindowScreenRect(r.win, 0);
        CHECK(native.postPointerButton(tool, {target.x - now.x, target.y - now.y}, false));
        settle(r, 6);
        CHECK(!r.shell.panelFloating("dock.console") && r.shell.panelSideName("dock.console") == "left");
        CHECK(native.toolWindowCount(r.win) == 0 && !displayed(r, "dock.dropzone.left"));
    }

    // a tab dragged out of the main window becomes a tool window under the pointer; its Dock button (OS click) docks it
    {
        CHECK(r.shell.showPanel("dock.profiler"));
        settle(r, 2);
        const NodeHandle bar = r.find("tabs.bottom.bar");
        const Rect b = r.ui().rect(bar);
        // the current (Profiler) tab: the bar lists Console? no: console moved left; Profiler is the first bottom tab
        const Vec2 at{(b.x + 30.f) * s, (b.y + b.h * 0.5f) * s};
        CHECK(native.postPointerMove(main, at) && native.postPointerButton(main, at, true));
        settle(r, 3);
        for (int k = 1; k <= 4; ++k) {
            CHECK(native.postPointerMove(main, {at.x + 40.f * s * f32(k), at.y - 90.f * s * f32(k)}));
            settle(r, 3);
        }
        const bool floated = r.shell.panelFloating("dock.profiler");
        const Rect vp = r.rect("viewport"); // released over the viewport: no drop zone, it stays a tool window
        const Vec2 rel{(vp.x + vp.w * 0.5f) * s, (vp.y + vp.h * 0.5f) * s};
        CHECK(native.postPointerMove(main, rel));
        settle(r, 3);
        CHECK(native.postPointerButton(main, rel, false));
        settle(r, 6);
        std::printf("float_window: tab drag-out floated %d, tool windows %zu\n", int(floated), native.toolWindowCount(r.win));
        CHECK(floated && r.shell.panelFloating("dock.profiler") && native.toolWindowCount(r.win) == 1);
        const Rect d = r.rect("float.profiler.dock"), f3 = r.rect("float.profiler");
        CHECK(native.postClick(native.toolWindow(r.win, 0), {(d.x - f3.x + d.w * 0.5f) * s, (d.y - f3.y + d.h * 0.5f) * s}));
        settle(r, 6);
        CHECK(!r.shell.panelFloating("dock.profiler") && native.toolWindowCount(r.win) == 0);
    }
    return 0;
}

// ---- blocker 2a: drag to dock with drop zones (headless: in-window frames, same code path) -------------------------
void gateDockDrag(Rig& r) {
    CHECK(!r.ui().osWindows()); // headless: frames are drawn in the main window
    // 1. a tab dragged out of the bottom dock floats under the pointer; released over the right dock it docks there
    const Rect bar = r.rect("tabs.bottom.bar");
    const Vec2 tab{bar.x + 24.f, bar.y + bar.h * 0.5f}; // Console, the first bottom tab
    const Rect right = r.rect("dock.right");
    const Vec2 overRight = Rig::centre(right);
    const Vec2 mid = Rig::centre(r.rect("viewport"));
    r.drag(tab, mid, 6, 0, false);
    CHECK(r.shell.panelDragging() && r.shell.panelFloating("dock.console"));
    CHECK(r.ui().alive(r.find("float.console")) && displayed(r, "dock.dropzone.left") && displayed(r, "dock.dropzone.right"));
    CHECK(r.shell.dropZone() == -1);
    for (int k = 1; k <= 4; ++k) {
        r.ui().pointerMove({mid.x + (overRight.x - mid.x) * f32(k) / 4.f, mid.y + (overRight.y - mid.y) * f32(k) / 4.f});
        r.frames();
    }
    CHECK(r.shell.dropZone() == 1 && r.ui().hasClass(r.find("dock.dropzone.right"), "hot") && !r.ui().hasClass(r.find("dock.dropzone.left"), "hot"));
    r.ui().pointerUp(overRight);
    settle(r);
    std::printf("dock_drag: console -> %s\n", r.shell.panelSideName("dock.console").c_str());
    CHECK(r.shell.panelSideName("dock.console") == "right" && r.shell.panelVisible("dock.console"));
    CHECK(!r.shell.panelDragging() && !displayed(r, "dock.dropzone.right") && !r.ui().alive(r.find("float.console")));

    // 2. dragged out and released over the viewport: it stays floating where it was dropped (back at the bottom first)
    r.shell.movePanel("dock.console", fuse::ui::widgets::DockSide::Bottom);
    settle(r);
    const Rect bbar = r.rect("tabs.bottom.bar");
    const Vec2 dropAt = Rig::centre(r.rect("viewport"));
    // Console is the bottom dock's first tab again (panels keep the table order)
    r.drag({bbar.x + 24.f, bbar.y + bbar.h * 0.5f}, dropAt, 6);
    CHECK(r.shell.panelFloating("dock.console") && r.shell.dropZone() == -1);
    const Rect f = r.rect("float.console");
    CHECK(f.contains(dropAt));
    // the frame is drawn in the main window (no OS tool windows headless)
    bool drawn = false;
    for (const auto& cmd : r.ui().drawList().cmds()) drawn = drawn || cmd.node == r.find("console.log").index;
    CHECK(drawn);

    // 3. a click on the title (no move) changes nothing; a title drag with Ctrl never docks; Escape keeps it floating
    const Rect cap = r.rect("float.console.caption");
    const Vec2 capAt{cap.x + 10.f, cap.y + cap.h * 0.5f};
    r.clickAt(capAt);
    CHECK(r.shell.panelFloating("dock.console") && std::fabs(r.rect("float.console").x - f.x) < 1.f);
    const Rect left = r.rect("dock.left");
    r.drag(capAt, Rig::centre(left), 6, fuse::ui::kModCtrl);
    CHECK(r.shell.panelFloating("dock.console")); // Ctrl: moved, not docked
    CHECK(std::fabs(r.rect("float.console").x - (f.x + Rig::centre(left).x - capAt.x)) < 2.f);
    const Rect cap2 = r.rect("float.console.caption");
    const Vec2 cap2At{cap2.x + 10.f, cap2.y + cap2.h * 0.5f};
    const Vec2 leftLow{r.rect("dock.left").x + 30.f, r.rect("dock.left").y + r.rect("dock.left").h * 0.85f};
    r.drag(cap2At, leftLow, 6, 0, false);
    CHECK(r.shell.dropZone() == 0);
    r.ui().keyDown(Key::Escape);
    r.ui().keyUp(Key::Escape);
    r.ui().pointerUp(leftLow);
    settle(r);
    CHECK(r.shell.panelFloating("dock.console") && !r.shell.panelDragging() && !displayed(r, "dock.dropzone.left"));

    // 4. the grip resizes the frame; the size is part of the saved layout
    const std::array<f32, 2> before = r.shell.floatSize("dock.console");
    const Vec2 grip = Rig::centre(r.rect("float.console.grip"));
    r.drag(grip, {grip.x + 120.f, grip.y + 60.f}, 4);
    const std::array<f32, 2> after = r.shell.floatSize("dock.console");
    std::printf("dock_drag: frame %.0fx%.0f -> %.0fx%.0f\n", before[0], before[1], after[0], after[1]);
    CHECK(std::fabs(after[0] - before[0] - 120.f) < 2.f && std::fabs(after[1] - before[1] - 60.f) < 2.f);
    CHECK(std::fabs(r.rect("float.console").w - after[0]) < 1.f && std::fabs(r.rect("float.console").h - after[1]) < 1.f);
    const std::string saved = r.shell.saveLayout();
    r.shell.setFloatSize("dock.console", 200.f, 150.f);
    CHECK(r.shell.restoreLayout(saved));
    settle(r);
    CHECK(std::fabs(r.shell.floatSize("dock.console")[0] - after[0]) < 1.f && r.shell.panelFloating("dock.console"));

    // 5. the floating title dragged onto the bottom dock docks it there
    const Rect cap3 = r.rect("float.console.caption");
    r.drag({cap3.x + 10.f, cap3.y + cap3.h * 0.5f}, Rig::centre(r.rect("dock.bottom")), 6);
    CHECK(r.shell.panelSideName("dock.console") == "bottom" && !r.shell.panelFloating("dock.console"));
    // 6. a side emptied by a drag offers a drop strip along its edge: move every left panel out, then drag one back
    for (const char* id : {"dock.hierarchy", "dock.project_hub", "dock.asset_browser"}) r.shell.movePanel(id, fuse::ui::widgets::DockSide::Right);
    settle(r);
    CHECK(r.rect("dock.left").w < 2.f || !r.ui().computed(r.find("dock.left")).visible);
    r.shell.setPanelFloating("dock.hierarchy", true);
    settle(r);
    const Rect cap4 = r.rect("float.hierarchy.caption");
    const Rect area = r.rect("editor.dockarea");
    r.drag({cap4.x + 10.f, cap4.y + cap4.h * 0.5f}, {area.x + 20.f, area.y + area.h * 0.4f}, 6, 0, false);
    CHECK(r.shell.dropZone() == 0 && displayed(r, "dock.dropzone.left"));
    r.ui().pointerUp({area.x + 20.f, area.y + area.h * 0.4f});
    settle(r);
    CHECK(r.shell.panelSideName("dock.hierarchy") == "left" && r.rect("dock.hierarchy").w > 50.f);
}

// ---- dock splits: several tab stacks per side in nested splitters (Qt splitDockWidget / tabifyDockWidget) ----------
bool near1(f32 a, f32 b, f32 eps = 1.5f) { return std::fabs(a - b) <= eps; }
void gateDockSplit(Rig& r) {
    using w::DockEdge;
    auto stackRect = [&](const char* panel) { return r.ui().rect(r.shell.panelStackNode(panel)); };
    CHECK(r.shell.dockStackCount() == 3);
    // 1. API: the asset browser in its own stack under the hierarchy's (a column split on the left side)
    CHECK(r.shell.splitPanel("dock.asset_browser", "dock.hierarchy", DockEdge::Bottom));
    settle(r);
    CHECK(r.shell.dockStackCount() == 4 && displayed(r, "dock.split.1") && displayed(r, "tabs.split.1.bar"));
    CHECK(r.shell.panelStackNode("dock.asset_browser") == r.find("dock.split.1") && r.shell.panelSideName("dock.asset_browser") == "left");
    CHECK(r.shell.panelVisible("dock.asset_browser") && r.shell.panelVisible("dock.hierarchy") && displayed(r, "dock.asset_browser"));
    const Rect hs = stackRect("dock.hierarchy"), as = stackRect("dock.asset_browser");
    std::printf("dock_split: left stacks %.0f,%.0f %.0fx%.0f over %.0f,%.0f %.0fx%.0f\n", hs.x, hs.y, hs.w, hs.h, as.x, as.y, as.w, as.h);
    CHECK(near1(hs.x, as.x) && near1(hs.w, as.w) && as.y >= hs.bottom() - 1.f && hs.h > 60.f && as.h > 60.f);
    CHECK(r.ui().text(r.ui().firstChild(r.find("dock.split.1"))) == "Asset Browser"); // titled after its panel

    // 2. drag: the Console tab onto the left edge zone of the right stack splits it (zone and preview while dragging)
    {
        const Rect bar = r.rect("tabs.bottom.bar");
        const Rect right = r.rect("dock.right");
        const Vec2 edge{right.x + 12.f, right.y + right.h * 0.5f};
        r.drag({bar.x + 24.f, bar.y + bar.h * 0.5f}, edge, 6, 0, false);
        std::printf("dock_split: drag over the right stack edge: side %d edge %d\n", r.shell.dropZone(), r.shell.dropEdge());
        CHECK(r.shell.panelDragging() && r.shell.dropZone() == 1 && r.shell.dropEdge() == int(DockEdge::Left));
        CHECK(r.shell.dropStackNode() == r.find("dock.right") && displayed(r, "dock.dropzone.split"));
        const Rect zone = r.rect("dock.dropzone.split");
        CHECK(near1(zone.x, right.x, 2.f) && near1(zone.w, right.w * 0.5f, 2.f)); // the left half previews the split
        CHECK(r.ui().text(r.find("dock.dropzone.split.label")) == "Split Left");
        r.ui().pointerUp(edge);
        settle(r);
    }
    CHECK(!r.shell.panelDragging() && !displayed(r, "dock.dropzone.split") && !r.shell.panelFloating("dock.console"));
    CHECK(r.shell.panelSideName("dock.console") == "right" && r.shell.dockStackCount() == 5);
    const NodeHandle consoleStack = r.shell.panelStackNode("dock.console");
    CHECK(consoleStack.valid() && consoleStack != r.find("dock.right") && r.shell.panelVisible("dock.console"));
    CHECK(stackRect("dock.console").right() <= r.rect("dock.right").x + 1.f && near1(stackRect("dock.console").y, r.rect("dock.right").y));
    CHECK(w::dockSplitOf(r.ui(), consoleStack) == w::dockSplitOf(r.ui(), r.find("dock.right")));

    // 3. drag: the Profiler tab onto the middle of the console's stack tabifies it there
    {
        const Rect bar = r.rect("tabs.bottom.bar"); // Profiler is the bottom stack's first tab now
        const Vec2 mid = Rig::centre(r.ui().rect(consoleStack));
        r.drag({bar.x + 24.f, bar.y + bar.h * 0.5f}, mid, 6, 0, false);
        CHECK(r.shell.dropEdge() == int(DockEdge::Center) && r.shell.dropStackNode() == consoleStack && r.shell.dropZone() == 1);
        CHECK(r.ui().text(r.find("dock.dropzone.split.label")) == "Add as Tab");
        r.ui().pointerUp(mid);
        settle(r);
    }
    CHECK(r.shell.panelStackNode("dock.profiler") == consoleStack && r.shell.panelVisible("dock.profiler") && r.shell.dockStackCount() == 5);
    CHECK(r.shell.showPanel("dock.console") && r.shell.panelVisible("dock.console") && !r.shell.panelVisible("dock.profiler"));

    // 4. nested: the material editor above the console stack (a column split inside the right side's row split)
    CHECK(r.shell.splitPanel("dock.material_editor", "dock.console", DockEdge::Top));
    settle(r);
    const Rect ms = stackRect("dock.material_editor"), cs = stackRect("dock.console");
    CHECK(r.shell.dockStackCount() == 6 && near1(ms.x, cs.x) && near1(ms.w, cs.w) && ms.bottom() <= cs.y + 1.f);
    const NodeHandle innerSplit = w::dockSplitOf(r.ui(), consoleStack);
    CHECK(innerSplit.valid() && innerSplit != w::dockSplitOf(r.ui(), r.find("dock.right")) &&
          innerSplit == w::dockSplitOf(r.ui(), r.shell.panelStackNode("dock.material_editor")));

    // 5. the handle between two stacks resizes them (pointer drag)
    {
        const NodeHandle handle = r.ui().nextSibling(r.ui().parent(r.find("dock.left")));
        const Rect h = r.ui().rect(handle);
        const f32 before = r.rect("dock.left").h;
        r.drag(Rig::centre(h), {h.x + h.w * 0.5f, h.y + h.h * 0.5f + 40.f}, 4);
        std::printf("dock_split: left stack %.0f -> %.0f px tall\n", before, r.rect("dock.left").h);
        CHECK(near1(r.rect("dock.left").h, before + 40.f, 3.f));
    }

    // 6. save, reset, restore: the same stacks, splits, tabs and sizes
    const std::string saved = r.shell.saveLayout();
    std::printf("dock_split: layout\n%s", saved.c_str());
    CHECK(saved.find("stack:dock.split.") != std::string::npos && saved.find("tree left v[") != std::string::npos &&
          saved.find("tree right h[") != std::string::npos);
    std::map<std::string, Rect> rects;
    const char* ids[] = {"dock.hierarchy", "dock.asset_browser", "dock.console", "dock.profiler", "dock.material_editor", "dock.inspector", "dock.timeline"};
    for (const char* id : ids) rects[id] = stackRect(id);
    r.shell.resetLayout();
    settle(r);
    CHECK(r.shell.dockStackCount() == 3 && !r.ui().alive(r.find("dock.split.1")) && r.shell.panelSideName("dock.console") == "bottom");
    CHECK(!w::dockSplitOf(r.ui(), r.find("dock.left")).valid() && !w::dockSplitOf(r.ui(), r.find("dock.right")).valid());
    CHECK(r.shell.restoreLayout(saved));
    settle(r);
    CHECK(r.shell.dockStackCount() == 6 && r.shell.saveLayout() == saved);
    for (const char* id : ids) {
        const Rect a = rects[id], b = stackRect(id);
        if (!(near1(a.x, b.x) && near1(a.y, b.y) && near1(a.w, b.w) && near1(a.h, b.h)))
            std::printf("dock_split: %s %.0f,%.0f %.0fx%.0f != %.0f,%.0f %.0fx%.0f\n", id, a.x, a.y, a.w, a.h, b.x, b.y, b.w, b.h);
        CHECK(near1(a.x, b.x) && near1(a.y, b.y) && near1(a.w, b.w) && near1(a.h, b.h));
    }
    CHECK(r.shell.panelVisible("dock.console") && r.shell.panelStackNode("dock.profiler") == r.shell.panelStackNode("dock.console"));

    // 7. a stack whose panels leave goes and its split collapses
    r.shell.movePanel("dock.console", w::DockSide::Bottom);
    r.shell.movePanel("dock.profiler", w::DockSide::Bottom);
    settle(r);
    CHECK(r.shell.dockStackCount() == 5);
    const NodeHandle matStack = r.shell.panelStackNode("dock.material_editor");
    CHECK(w::dockSplitOf(r.ui(), matStack) == w::dockSplitOf(r.ui(), r.find("dock.right")) && w::dockSplitOf(r.ui(), matStack).valid());
    r.shell.setPanelFloating("dock.material_editor", true);
    settle(r);
    CHECK(r.shell.dockStackCount() == 4 && !w::dockSplitOf(r.ui(), r.find("dock.right")).valid() && r.rect("dock.right").w > 100.f);
    r.shell.setPanelFloating("dock.material_editor", false); // home: the right side's own stack
    settle(r);
    CHECK(r.shell.panelStackNode("dock.material_editor") == r.find("dock.right"));

    // 8. the tab context menu: Split Down puts the current panel in a stack below its own
    CHECK(r.shell.showPanel("dock.inspector"));
    settle(r);
    const Vec2 tabAt{r.rect("tabs.right.bar").x + 20.f, Rig::centre(r.rect("tabs.right.bar")).y};
    r.ui().pointerMove(tabAt);
    r.ui().pointerDown(tabAt, fuse::ui::MouseButton::Right);
    r.ui().pointerUp(tabAt, fuse::ui::MouseButton::Right);
    settle(r);
    CHECK(pickMenu(r, "panel.menu", "Split Down"));
    settle(r);
    const NodeHandle inspStack = r.shell.panelStackNode("dock.inspector");
    CHECK(inspStack.valid() && inspStack != r.find("dock.right") && r.shell.panelSideName("dock.inspector") == "right");
    CHECK(r.ui().rect(inspStack).y >= r.rect("dock.right").bottom() - 1.f && r.shell.dockStackCount() == 5);
    // a lone panel cannot split its own stack
    CHECK(r.shell.splitPanel("dock.inspector", "dock.inspector", DockEdge::Left) && r.shell.panelStackNode("dock.inspector") == inspStack);
    // View > Reset Layout: one stack per side again
    r.shell.resetLayout();
    settle(r);
    CHECK(r.shell.dockStackCount() == 3 && r.shell.panelStackNode("dock.inspector") == r.find("dock.right"));
}

// ---- blocker 2b: hierarchy drag and drop reparenting (undo) ---------------------------------------------------------
void gateHierarchyDnd(Rig& r) {
    const EntityRef a = create(r, ContextMenuAction::CreateEmpty);
    const EntityRef b = create(r, ContextMenuAction::CreateCube);
    const EntityRef c = create(r, ContextMenuAction::CreatePointLight);
    CHECK(a.valid() && b.valid() && c.valid());
    settle(r);
    // a plain click selects, nothing moves
    r.clickAt(rowCentre(r, a));
    CHECK(r.v().primary == a && !parentOf(r, a).valid());
    // drag A onto B: drop mark on B's row, A becomes B's child (one reparent command)
    const Vec2 pa = rowCentre(r, a), pb = rowCentre(r, b);
    r.drag(pa, pb, 6, 0, false);
    CHECK(r.shell.hierarchyDragging() && r.shell.hierarchyDropLine() == lineOf(r, b));
    CHECK(displayed(r, "hierarchy.drop_mark"));
    const Rect mark = r.rect("hierarchy.drop_mark"), rowB = w::treeLineRect(r.ui(), r.find("hierarchy.tree"), u32(lineOf(r, b)));
    CHECK(std::fabs(mark.y - rowB.y) < 1.f && std::fabs(mark.h - rowB.h) < 1.f);
    CHECK(std::string(r.ui().text(r.find("hierarchy.info"))).find("child of") != std::string::npos);
    r.ui().pointerUp(pb);
    settle(r);
    std::printf("hierarchy_dnd: parent of A = #%u (B #%u)\n", parentOf(r, a).index, b.index);
    CHECK(parentOf(r, a) == b && !r.shell.hierarchyDragging() && !displayed(r, "hierarchy.drop_mark"));
    CHECK(r.ui().text(r.find("hierarchy.tree")).find('\t') != std::string_view::npos); // indented under B
    // undo / redo
    r.c.undo();
    settle(r);
    CHECK(!parentOf(r, a).valid());
    r.c.redo();
    settle(r);
    CHECK(parentOf(r, a) == b);
    // cycle: B dropped onto its child A is refused
    r.drag(rowCentre(r, b), rowCentre(r, a), 6);
    settle(r);
    CHECK(parentOf(r, a) == b && !parentOf(r, b).valid());
    // drop below the rows: back to the top level
    const Rect tree = r.rect("hierarchy.tree");
    const Rect lastRow = w::treeLineRect(r.ui(), r.find("hierarchy.tree"), u32(r.v().hierarchy.size() - 1));
    const Vec2 empty{tree.x + 40.f, std::min(tree.y + tree.h - 4.f, lastRow.y + lastRow.h + 20.f)};
    CHECK(empty.y > lastRow.y + lastRow.h);
    r.drag(rowCentre(r, a), empty, 6, 0, false);
    CHECK(r.shell.hierarchyDropLine() == -1);
    r.ui().pointerUp(empty);
    settle(r);
    CHECK(!parentOf(r, a).valid());
    // a multi-selection moves together: Ctrl-click A and C, drag C onto B
    r.clickAt(rowCentre(r, a));
    r.clickAt(rowCentre(r, c), fuse::ui::kModCtrl);
    CHECK(r.v().selection.size() == 2);
    r.drag(rowCentre(r, c), rowCentre(r, b), 6);
    settle(r);
    CHECK(parentOf(r, a) == b && parentOf(r, c) == b);
    // Escape cancels a drag
    r.drag(rowCentre(r, c), empty, 6, 0, false);
    CHECK(r.shell.hierarchyDragging());
    r.ui().keyDown(Key::Escape);
    r.ui().keyUp(Key::Escape);
    r.ui().pointerUp(empty);
    settle(r);
    CHECK(!r.shell.hierarchyDragging() && parentOf(r, c) == b);
}

// ---- blocker 3: hierarchy context menu -------------------------------------------------------------------------------
void gateHierarchyMenu(Rig& r) {
    const EntityRef cube = create(r, ContextMenuAction::CreateCube);
    settle(r);
    const u32 count0 = r.v().entityCount;
    // right click on the row: selects it, opens the menu
    r.ui().pointerMove(rowCentre(r, cube));
    r.ui().pointerDown(rowCentre(r, cube), fuse::ui::MouseButton::Right);
    r.ui().pointerUp(rowCentre(r, cube), fuse::ui::MouseButton::Right);
    settle(r);
    CHECK(r.ui().alive(r.find("hierarchy.context")) && r.v().primary == cube);
    const auto& items = r.shell.hierarchyContextMenuItems();
    std::string all;
    for (const std::string& s : items) all += s + " | ";
    std::printf("hierarchy_menu: %s\n", all.c_str());
    auto has = [&](std::string_view t) {
        for (const std::string& s : items)
            if (s.find(t) != std::string::npos) return true;
        return false;
    };
    CHECK(has("Rename") && has("Create Empty Child") && has("Create Cube Child") && has("Duplicate") && has("Delete") &&
          has("Prefab") && has("Clear Parent"));
    CHECK(has("!Clear Parent")); // top-level: disabled
    // Create Empty Child: one undo step, parented, selected
    CHECK(pickMenu(r, "hierarchy.context", "Create Empty Child"));
    settle(r);
    const EntityRef child = r.v().primary;
    CHECK(child.valid() && child != cube && parentOf(r, child) == cube && r.v().entityCount == count0 + 1);
    r.c.undo();
    settle(r);
    CHECK(r.v().entityCount == count0 && lineOf(r, child) < 0);
    r.c.redo();
    settle(r);
    CHECK(r.v().entityCount == count0 + 1 && parentOf(r, child) == cube);
    // Clear Parent on the child (enabled now)
    r.ui().pointerDown(rowCentre(r, child), fuse::ui::MouseButton::Right);
    r.ui().pointerUp(rowCentre(r, child), fuse::ui::MouseButton::Right);
    settle(r);
    CHECK(r.v().primary == child && !has("!Clear Parent") && has("Clear Parent"));
    CHECK(pickMenu(r, "hierarchy.context", "Clear Parent"));
    settle(r);
    CHECK(!parentOf(r, child).valid());
    // Rename opens the inline name row
    r.ui().pointerDown(rowCentre(r, cube), fuse::ui::MouseButton::Right);
    r.ui().pointerUp(rowCentre(r, cube), fuse::ui::MouseButton::Right);
    settle(r);
    CHECK(pickMenu(r, "hierarchy.context", "Rename"));
    settle(r);
    CHECK(displayed(r, "hierarchy.rename_row") && r.ui().focused() == r.find("hierarchy.rename"));
    r.ui().setText(r.find("hierarchy.rename"), "Crate");
    r.ui().keyDown(Key::Enter);
    settle(r);
    CHECK(r.v().hierarchy[u32(lineOf(r, cube))].label.find("Crate") != std::string::npos);
    // Duplicate, Delete (+ undo)
    r.ui().pointerDown(rowCentre(r, cube), fuse::ui::MouseButton::Right);
    r.ui().pointerUp(rowCentre(r, cube), fuse::ui::MouseButton::Right);
    settle(r);
    CHECK(pickMenu(r, "hierarchy.context", "Duplicate"));
    settle(r);
    CHECK(r.v().entityCount == count0 + 2);
    const EntityRef dup = r.v().primary;
    r.ui().pointerDown(rowCentre(r, dup), fuse::ui::MouseButton::Right);
    r.ui().pointerUp(rowCentre(r, dup), fuse::ui::MouseButton::Right);
    settle(r);
    CHECK(pickMenu(r, "hierarchy.context", "Delete"));
    settle(r);
    CHECK(r.v().entityCount == count0 + 1 && lineOf(r, dup) < 0);
    r.c.undo();
    settle(r);
    CHECK(r.v().entityCount == count0 + 2);
    // Save as Prefab (asks for the file through the shell services)
    const fs::path prefab = fs::temp_directory_path() / "fuse_editor_ui_hier_menu.prefab";
    std::error_code ec;
    fs::remove(prefab, ec);
    r.sv.files.push_back(prefab.string());
    r.ui().pointerDown(rowCentre(r, cube), fuse::ui::MouseButton::Right);
    r.ui().pointerUp(rowCentre(r, cube), fuse::ui::MouseButton::Right);
    settle(r);
    CHECK(pickMenu(r, "hierarchy.context", "Save"));
    settle(r, 6);
    std::printf("hierarchy_menu: prefab written %d\n", int(fs::exists(prefab, ec)));
    CHECK(fs::exists(prefab, ec) && !r.sv.asked.empty() && r.sv.asked.back().find("Prefab") != std::string::npos);
    fs::remove(prefab, ec);
    // right click on the empty space: create items only
    const Rect tree = r.rect("hierarchy.tree");
    const Vec2 empty{tree.x + 40.f, tree.y + tree.h - 6.f};
    r.ui().pointerDown(empty, fuse::ui::MouseButton::Right);
    r.ui().pointerUp(empty, fuse::ui::MouseButton::Right);
    settle(r);
    CHECK(r.ui().alive(r.find("hierarchy.context")) && !has("Rename") && has("Create Cube"));
    const u32 before = r.v().entityCount;
    CHECK(pickMenu(r, "hierarchy.context", "Create Cube"));
    settle(r);
    CHECK(r.v().entityCount == before + 1 && !parentOf(r, r.c.lastCreated()).valid());
    // keyboard: Shift+F10 on the focused tree opens it for the current row
    r.clickAt(rowCentre(r, cube));
    r.ui().keyDown(Key::F10, fuse::ui::kModShift);
    settle(r);
    CHECK(r.ui().alive(r.find("hierarchy.context")) && has("Rename"));
    r.ui().keyDown(Key::Escape);
    settle(r);
}

// ---- blocker 4: colour dialog ----------------------------------------------------------------------------------------
struct ColorScript {
    std::string hex;
    int button = 1; // 1 OK, 2 Cancel, 3 Escape
    bool ran = false;
};
void scriptColorDialog(Rig& r, ColorScript& s) {
    r.ui().startTimer(0.05f, false, [p = &s](fuse::ui::UiContext& u) {
        const NodeHandle hex = u.find("color.hex");
        if (!u.alive(hex)) return;
        p->ran = true;
        if (!p->hex.empty()) {
            u.setText(hex, p->hex);
            u.emit(hex, fuse::ui::SignalKind::Submitted, {});
        }
        if (p->button == 1) u.emit(u.find("color.ok"), fuse::ui::SignalKind::Clicked, {});
        else if (p->button == 2) u.emit(u.find("color.cancel"), fuse::ui::SignalKind::Clicked, {});
        else { u.setFocus(hex); u.keyDown(Key::Escape); }
    });
}

void gateColorDialog(Rig& r) {
    CHECK(r.ds != nullptr);
    CHECK(r.shell.panels().lightingAddSun().valid());
    settle(r, 4);
    // OK: the hex field's colour reaches the sun (Lighting panel -> ShellServices::pickColour -> SetProperty)
    ColorScript ok{"#3399E6", 1};
    scriptColorDialog(r, ok);
    CHECK(r.shell.panels().lightingPickColour("sun"));
    settle(r, 4);
    LightingView v = r.shell.panels().lighting();
    std::printf("color_dialog: sun colour %.3f %.3f %.3f (backend %s)\n", v.sunColor[0], v.sunColor[1], v.sunColor[2], r.ds->lastBackend());
    CHECK(ok.ran && std::string(r.ds->lastBackend()) == "fuse");
    CHECK(std::fabs(v.sunColor[0] - 0.2f) < 0.01f && std::fabs(v.sunColor[1] - 0.6f) < 0.01f && std::fabs(v.sunColor[2] - 0.9f) < 0.01f);
    CHECK(!r.ui().alive(r.find("editor.color_dialog")));
    // Cancel and Escape leave it unchanged
    ColorScript cancel{"#FF0000", 2};
    scriptColorDialog(r, cancel);
    CHECK(!r.shell.panels().lightingPickColour("sun"));
    ColorScript esc{"#00FF00", 3};
    scriptColorDialog(r, esc);
    CHECK(!r.shell.panels().lightingPickColour("sun"));
    settle(r, 3);
    v = r.shell.panels().lighting();
    CHECK(cancel.ran && esc.ran && std::fabs(v.sunColor[0] - 0.2f) < 0.01f);
    // the picker itself: a direct pick returns the picker's colour with alpha
    ColorScript direct{"", 1};
    r.ui().startTimer(0.05f, false, [p = &direct](fuse::ui::UiContext& u) {
        const NodeHandle pk = u.find("color.picker");
        if (!u.alive(pk)) return;
        p->ran = true;
        w::setColorPickerColor(u, pk, {1.f, 0.5f, 0.f, 0.25f});
        u.emit(u.find("color.ok"), fuse::ui::SignalKind::Clicked, {});
    });
    const auto c = r.ds->pickColour({0.f, 0.f, 0.f, 1.f});
    CHECK(direct.ran && c && std::fabs(c->r - 1.f) < 0.01f && std::fabs(c->g - 0.5f) < 0.02f && std::fabs(c->a - 0.25f) < 0.02f);
}

// ---- blocker 4 + 5 on the OS: native colour / file dialogs, FUSE UI fallback -----------------------------------------
#if defined(_WIN32)
struct DlgFound { DWORD pid; HWND hwnd; };
HWND findCommonDialog() { // a visible #32770 dialog of this process
    DlgFound f{GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        auto& f = *reinterpret_cast<DlgFound*>(l);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        wchar_t cls[64]{};
        GetClassNameW(h, cls, 63);
        if (pid == f.pid && IsWindowVisible(h) && std::wcscmp(cls, L"#32770") == 0) { f.hwnd = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.hwnd;
}
std::wstring wideOf(const std::string& s) {
    std::wstring w(std::size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}
HWND fileNameEdit(HWND dlg) {
    struct S { HWND dlg, edit; } s{dlg, nullptr};
    EnumChildWindows(dlg, [](HWND h, LPARAM l) -> BOOL {
        auto& s = *reinterpret_cast<S*>(l);
        wchar_t cls[64]{};
        GetClassNameW(h, cls, 63);
        if (std::wcscmp(cls, L"Edit") != 0 || !IsWindowVisible(h)) return TRUE;
        for (HWND a = h; a && a != s.dlg; a = GetParent(a))
            if (GetDlgCtrlID(a) == 1148 || GetDlgCtrlID(a) == 1001) { s.edit = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.edit;
}
/// Helper thread: waits for the dialog, then fills it (file name, or the colour dialog's R / G / B edits) and presses
/// `button`. `state` 1 = done.
std::thread answerDialog(std::wstring fileName, int rgb, int button, std::atomic<int>& state) {
    return std::thread([fileName, rgb, button, &state] {
        HWND dlg = nullptr;
        for (int i = 0; i < 400 && !(dlg = findCommonDialog()); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (!dlg) { state = -1; return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (!fileName.empty()) {
            HWND edit = nullptr;
            for (int i = 0; i < 80 && !(edit = fileNameEdit(dlg)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
            if (!edit) { state = -2; PostMessageW(dlg, WM_COMMAND, IDCANCEL, 0); return; }
            SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(fileName.c_str()));
        }
        if (rgb >= 0) { // ChooseColor (CC_FULLOPEN): Red / Green / Blue edits 706 / 707 / 708
            SetDlgItemInt(dlg, 706, UINT((rgb >> 16) & 255), FALSE);
            SetDlgItemInt(dlg, 707, UINT((rgb >> 8) & 255), FALSE);
            SetDlgItemInt(dlg, 708, UINT(rgb & 255), FALSE);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        PostMessageW(dlg, WM_COMMAND, WPARAM(button), 0);
        for (int i = 0; i < 200 && IsWindow(dlg); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (IsWindow(dlg)) { state = -3; PostMessageW(dlg, WM_COMMAND, IDCANCEL, 0); return; }
        state = 1;
    });
}
#endif

/// FUSE UI file dialog driven from a UI timer (the nested dialog loop runs it): navigate to `dir`, type `name`, accept.
struct FileScript {
    std::string dir, name;
    bool ran = false;
};
void scriptFileDialog(Rig& r, FileScript& s) {
    r.ui().startTimer(0.05f, false, [p = &s](fuse::ui::UiContext& u) {
        const NodeHandle dlg = u.firstChild(u.find("editor.file_dialog")); // the id is on the popup scope; the API takes the card
        if (!u.alive(dlg)) return;
        p->ran = true;
        w::fileDialogNavigate(u, dlg, p->dir);
        for (u32 i = 0; i < w::fileDialogEntryCount(u, dlg); ++i)
            if (w::fileDialogEntry(u, dlg, i) == p->name) { w::fileDialogActivate(u, dlg, i); return; }
        // not listed (save): the file-name field (the dialog's second text field) + Enter
        std::vector<NodeHandle> fields;
        std::function<void(NodeHandle)> walk = [&](NodeHandle n) {
            for (NodeHandle k = u.firstChild(n); u.alive(k); k = u.nextSibling(k)) {
                if (u.role(k) == fuse::ui::A11yRole::TextField) fields.push_back(k);
                walk(k);
            }
        };
        walk(dlg);
        if (fields.size() >= 2) {
            u.setText(fields[1], p->name);
            u.setFocus(fields[1]);
            u.keyDown(Key::Enter);
        }
    });
}

int gateNativeDialogs() {
    Rig r({.dialogs = true, .native = true});
    if (!r.native->available()) {
        std::printf("fuse_editor_ui_native_dialogs: no display, skipped\n");
        return 77;
    }
    r.native->show(r.win);
    for (int i = 0; i < 20; ++i) r.frames();
    const fs::path dir = fs::temp_directory_path() / ("fuse_editor_ui_dialogs_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000));
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path scene = dir / "dialog_scene.fuselevel";
    create(r, ContextMenuAction::CreateCube);
#if defined(_WIN32)
    std::thread([] { std::this_thread::sleep_for(std::chrono::seconds(150)); std::fprintf(stderr, "native_dialogs watchdog\n"); std::_Exit(3); }).detach();
    // Save As through IFileSaveDialog (owned by the editor window)
    {
        std::atomic<int> st{0};
        auto th = answerDialog(wideOf(scene.string()), -1, IDOK, st);
        CHECK(r.c.fileSaveAs());
        th.join();
        std::printf("native_dialogs: save helper %d, backend %s, scene %s\n", st.load(), r.ds->lastBackend(), r.v().scenePath.c_str());
        CHECK(st == 1 && std::string(r.ds->lastBackend()) == "native" && fs::exists(scene, ec));
    }
    // Open through IFileOpenDialog
    {
        CHECK(r.c.fileNew());
        settle(r);
        std::atomic<int> st{0};
        auto th = answerDialog(wideOf(scene.string()), -1, IDOK, st);
        CHECK(r.c.fileOpen());
        th.join();
        settle(r);
        CHECK(st == 1 && std::string(r.ds->lastBackend()) == "native" && fs::equivalent(r.v().scenePath, scene, ec));
    }
    // Cancel: nothing opens
    {
        std::atomic<int> st{0};
        auto th = answerDialog(L"", -1, IDCANCEL, st);
        const auto p = r.ds->pickOpenFile("Open Scene", "FUSE level (*.fuselevel)", dir.string());
        th.join();
        CHECK(st == 1 && !p);
    }
    // folder picker (cancelled)
    {
        std::atomic<int> st{0};
        auto th = answerDialog(L"", -1, IDCANCEL, st);
        CHECK(!r.ds->pickDirectory("Project Location", dir.string()));
        th.join();
        CHECK(st == 1);
    }
    // native colour dialog (ChooseColor), opt-in
    {
        CHECK(r.shell.panels().lightingAddSun().valid());
        settle(r, 4);
        r.ds->options().nativeColorDialog = true;
        std::atomic<int> st{0};
        auto th = answerDialog(L"", 0x3399E6, IDOK, st);
        CHECK(r.shell.panels().lightingPickColour("sun"));
        th.join();
        settle(r, 4);
        const LightingView v = r.shell.panels().lighting();
        std::printf("native_dialogs: ChooseColor helper %d -> %.3f %.3f %.3f\n", st.load(), v.sunColor[0], v.sunColor[1], v.sunColor[2]);
        CHECK(st == 1 && std::string(r.ds->lastBackend()) == "native");
        CHECK(std::fabs(v.sunColor[0] - 0.2f) < 0.01f && std::fabs(v.sunColor[1] - 0.6f) < 0.01f && std::fabs(v.sunColor[2] - 0.9f) < 0.01f);
        std::atomic<int> st2{0};
        auto th2 = answerDialog(L"", -1, IDCANCEL, st2);
        CHECK(!r.shell.panels().lightingPickColour("sun"));
        th2.join();
        CHECK(st2 == 1);
        r.ds->options().nativeColorDialog = false;
    }
#elif defined(__APPLE__)
    // NSSavePanel / NSOpenPanel / NSColorPanel (fuse::app's window-less dialogs), answered from a main run loop timer
    // inside their modal sessions (dialog_driver_macos.mm)
    std::thread([] { std::this_thread::sleep_for(std::chrono::seconds(240)); std::fprintf(stderr, "native_dialogs watchdog\n"); std::_Exit(3); }).detach();
    using Ans = fuse::editor::test::MacDialogAnswer;
    auto answer = [](Ans a, const std::function<void()>& call) {
        auto st = fuse::editor::test::macAnswerNextDialog(std::move(a));
        call();
        fuse::editor::test::macDialogFinish(*st);
        std::printf("native_dialogs: driver %d (%s)\n", st->answered, st->what.c_str());
        return st->answered;
    };
    // Save As through NSSavePanel
    {
        bool saved = false;
        const int st = answer({.kind = Ans::Kind::Save, .folder = dir.string(), .name = scene.filename().string()},
                              [&] { saved = r.c.fileSaveAs(); });
        std::printf("native_dialogs: save backend %s, scene %s\n", r.ds->lastBackend(), r.v().scenePath.c_str());
        CHECK(saved && st == 1 && std::string(r.ds->lastBackend()) == "native" && fs::exists(scene, ec));
    }
    // Open through NSOpenPanel
    {
        CHECK(r.c.fileNew());
        settle(r);
        bool opened = false;
        const int st = answer({.kind = Ans::Kind::Open, .file = scene.string()}, [&] { opened = r.c.fileOpen(); });
        settle(r);
        CHECK(opened && st == 1 && std::string(r.ds->lastBackend()) == "native" && fs::equivalent(r.v().scenePath, scene, ec));
    }
    // Cancel: nothing opens
    {
        std::optional<std::string> p;
        const int st = answer({.kind = Ans::Kind::Cancel}, [&] { p = r.ds->pickOpenFile("Open Scene", "FUSE level (*.fuselevel)", dir.string()); });
        CHECK(st == 1 && !p && std::string(r.ds->lastBackend()) == "native");
    }
    // folder picker: accepted, then cancelled
    {
        std::optional<std::string> p;
        int st = answer({.kind = Ans::Kind::Folder, .folder = dir.string()}, [&] { p = r.ds->pickDirectory("Project Location", fs::temp_directory_path().string()); });
        std::printf("native_dialogs: folder %s\n", p ? p->c_str() : "(none)");
        CHECK(st == 1 && p && fs::equivalent(*p, dir, ec));
        st = answer({.kind = Ans::Kind::Cancel}, [&] { p = r.ds->pickDirectory("Project Location", dir.string()); });
        CHECK(st == 1 && !p);
    }
    // native colour dialog (NSColorPanel with OK / Cancel), opt-in
    {
        CHECK(r.shell.panels().lightingAddSun().valid());
        settle(r, 4);
        r.ds->options().nativeColorDialog = true;
        bool picked = false;
        int st = answer({.kind = Ans::Kind::Colour, .r = 0.2f, .g = 0.6f, .b = 0.9f}, [&] { picked = r.shell.panels().lightingPickColour("sun"); });
        settle(r, 4);
        const LightingView v = r.shell.panels().lighting();
        std::printf("native_dialogs: NSColorPanel -> %.3f %.3f %.3f\n", v.sunColor[0], v.sunColor[1], v.sunColor[2]);
        CHECK(picked && st == 1 && std::string(r.ds->lastBackend()) == "native");
        CHECK(std::fabs(v.sunColor[0] - 0.2f) < 0.01f && std::fabs(v.sunColor[1] - 0.6f) < 0.01f && std::fabs(v.sunColor[2] - 0.9f) < 0.01f);
        st = answer({.kind = Ans::Kind::Cancel}, [&] { picked = r.shell.panels().lightingPickColour("sun"); });
        CHECK(st == 1 && !picked);
        r.ds->options().nativeColorDialog = false;
    }
#else
    // no zenity here (PATH emptied): the OS dialog is Unavailable and the FUSE UI dialog takes over
    setenv("PATH", "/nonexistent", 1);
    {
        FileScript s{dir.string(), scene.filename().string()};
        scriptFileDialog(r, s);
        CHECK(r.c.fileSaveAs());
        settle(r);
        std::printf("native_dialogs: fallback save ran %d, backend %s, exists %d\n", int(s.ran), r.ds->lastBackend(), int(fs::exists(scene, ec)));
        CHECK(s.ran && std::string(r.ds->lastBackend()) == "fuse" && fs::exists(scene, ec));
    }
    {
        CHECK(r.c.fileNew());
        settle(r);
        FileScript s{dir.string(), scene.filename().string()};
        scriptFileDialog(r, s);
        CHECK(r.c.fileOpen());
        settle(r);
        CHECK(s.ran && std::string(r.ds->lastBackend()) == "fuse" && fs::equivalent(r.v().scenePath, scene, ec));
    }
    {
        CHECK(r.shell.panels().lightingAddSun().valid());
        settle(r, 4);
        r.ds->options().nativeColorDialog = true; // Unavailable: the FUSE colour dialog
        ColorScript ok{"#3399E6", 1};
        scriptColorDialog(r, ok);
        CHECK(r.shell.panels().lightingPickColour("sun"));
        CHECK(ok.ran && std::string(r.ds->lastBackend()) == "fuse");
    }
#endif
    fs::remove_all(dir, ec);
    return 0;
}

// ---- keyboard: Tab order through the editor window ---------------------------------------------------------------------
void gateTabOrder(Rig& r) {
    // walk Tab from the menu bar until it cycles; every stop must be shown, regions must come in reading order
    // (menu bar, toolbar, left dock, centre viewport bar / viewport, right dock, bottom dock), each region visited once
    r.ui().setFocus(r.ui().find("menu.File"));
    settle(r);
    auto regionOf = [&](NodeHandle n) -> int {
        for (NodeHandle a = n; r.ui().alive(a); a = r.ui().parent(a)) {
            const std::string_view id = r.ui().idOf(a);
            if (id == "editor.menubar") return 0;
            if (id == "editor.toolbar") return 1;
            if (id == "dock.left") return 2;
            if (id == "editor.centre") return 3;
            if (id == "dock.right") return 4;
            if (id == "dock.bottom") return 5;
            if (id == "editor.status") return 6;
        }
        return -1;
    };
    std::vector<NodeHandle> stops;
    std::vector<int> regions;
    const NodeHandle first = r.ui().focused();
    for (int i = 0; i < 400; ++i) {
        const NodeHandle f = r.ui().focused();
        if (i > 0 && f == first) break;
        stops.push_back(f);
        regions.push_back(regionOf(f));
        r.ui().keyDown(Key::Tab);
        r.ui().keyUp(Key::Tab);
        r.frames();
    }
    std::string trace;
    int prev = -2, bad = 0, unknown = 0;
    std::vector<int> seen;
    u32 splitters = 0;
    for (std::size_t i = 0; i < stops.size(); ++i) {
        // dock splitters sit between the regions (keyboard resize): stops of their own, not part of a region
        if (r.ui().role(stops[i]) == fuse::ui::A11yRole::Separator) { ++splitters; continue; }
        const int g = regions[i];
        if (g != prev) {
            trace += "\n  [" + std::to_string(g) + "] ";
            if (std::find(seen.begin(), seen.end(), g) != seen.end()) ++bad; // a region entered twice: tab order jumps
            seen.push_back(g);
            prev = g;
        }
        const std::string_view id = r.ui().idOf(stops[i]);
        trace += std::string(id.empty() ? std::string_view(fuse::ui::a11yRoleName(r.ui().role(stops[i]))) : id) + " ";
        unknown += g < 0;
        const Rect rr = r.ui().rect(stops[i]);
        CHECK(rr.w > 0.f && rr.h > 0.f); // never an invisible stop
    }
    std::printf("tab_order: %zu stops (%u splitters), regions:%s\n", stops.size(), splitters, trace.c_str());
    CHECK(stops.size() > 20 && bad == 0 && unknown == 0 && splitters <= 3);
    // reading order of the regions
    std::vector<int> order;
    for (int g : seen)
        if (order.empty() || order.back() != g) order.push_back(g);
    CHECK(std::is_sorted(order.begin(), order.end()));
    // Shift+Tab walks back the same stops
    r.ui().setFocus(stops[5]);
    r.ui().keyDown(Key::Tab, fuse::ui::kModShift);
    r.frames();
    CHECK(r.ui().focused() == stops[4]);
    // a dock's tab bar is a stop and Left / Right switch its panel (the dock title row reaches the panels by keyboard)
    const NodeHandle bar = r.find("tabs.bottom.bar");
    CHECK(std::find(stops.begin(), stops.end(), bar) != stops.end());
    r.ui().setFocus(bar);
    const f32 v0 = r.ui().value(bar);
    r.ui().keyDown(Key::Right);
    r.frames();
    CHECK(r.ui().value(bar) == v0 + 1.f && r.shell.panelVisible("dock.profiler"));
}

} // namespace

int main(int argc, char** argv) {
    std::string gate, out;
    for (int i = 1; i < argc; ++i) {
        if (!std::strncmp(argv[i], "--gate=", 7)) gate = argv[i] + 7;
        else if (!std::strncmp(argv[i], "--out=", 6)) out = argv[i] + 6;
    }
    int rc = 0;
    if (gate == "float_window") rc = gateFloatWindow();
    else if (gate == "native_dialogs") rc = gateNativeDialogs();
    else {
        const std::map<std::string, std::function<void(Rig&)>> gates{
            {"dock_drag", gateDockDrag}, {"dock_split", gateDockSplit}, {"hierarchy_dnd", gateHierarchyDnd}, {"hierarchy_menu", gateHierarchyMenu},
            {"color_dialog", gateColorDialog}, {"tab_order", gateTabOrder}};
        if (!gates.count(gate)) {
            std::fprintf(stderr, "unknown gate '%s'\n", gate.c_str());
            return 2;
        }
        Rig r({.dialogs = gate == "color_dialog"});
        gates.at(gate)(r);
        if (!out.empty()) {
            r.frames();
            r.win.rasterize({});
            if (!r.win.writePng(out)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); ++g_fail; }
        }
    }
    if (rc == 77) return 77;
    if (g_fail || rc) {
        std::fprintf(stderr, "fuse_editor_ui_%s: %d failure(s)\n", gate.c_str(), g_fail);
        return 1;
    }
    std::printf("fuse_editor_ui_%s: OK\n", gate.c_str());
    return 0;
}
