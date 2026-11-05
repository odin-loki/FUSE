// fuse_editor_ui_* gates for the Qt-removal blockers on the panel pages (QT_PARITY.md): the Profiler flame graph and
// timeline key drag retime. Headless on the CPU rasteriser, real pointer / key events through the UiContext.
//   fuse_editor_ui_panels2_gates --gate=<profiler_flame|timeline_drag> [--out=shot.png]
#include "fuse/editor/fui/editor_shell.hpp"
#include "scripted_services.hpp"

#include <fuse/editor/editor_host.hpp>
#include <fuse/profiler/profiler.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>

using namespace fuse::editor;
using fuse::f32;
using fuse::i32;
using fuse::u32;
using fuse::ui::Key;
using fuse::ui::NodeHandle;
using fuse::ui::Rect;
using fuse::ui::Vec2;
namespace w = fuse::ui::widgets;

namespace {
int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Rig {
    fuse::app::Application app;
    fuse::app::AppWindow& win;
    EditorHost host;
    test::ScriptedServices sv;
    EditorController c;
    fui::EditorShellWindow shell;
    Rig()
        : app({.name = "fuse_editor_ui_panels2", .loadSettings = false}),
          win(app.createWindow({.title = "FUSE Editor", .width = 1600, .height = 900})), c(host, sv), shell(win, c) {
        frames(2);
    }
    void frames(int n = 1) {
        for (int i = 0; i < n; ++i) {
            c.tick();
            shell.frame();
            app.step(0.f);
        }
    }
    fuse::ui::UiContext& ui() { return win.ui(); }
    NodeHandle find(std::string_view id) { return ui().find(id); }
    Rect rect(std::string_view id) { return ui().rect(find(id)); }
    void clickId(std::string_view id) {
        const Rect r = rect(id);
        win.click({r.x + r.w * 0.5f, r.y + r.h * 0.5f});
        frames();
    }
};

void spinMs(double ms) {
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() < ms) {}
}

i32 barIndex(const FlameView& f, std::string_view name, i32 parent = -2) {
    for (std::size_t i = 0; i < f.bars.size(); ++i)
        if (f.bars[i].name == name && (parent == -2 || f.bars[i].parent == parent)) return i32(i);
    return -1;
}

// ---- profiler flame graph -------------------------------------------------------------------------------------------
void gateProfilerFlame(Rig& r) {
    namespace prof = fuse::profiler;
    // 1. layout of an explicit capture: A [0, 10 ms] holds B [1, 6] and C [6, 9]; children sorted by name, from the left
    {
        std::vector<prof::ProfileEvent> ev;
        auto add = [&](const char* name, double ms, prof::EventPhase ph, u32 id) {
            prof::ProfileEvent e{};
            e.name = name;
            e.timestampNs = fuse::u64(ms * 1e6);
            e.phase = ph;
            e.scopeId = id;
            ev.push_back(e);
        };
        add("A", 0, prof::EventPhase::Begin, 1);
        add("B", 1, prof::EventPhase::Begin, 2);
        add("B", 6, prof::EventPhase::End, 2);
        add("C", 6, prof::EventPhase::Begin, 3);
        add("C", 9, prof::EventPhase::End, 3);
        add("A", 10, prof::EventPhase::End, 1);
        r.shell.panels().profilerFlameFromEvents(ev);
        const FlameView& f = r.shell.panels().profilerFlame();
        const i32 a = barIndex(f, "A"), b = barIndex(f, "B", a), c = barIndex(f, "C", a);
        CHECK(f.bars.size() == 3 && a == 0 && b > 0 && c > 0 && f.maxDepth == 1);
        if (a >= 0 && b > 0 && c > 0) {
            CHECK(std::fabs(f.bars[a].x0) < 1e-5f && std::fabs(f.bars[a].x1 - 1.f) < 1e-5f && f.bars[a].depth == 0);
            CHECK(std::fabs(f.bars[b].x0) < 1e-5f && std::fabs(f.bars[b].x1 - 0.5f) < 1e-4f && f.bars[b].depth == 1);
            CHECK(std::fabs(f.bars[c].x0 - 0.5f) < 1e-4f && std::fabs(f.bars[c].x1 - 0.8f) < 1e-4f);
            CHECK(std::fabs(f.bars[a].totalMs - 10.f) < 1e-3f && std::fabs(f.bars[a].selfMs - 2.f) < 1e-3f);
            CHECK(std::fabs(f.totalMs - 10.f) < 1e-3f);
        }
    }

    // 2. live capture of real fuse::profiler scopes (gate frames nested inside one another) through the panel
    CHECK(r.shell.showPanel("dock.profiler"));
    r.frames();
    prof::setEnabled(true);
    for (int i = 0; i < 3; ++i) {
        prof::ProfileScope frame("gate.frame");
        {
            prof::ProfileScope update("gate.update");
            prof::ProfileScope physics("gate.physics");
            spinMs(2.0);
        }
        {
            prof::ProfileScope render("gate.render");
            spinMs(1.0);
        }
    }
    const NodeHandle view = r.find("profiler.view");
    CHECK(r.ui().alive(view) && r.ui().alive(r.find("profiler.flame")));
    CHECK(r.rect("profiler.flame").h < 1.f && r.rect("profiler.chart").h > 40.f); // chart first
    r.ui().setValue(view, 1.f, true); // Flame Graph: captures the ring
    r.frames(2);
    CHECK(r.rect("profiler.flame").h > 40.f && r.rect("profiler.chart").h < 1.f);
    const FlameView f = r.shell.panels().profilerFlame();
    std::printf("profiler_flame: %s\n", f.summary.c_str());
    const i32 fr = barIndex(f, "gate.frame"), up = barIndex(f, "gate.update", fr), ph = barIndex(f, "gate.physics", up),
              rd = barIndex(f, "gate.render", fr);
    CHECK(fr >= 0 && up >= 0 && ph >= 0 && rd >= 0);
    if (fr >= 0 && up >= 0 && ph >= 0 && rd >= 0) {
        const FlameBarView &F = f.bars[fr], &U = f.bars[up], &P = f.bars[ph], &R = f.bars[rd];
        std::printf("  gate.frame %.2f ms x%u, update %.2f, physics %.2f, render %.2f\n", F.totalMs, F.calls, U.totalMs, P.totalMs, R.totalMs);
        CHECK(F.calls == 3 && U.calls == 3 && P.calls == 3 && R.calls == 3);
        CHECK(U.depth == F.depth + 1 && P.depth == U.depth + 1 && R.depth == F.depth + 1);
        // spun 2 ms + 1 ms per frame (wall clock; the ring may use a calibrated TSC, so allow ~10%)
        CHECK(P.totalMs >= 5.4f && U.totalMs >= P.totalMs && R.totalMs >= 2.7f && F.totalMs >= U.totalMs + R.totalMs - 0.01f);
        // children inside their parent, siblings side by side
        CHECK(U.x0 >= F.x0 - 1e-5f && R.x1 <= F.x1 + 1e-5f && P.x0 >= U.x0 - 1e-5f && P.x1 <= U.x1 + 1e-5f);
        CHECK(U.x1 <= R.x0 + 1e-5f || R.x1 <= U.x0 + 1e-5f);
        // drawn: one bar per scope, roots at the bottom (a child sits right above its parent)
        const NodeHandle bf = r.find("profiler.flame.bar" + std::to_string(fr)), bu = r.find("profiler.flame.bar" + std::to_string(up));
        CHECK(r.ui().alive(bf) && r.ui().alive(bu));
        const Rect area = r.rect("profiler.flame"), rf = r.ui().rect(bf), ru = r.ui().rect(bu);
        CHECK(rf.w > 2.f && rf.x >= area.x - 1.f && rf.right() <= area.right() + 1.f);
        std::printf("  bars: frame %.0f,%.0f %.0fx%.0f  update %.0f,%.0f %.0fx%.0f  area %.0f,%.0f %.0fx%.0f\n", rf.x, rf.y, rf.w, rf.h, ru.x,
                    ru.y, ru.w, ru.h, area.x, area.y, area.w, area.h);
        CHECK(ru.bottom() <= rf.y + 0.5f && rf.y - ru.bottom() < 5.f && ru.x >= rf.x - 1.f && ru.w <= rf.w + 1.f);
        // click a bar: name, ms and calls in the info line
        r.win.click({ru.x + ru.w * 0.5f, ru.y + ru.h * 0.5f});
        r.frames();
        const std::string info(r.ui().text(r.find("profiler.flame_info")));
        std::printf("  selected: %s\n", info.c_str());
        CHECK(info.rfind("gate.update", 0) == 0 && info.find("ms total") != std::string::npos && info.find("3 calls") != std::string::npos);
    }
    // the editor's own frame is profiled too (shell panel sync scopes)
    CHECK(barIndex(f, "editor.panels") >= 0);
    // Pause freezes the flame graph, Clear empties it; Chart restores the chart
    const u32 captures = r.shell.panels().profilerFlame().captures;
    r.clickId("profiler.pause");
    r.frames(20);
    r.shell.panels().profilerCaptureFlame();
    CHECK(r.shell.panels().profilerFlame().captures == captures);
    r.clickId("profiler.pause");
    r.clickId("profiler.clear");
    CHECK(r.shell.panels().profilerFlame().bars.empty());
    r.frames(20); // the view re-captures twice a second while shown
    CHECK(r.shell.panels().profilerFlame().captures >= 1 && barIndex(r.shell.panels().profilerFlame(), "editor.panels") >= 0);
    r.ui().setValue(view, 0.f, true);
    r.frames();
    CHECK(r.rect("profiler.chart").h > 40.f && r.rect("profiler.flame").h < 1.f);
    r.ui().setValue(view, 1.f, true); // the screenshot shows the flame graph
    r.frames(2);
}

// ---- timeline key drag ----------------------------------------------------------------------------------------------
struct TlGeom {
    Rect c;
    f32 pxPerSec;
    f32 x(f32 sec) const { return c.x + 96.f + sec * pxPerSec; } // kTlHead; view = fitted [0, duration)
    f32 y(u32 lane) const { return c.y + 22.f + f32(lane) * 28.f + 14.f; }
};
TlGeom geom(Rig& r) {
    const Rect c = r.ui().contentRect(r.find("timeline.track"));
    return {c, (c.w - 96.f) / (f32(r.shell.panels().timeline().durationMs) / 1000.f)};
}

void gateTimelineDrag(Rig& r) {
    CHECK(r.shell.showPanel("dock.timeline"));
    r.c.runEntityAction(ContextMenuAction::CreateSdfSphere);
    r.frames(2);
    r.clickId("timeline.key"); // keys at 0
    r.shell.panels().timelineSetPlayhead(1000);
    r.frames();
    r.clickId("timeline.key"); // keys at 1000
    TimelineView tv = r.shell.panels().timeline();
    CHECK(!tv.lanes.empty() && tv.lanes[0].keysMs.size() == 2 && tv.lanes[0].keysMs[1] == 1000);
    CHECK(r.shell.panels().timelineSnapFps() == 30);
    const u32 keysBefore = tv.keyCount;
    const std::string undoBefore = r.c.viewState().undoLabel;

    // drag lane 0's key at 1 s to ~2.51 s: live while dragging, snapped to 30 fps (2500 ms), one undo step
    TlGeom g = geom(r);
    r.ui().pointerMove({g.x(1.f), g.y(0)});
    r.ui().pointerDown({g.x(1.f), g.y(0)});
    r.frames();
    CHECK(r.shell.panels().timelineKeyDragging());
    CHECK(r.shell.panels().timeline().selTrack == 0 && r.shell.panels().timeline().selKey == 1);
    r.ui().pointerMove({g.x(1.8f), g.y(0)});
    r.frames();
    const i32 mid = r.shell.panels().timelineKeyTime(0, 1);
    std::printf("timeline_drag: mid-drag key at %d ms\n", mid);
    CHECK(mid == 1800); // 1.8 s = frame 54 at 30 fps
    CHECK(r.c.viewState().undoLabel == undoBefore); // nothing committed mid-drag
    r.ui().pointerMove({g.x(2.51f), g.y(0)});
    r.frames();
    r.ui().pointerUp({g.x(2.51f), g.y(0)});
    r.frames();
    tv = r.shell.panels().timeline();
    std::printf("timeline_drag: dropped at %d ms, undo '%s'\n", tv.lanes[0].keysMs[1], r.c.viewState().undoLabel.c_str());
    CHECK(!r.shell.panels().timelineKeyDragging());
    CHECK(tv.lanes[0].keysMs[1] == 2500 && tv.keyCount == keysBefore);
    CHECK(r.host.undoStack().peekUndoDescription() == "Move key");
    // the widget shows the key where the model has it
    const Vec2 clip = w::timelineClip(r.ui(), r.find("timeline.track"), 0, 1);
    CHECK(std::fabs((clip.x + clip.y) * 0.5f - 2.5f) < 0.01f);
    // one undo restores 1000 (not an intermediate time), redo reapplies
    CHECK(r.c.trigger(ActionId::EditUndo));
    r.frames();
    CHECK(r.shell.panels().timeline().lanes[0].keysMs[1] == 1000);
    CHECK(r.c.trigger(ActionId::EditRedo));
    r.frames();
    CHECK(r.shell.panels().timeline().lanes[0].keysMs[1] == 2500);

    // Shift: no snapping (ms precision)
    g = geom(r);
    r.ui().setModifiers(fuse::ui::kModShift);
    r.ui().pointerMove({g.x(2.5f), g.y(0)});
    r.ui().pointerDown({g.x(2.5f), g.y(0)});
    r.ui().pointerMove({g.x(2.517f), g.y(0)});
    r.ui().pointerUp({g.x(2.517f), g.y(0)});
    r.ui().setModifiers(0);
    r.frames();
    const i32 free = r.shell.panels().timeline().lanes[0].keysMs[1];
    std::printf("timeline_drag: Shift drop at %d ms\n", free);
    CHECK(std::abs(free - 2517) <= 2 && free != 2500 && free != 2533);
    CHECK(r.c.trigger(ActionId::EditUndo));
    r.frames();
    CHECK(r.shell.panels().timeline().lanes[0].keysMs[1] == 2500);

    // Escape cancels a drag: the key returns, nothing new on the undo stack
    const std::string undoNow = r.c.viewState().undoLabel;
    g = geom(r);
    r.ui().pointerMove({g.x(2.5f), g.y(0)});
    r.ui().pointerDown({g.x(2.5f), g.y(0)});
    r.ui().pointerMove({g.x(4.f), g.y(0)});
    r.frames();
    CHECK(r.shell.panels().timelineKeyTime(0, 1) == 4000);
    if (r.ui().focused() != r.find("timeline.track")) r.ui().setFocus(r.find("timeline.track"));
    r.ui().keyDown(Key::Escape);
    r.ui().keyUp(Key::Escape);
    r.frames();
    r.ui().pointerUp({g.x(4.f), g.y(0)});
    r.frames();
    CHECK(!r.shell.panels().timelineKeyDragging());
    CHECK(r.shell.panels().timeline().lanes[0].keysMs[1] == 2500 && r.c.viewState().undoLabel == undoNow);

    // a drag that ends where it started records nothing
    g = geom(r);
    r.ui().pointerMove({g.x(2.5f), g.y(0)});
    r.ui().pointerDown({g.x(2.5f), g.y(0)});
    r.ui().pointerMove({g.x(2.505f), g.y(0)});
    r.ui().pointerUp({g.x(2.505f), g.y(0)});
    r.frames();
    CHECK(r.shell.panels().timeline().lanes[0].keysMs[1] == 2500 && r.c.viewState().undoLabel == undoNow);

    // dragging past another key reorders the lane (the key keeps its identity: the selection follows it)
    g = geom(r);
    r.ui().pointerMove({g.x(2.5f), g.y(0)});
    r.ui().pointerDown({g.x(2.5f), g.y(0)});
    r.ui().pointerMove({g.x(0.0f), g.y(0)}); // over the key at 0 (it comes back once the drag moves on)
    r.frames();
    r.ui().pointerMove({g.x(0.5f), g.y(0)});
    r.ui().pointerUp({g.x(0.5f), g.y(0)});
    r.frames();
    tv = r.shell.panels().timeline();
    std::printf("timeline_drag: lane 0 keys after reorder drag:");
    for (i32 k : tv.lanes[0].keysMs) std::printf(" %d", k);
    std::printf("\n");
    CHECK(tv.lanes[0].keysMs.size() == 2 && tv.lanes[0].keysMs[0] == 0 && tv.lanes[0].keysMs[1] == 500);
    CHECK(r.c.trigger(ActionId::EditUndo));
    r.frames();
    CHECK(r.shell.panels().timeline().lanes[0].keysMs[1] == 2500);

    // keyboard (widget Ctrl+Left / Right on the selected clip) retimes the key by a frame: one undo step each
    g = geom(r);
    r.ui().pointerMove({g.x(2.5f), g.y(0)});
    r.ui().pointerDown({g.x(2.5f), g.y(0)}); // selects the clip (a click, no move)
    r.ui().pointerUp({g.x(2.5f), g.y(0)});
    r.frames();
    r.ui().setFocus(r.find("timeline.track"));
    r.ui().keyDown(Key::Right, fuse::ui::kModCtrl);
    r.ui().keyUp(Key::Right, fuse::ui::kModCtrl);
    r.frames();
    const i32 kb = r.shell.panels().timeline().lanes[0].keysMs[1];
    std::printf("timeline_drag: Ctrl+Right -> %d ms, undo '%s'\n", kb, r.host.undoStack().peekUndoDescription().c_str());
    CHECK(kb == 2533 && r.host.undoStack().peekUndoDescription() == "Move key");
    CHECK(r.c.trigger(ActionId::EditUndo));
    r.frames();
    CHECK(r.shell.panels().timeline().lanes[0].keysMs[1] == 2500);
}
} // namespace

int main(int argc, char** argv) {
    std::string gate, out;
    for (int i = 1; i < argc; ++i) {
        if (!std::strncmp(argv[i], "--gate=", 7)) gate = argv[i] + 7;
        else if (!std::strncmp(argv[i], "--out=", 6)) out = argv[i] + 6;
    }
    Rig r;
    const std::map<std::string, std::function<void(Rig&)>> gates{{"profiler_flame", gateProfilerFlame}, {"timeline_drag", gateTimelineDrag}};
    if (!gates.count(gate)) {
        std::fprintf(stderr, "unknown gate '%s'\n", gate.c_str());
        return 2;
    }
    gates.at(gate)(r);
    if (!out.empty()) {
        r.frames();
        r.win.rasterize({});
        if (!r.win.writePng(out)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); ++g_fail; }
    }
    if (g_fail) { std::fprintf(stderr, "fuse_editor_ui_%s: %d failure(s)\n", gate.c_str(), g_fail); return 1; }
    std::printf("fuse_editor_ui_%s: OK\n", gate.c_str());
    return 0;
}
