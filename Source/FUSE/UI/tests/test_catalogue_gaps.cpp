// fuse_ui_catalogue_gaps: behaviour gates for the public widget functions no other gate called (found by grepping
// the tests for every function of widgets.hpp / platform_feel.hpp): doubleSpinBox, standalone tabBar, closePopup /
// closeDialog / closePopover, timelineSetView, viewportSetImage / overlay, nodeGraphDisconnect, haptic,
// defaultThemeCss, and the touch-drag hooks of gridview / header / video (table, text and code edit are in
// fuse_ui_touch_scroll). Also the gates of two fixes: context menus on widgets that use the right press themselves
// (context_menu_views) and the default theme's look of disabled inputs (disabled_theme).
// `--gate=<name>` runs one group (ctest fuse_ui_gap_<name>).
#include "test_util.hpp"

#include "fuse/ui/widgets/modelview.hpp"
#include "fuse/ui/widgets/platform_feel.hpp"

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
bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::abs(a - b) <= eps; }
struct Fixture {
    UiContext ui{{.viewport = {900, 640}, .font = &font()}};
    Fixture() { w::install(ui); }
    void key(Key k, u8 mods = 0) {
        ui.keyDown(k, mods);
        ui.keyUp(k, mods);
        ui.update(1.f / 60.f);
    }
    void click(Vec2 p) {
        ui.pointerMove(p);
        ui.pointerDown(p);
        ui.pointerUp(p);
        ui.update(1.f / 60.f);
    }
    /// One finger from `a` to `b` in `steps` moves; the finger stays down.
    void touchDrag(Vec2 a, Vec2 b, double& t, int steps = 6) {
        ui.touchDown(a, t);
        ui.update(1.f / 60.f);
        for (int i = 1; i <= steps; ++i) {
            const f32 k = f32(i) / f32(steps);
            ui.touchMove({a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k}, t += 0.016);
            ui.update(1.f / 60.f);
        }
    }
};

// ---- double spin box ------------------------------------------------------------------------------------------------
void gateDoubleSpinBox() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle sb = w::doubleSpinBox(ui, ui.root(), 0.f, 1.f, 0.5f, 0.1f, "gain");
    const NodeHandle def = w::doubleSpinBox(ui, ui.root(), -2.f, 2.f, 0.f, 0.f, "def"); // step <= 0: the default 0.1
    const NodeHandle fine = w::doubleSpinBox(ui, ui.root(), 0.f, 1.f, 0.25f, 0.05f, "fine");
    int changes = 0;
    ui.connect(sb, SignalKind::ValueChanged, [&changes](UiContext&, NodeHandle, const SignalArgs&) { ++changes; });
    ui.update();
    UI_CHECK(ui.role(sb) == A11yRole::SpinBox && near(ui.rangeStep(sb), 0.1f) && near(ui.rangeStep(def), 0.1f));
    UI_CHECK(near(ui.rangeMin(sb), 0.f) && near(ui.rangeMax(sb), 1.f) && near(ui.value(sb), 0.5f));
    // keys step by the fractional step and clamp at the range
    ui.setFocus(sb, FocusReason::Keyboard);
    f.key(Key::Up);
    UI_CHECK(near(ui.value(sb), 0.6f) && changes == 1);
    f.key(Key::Down);
    f.key(Key::Down);
    UI_CHECK(near(ui.value(sb), 0.4f) && changes == 3);
    for (int i = 0; i < 12; ++i) f.key(Key::Up);
    UI_CHECK(near(ui.value(sb), 1.f));
    const int atTop = changes;
    f.key(Key::Up);
    UI_CHECK(near(ui.value(sb), 1.f) && changes == atTop); // clamped: no signal
    // the arrow buttons at the right end, and the wheel
    const Rect c = ui.contentRect(sb);
    f.click({c.right() - 10.f, c.y + c.h * 0.75f});
    UI_CHECK(near(ui.value(sb), 0.9f));
    f.click({c.right() - 10.f, c.y + c.h * 0.25f});
    UI_CHECK(near(ui.value(sb), 1.f));
    ui.wheel(ctr(ui.rect(sb)), {0.f, -1.f});
    ui.update();
    UI_CHECK(near(ui.value(sb), 0.9f));
    ui.setFocus(def, FocusReason::Keyboard);
    f.key(Key::Down);
    UI_CHECK(near(ui.value(def), -0.1f));
    ui.setFocus(fine, FocusReason::Keyboard);
    f.key(Key::Up);
    UI_CHECK(near(ui.value(fine), 0.3f));
    // the value is shown with the step's precision (1 decimal for 0.1, 2 for 0.05): the 0.05 box paints one more glyph
    auto glyphs = [&](NodeHandle n) {
        u32 g = 0;
        for (const DrawCmd& d : ui.drawList().cmds()) g += d.op == DrawOp::Glyph && d.node == n.index;
        return g;
    };
    ui.update();
    UI_CHECK_EQ(glyphs(sb), 3u);   // "0.9"
    UI_CHECK_EQ(glyphs(fine), 4u); // "0.30"
    ui.setDisabled(sb, true);
    f.click({c.right() - 10.f, c.y + c.h * 0.25f});
    UI_CHECK(near(ui.value(sb), 0.9f));
}

// ---- standalone tab bar ---------------------------------------------------------------------------------------------
void gateTabBar() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle bar = w::tabBar(ui, ui.root(), "General\nRender\nAudio", 1, "tabs");
    int changes = 0;
    f32 last = -1.f;
    ui.connect(bar, SignalKind::ValueChanged, [&changes, &last](UiContext&, NodeHandle, const SignalArgs& a) { ++changes; last = a.value; });
    ui.update();
    UI_CHECK(ui.role(bar) == A11yRole::TabList && ui.value(bar) == 1.f && changes == 0);
    const Rect c = ui.contentRect(bar);
    f.click({c.x + 8.f, ctr(c).y}); // the first tab
    UI_CHECK(ui.value(bar) == 0.f && changes == 1 && last == 0.f);
    f.click({c.x + 8.f, ctr(c).y}); // already current: no signal
    UI_CHECK_EQ(changes, 1);
    UI_CHECK(ui.focused() == bar);
    f.key(Key::Right);
    f.key(Key::Right);
    UI_CHECK(ui.value(bar) == 2.f && changes == 3);
    f.key(Key::Right); // no wrap
    UI_CHECK(ui.value(bar) == 2.f && changes == 3);
    f.key(Key::Left);
    UI_CHECK(ui.value(bar) == 1.f && last == 1.f);
    f.click({c.right() - 2.f, ctr(c).y}); // right of the last tab: nothing
    UI_CHECK(ui.value(bar) == 1.f);
}

// ---- closing popups by API ------------------------------------------------------------------------------------------
void gateClose() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle owner = w::button(ui, ui.root(), "Menu", "owner");
    ui.update();
    ui.setFocus(owner);
    // popup menu
    const NodeHandle scope = w::popupMenu(ui, owner, "One\nTwo", {40.f, 60.f}, "menu");
    ui.update();
    UI_CHECK(w::popupOpen(ui, scope) && ui.focused() != owner);
    w::closePopup(ui, ui.firstChild(scope)); // any node inside the scope names it
    UI_CHECK(ui.focused() == owner);             // the focus is back at once
    ui.update();
    UI_CHECK(!w::popupOpen(ui, scope) && !ui.alive(scope)); // destroyed on the next update
    w::closePopup(ui, scope);   // closing a dead scope is a no-op
    f.click(ctr(ui.rect(owner)));
    UI_CHECK(ui.focused() == owner); // the modal trap is gone
    // dialog: hidden, reusable
    const NodeHandle dlg = w::dialog(ui, "Delete asset?", "dlg");
    const NodeHandle yes = w::button(ui, w::dialogBody(ui, dlg), "Delete", "yes");
    w::openDialog(ui, dlg);
    ui.update();
    UI_CHECK(w::popupOpen(ui, ui.parent(dlg)) && ui.computed(yes).visible);
    w::closeDialog(ui, dlg);
    ui.update();
    UI_CHECK(!w::popupOpen(ui, ui.parent(dlg)) && ui.alive(dlg) && ui.alive(yes) && !ui.computed(ui.parent(dlg)).visible);
    UI_CHECK(ui.focused() == owner);
    w::closeDialog(ui, dlg); // twice: no-op
    w::openDialog(ui, dlg);
    ui.update();
    UI_CHECK(w::popupOpen(ui, ui.parent(dlg)));
    w::closeDialog(ui, dlg);
    ui.update();
    // popover
    const NodeHandle pop = w::popover(ui, owner, "pop");
    w::label(ui, pop, "Details");
    ui.update();
    const NodeHandle pscope = ui.parent(pop);
    UI_CHECK(w::popupOpen(ui, pscope) && ui.rect(pop).y >= ui.rect(owner).bottom() - 1.f);
    w::closePopover(ui, pop);
    UI_CHECK(ui.focused() == owner);
    ui.update();
    UI_CHECK(!w::popupOpen(ui, pscope) && !ui.alive(pop));
}

// ---- timeline view range --------------------------------------------------------------------------------------------
void gateTimelineView() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle tl = w::timeline(ui, ui.root(), 20.f, "tl");
    ui.setCss(tl, "width", "800px");
    ui.setCss(tl, "height", "160px");
    const u32 tr = w::timelineAddTrack(ui, tl, "Camera");
    w::timelineAddClip(ui, tl, tr, 2.f, 6.f, "Shot", Color::rgb(0x3D8BFD));
    ui.update();
    UI_CHECK(w::timelineView(ui, tl).y == 0.f); // free zoom by default
    w::timelineSetView(ui, tl, 2.f, 8.f);
    ui.update();
    UI_CHECK(near(w::timelineView(ui, tl).x, 2.f) && near(w::timelineView(ui, tl).y, 8.f));
    // with a range set the ruler maps the lane width onto it: a click in the middle of the lane scrubs to start + span/2
    const Rect r = ui.rect(tl);
    const f32 laneX = r.x + 96.f, laneW = r.w - 96.f; // (the track-name column is 96 px)
    ui.pointerMove({laneX + laneW * 0.5f, r.y + 10.f});
    ui.pointerDown({laneX + laneW * 0.5f, r.y + 10.f});
    ui.pointerUp({laneX + laneW * 0.5f, r.y + 10.f});
    ui.update();
    UI_CHECK(near(ui.value(tl), 6.f, 0.15f));
    // Shift+wheel pans, Ctrl+wheel zooms about the pointer; both report through Event {value = 1}
    int events = 0;
    ui.connect(tl, SignalKind::Event, [&events](UiContext&, NodeHandle, const SignalArgs& a) { events += a.event == nullptr && a.value == 1.f; });
    ui.setModifiers(kModShift);
    ui.wheel({laneX + laneW * 0.5f, r.y + 60.f}, {0.f, -1.f});
    ui.update();
    const Vec2 panned = w::timelineView(ui, tl);
    UI_CHECK(!near(panned.x, 2.f) && near(panned.y, 8.f) && events == 1);
    ui.setModifiers(kModCtrl);
    ui.wheel({laneX + laneW * 0.5f, r.y + 60.f}, {0.f, 1.f});
    ui.update();
    const Vec2 zoomed = w::timelineView(ui, tl);
    UI_CHECK(zoomed.y < 8.f && zoomed.y > 0.f && events == 2);
    UI_CHECK(near(zoomed.x + zoomed.y * 0.5f, panned.x + panned.y * 0.5f, 0.05f)); // about the pointer (lane centre)
    ui.setModifiers(0);
    // the view never leaves [0, duration]
    w::timelineSetView(ui, tl, 18.f, 8.f);
    UI_CHECK(w::timelineView(ui, tl).x + w::timelineView(ui, tl).y <= 20.f + 1e-3f);
    w::timelineSetView(ui, tl, 0.f, 0.f); // span 0: back to free zoom
    UI_CHECK(w::timelineView(ui, tl).y == 0.f);
}

// ---- viewport image / overlay ---------------------------------------------------------------------------------------
void gateViewport() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle vp = w::viewportWidget(ui, ui.root(), 3, 200, 100, "vp");
    ui.setCss(vp, "width", "400px");
    ui.setCss(vp, "height", "400px");
    ui.update();
    const Rect wide = w::viewportImageRect(ui, vp); // 2:1 letterboxed into the square
    UI_CHECK(near(wide.w, 2.f * wide.h, 1.f) && near(wide.w, ui.contentRect(vp).w, 1.f));
    auto imageCmd = [&]() -> const DrawCmd* {
        for (const DrawCmd& d : ui.drawList().cmds())
            if (d.op == DrawOp::Image && d.node == vp.index) return &d;
        return nullptr;
    };
    UI_CHECK(imageCmd() && imageCmd()->image == 3u);
    w::viewportSetImage(ui, vp, 7, 100, 200); // a new target: index and aspect follow
    ui.update();
    const Rect tall = w::viewportImageRect(ui, vp);
    UI_CHECK(near(tall.h, 2.f * tall.w, 1.f) && near(tall.h, ui.contentRect(vp).h, 1.f));
    UI_CHECK(imageCmd() && imageCmd()->image == 7u && near(imageCmd()->rect.w, tall.w, 1.f));
    // input is forwarded in pixels of the new image
    ui.pointerMove(ctr(tall));
    ui.update();
    UI_CHECK(near(w::viewportLastInput(ui, vp).pos.x, 50.f, 1.f) && near(w::viewportLastInput(ui, vp).pos.y, 100.f, 1.f));
    // overlay: stored, and painted over the image as part of the draw list
    UI_CHECK(w::viewportOverlay(ui, vp).lines.empty() && w::viewportOverlay(ui, vp).badge.empty());
    const std::size_t plain = ui.drawList().size();
    w::ViewportOverlay ov;
    ov.lines.push_back({{0.f, 0.f}, {100.f, 200.f}, Color::rgb(0xFF0000), 2.f});
    ov.dots.push_back({{50.f, 100.f}, 5.f, Color::rgb(0x00FF00)});
    ov.frame = Color::rgb(0x33CC66);
    ov.frameWidth = 3.f;
    ov.badge = "PLAYING";
    ov.badgeColor = Color::rgb(0x33CC66);
    ov.badgeText = Color::rgb(0x000000);
    w::viewportSetOverlay(ui, vp, ov);
    ui.update();
    UI_CHECK(ui.stats().repainted && ui.drawList().size() > plain + 3);
    const w::ViewportOverlay& got = w::viewportOverlay(ui, vp);
    UI_CHECK(got.lines.size() == 1 && got.dots.size() == 1 && got.badge == "PLAYING" && got.frameWidth == 3.f);
    bool line = false;
    for (const DrawCmd& d : ui.drawList().cmds()) // the line runs corner to corner of the letterboxed image
        line = line || (d.op == DrawOp::Line && near(d.rect.x, tall.x, 1.f) && near(d.rect.y, tall.y, 1.f));
    UI_CHECK(line);
    w::viewportSetOverlay(ui, vp, {});
    ui.update();
    UI_CHECK_EQ(ui.drawList().size(), plain);
    UI_CHECK(w::viewportOverlay(ui, ui.root()).lines.empty()); // not a viewport: an empty overlay
}

// ---- node graph: removing a link ------------------------------------------------------------------------------------
void gateNodeGraphDisconnect() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle g = w::nodeGraph(ui, ui.root(), "graph");
    const w::GraphPin in[] = {{"a", 1}, {"b", 1}}, out[] = {{"out", 1}};
    const u32 n0 = w::nodeGraphAddNode(ui, g, "Const", {20, 20}, {}, out);
    const u32 n1 = w::nodeGraphAddNode(ui, g, "Add", {220, 20}, in, out);
    int changes = 0;
    f32 last = -1.f;
    ui.connect(g, SignalKind::ValueChanged, [&changes, &last](UiContext&, NodeHandle, const SignalArgs& a) { ++changes; last = a.value; });
    UI_CHECK(w::nodeGraphConnect(ui, g, n0, 0, n1, 0) && w::nodeGraphConnect(ui, g, n0, 0, n1, 1));
    UI_CHECK_EQ(w::nodeGraphLinkCount(ui, g), 2u);
    const int before = changes;
    UI_CHECK(w::nodeGraphDisconnect(ui, g, n1, 0));
    UI_CHECK(w::nodeGraphLinkCount(ui, g) == 1u && changes == before + 1 && last == 1.f);
    UI_CHECK(!w::nodeGraphDisconnect(ui, g, n1, 0)); // nothing on that pin any more
    UI_CHECK(!w::nodeGraphDisconnect(ui, g, n1, 7) && !w::nodeGraphDisconnect(ui, g, 99, 0));
    UI_CHECK(w::nodeGraphLinkCount(ui, g) == 1u && changes == before + 1);
    UI_CHECK(w::nodeGraphConnect(ui, g, n0, 0, n1, 0)); // the freed input takes a link again
    UI_CHECK(w::nodeGraphDisconnect(ui, g, n1, 1) && w::nodeGraphDisconnect(ui, g, n1, 0));
    UI_CHECK_EQ(w::nodeGraphLinkCount(ui, g), 0u);
    UI_CHECK_EQ(w::nodeGraphTopoOrder(ui, g).size(), 2u);
}

// ---- haptics hook, theme text ---------------------------------------------------------------------------------------
void gateHapticTheme() {
    static std::vector<w::HapticKind> got;
    static int marker = 0;
    got.clear();
    w::setHapticsHandler(nullptr, nullptr);
    w::haptic(w::HapticKind::Tick); // no sink: a no-op
    w::setHapticsHandler([](void* user, w::HapticKind k) { if (user == &marker) got.push_back(k); }, &marker);
    w::haptic(w::HapticKind::Tick);
    w::haptic(w::HapticKind::Confirm);
    w::haptic(w::HapticKind::Reject);
    UI_CHECK(got.size() == 3 && got[0] == w::HapticKind::Tick && got[1] == w::HapticKind::Confirm && got[2] == w::HapticKind::Reject);
    w::setHapticsHandler(nullptr, nullptr);
    w::haptic(w::HapticKind::Toggle);
    UI_CHECK_EQ(got.size(), std::size_t(3));

    // the default theme is a clean style sheet (no diagnostics) with the tokens the widgets read
    const std::string_view css = w::defaultThemeCss();
    UI_CHECK(css.find(":root") != std::string_view::npos && css.find("--accent") != std::string_view::npos);
    UiContext ui{{.viewport = {320, 200}, .font = &font()}};
    w::registerTierA(ui);
    std::vector<StyleDiagnostic> diags;
    UI_CHECK(ui.addStyleSheet(css, &diags).has_value());
    for (const StyleDiagnostic& d : diags) std::fprintf(stderr, "theme: line %u: %s\n", unsigned(d.line), d.message.c_str());
    UI_CHECK(diags.empty());
    const NodeHandle b = w::button(ui, ui.root(), "Themed");
    ui.update();
    UI_CHECK(ui.computed(b).background.a > 0.f && ui.computed(b).color.a > 0.f);
}

// ---- touch-drag hooks: grid view, header, video ---------------------------------------------------------------------
void gateTouchHooks() {
    double t = 0.0;
    { // grid view: tile rows follow the finger; a grid that fits lets its scroll area pan instead
        Fixture f;
        UiContext& ui = f.ui;
        w::StringTableModel m("name");
        for (int i = 0; i < 300; ++i) m.addRow("tile " + std::to_string(i));
        const NodeHandle grid = w::gridView(ui, ui.root(), &m.model(), 100.f, 80.f, "grid");
        ui.setCss(grid, "width", "420px");
        ui.setCss(grid, "height", "250px");
        ui.update();
        UI_CHECK(w::gridColumns(ui, grid) >= 3u && w::gridFirstVisibleRow(ui, grid) == 0u);
        const Vec2 c = ctr(ui.rect(grid));
        f.touchDrag({c.x, c.y + 100.f}, {c.x, c.y + 100.f - 248.f}, t); // slop 8 + three rows of 80
        UI_CHECK(ui.touchPanning() && ui.touchPanTarget() == grid);
        UI_CHECK_EQ(w::gridFirstVisibleRow(ui, grid), 3u);
        ui.touchUp({c.x, c.y - 148.f}, t += 0.2);
        ui.update(1.f / 60.f);
        UI_CHECK(ui.value(grid) >= 0.f); // (like lists and tables, the press selects the tile the pan starts on)
        const f32 sel = ui.value(grid);
        ui.stopFling();
        f.touchDrag({c.x, c.y}, {c.x, c.y + 400.f}, t += 1.0); // back down: stops at the first row
        ui.touchUp({c.x, c.y + 400.f}, t += 0.2);
        ui.stopFling();
        UI_CHECK_EQ(w::gridFirstVisibleRow(ui, grid), 0u);
        ui.touchDown(c, t += 1.0); // a tap still selects a tile
        ui.touchUp(c, t += 0.05);
        ui.update(1.f / 60.f);
        UI_CHECK(ui.value(grid) >= 0.f && ui.value(grid) != sel);
    }
    { // header: a drag on a section edge resizes (no pan); a drag on a section pans the scroll area around it
        Fixture f;
        UiContext& ui = f.ui;
        const NodeHandle area = w::scrollArea(ui, ui.root(), "area");
        ui.setCss(area, "height", "200px");
        const NodeHandle hdr = w::headerView(ui, area, "Name\nSize\nType", 120.f, "hdr");
        for (int i = 0; i < 30; ++i) w::label(ui, area, "row");
        ui.update();
        const Rect hc = ui.contentRect(hdr);
        const f32 edge = hc.x + 120.f; // right edge of "Name"
        f.touchDrag({edge, ctr(hc).y}, {edge + 60.f, ctr(hc).y + 3.f}, t += 1.0);
        UI_CHECK(!ui.touchPanning());
        UI_CHECK(near(w::headerSectionWidth(ui, hdr, 0), 180.f, 1.f) && near(w::headerSectionWidth(ui, hdr, 1), 120.f));
        ui.touchUp({edge + 60.f, ctr(hc).y + 3.f}, t += 0.2);
        ui.update(1.f / 60.f);
        UI_CHECK(w::headerSortSection(ui, hdr) == -1); // an edge drag does not sort
        const f32 w0 = w::headerSectionWidth(ui, hdr, 0);
        f.touchDrag({hc.x + 40.f, ctr(hc).y}, {hc.x + 40.f, ctr(hc).y - 120.f}, t += 1.0);
        UI_CHECK(ui.touchPanning() && ui.touchPanTarget() == area && ui.scroll(area).y > 60.f);
        UI_CHECK(near(w::headerSectionWidth(ui, hdr, 0), w0));
        ui.touchUp({hc.x + 40.f, ctr(hc).y - 120.f}, t += 0.2);
        ui.stopFling();
    }
    { // video: a drag that starts on the seek bar scrubs (no pan); a drag on the picture pans the scroll area
        Fixture f;
        UiContext& ui = f.ui;
        const NodeHandle area = w::scrollArea(ui, ui.root(), "area");
        ui.setCss(area, "height", "300px");
        const NodeHandle vid = w::videoWidget(ui, area, &w::testPatternSource(), "vid");
        for (int i = 0; i < 30; ++i) w::label(ui, area, "row");
        ui.update();
        const Rect c = ui.contentRect(vid);
        const f32 tx = c.x + 30.f, tw = c.w - 100.f, by = c.bottom() - 13.f; // the track inside the 26 px bar
        const u32 frames = w::testPatternSource().frames;
        UI_CHECK(frames > 10u && w::videoFrame(ui, vid) == 0u);
        f.touchDrag({tx + tw * 0.25f, by}, {tx + tw * 0.75f, by - 2.f}, t += 1.0);
        UI_CHECK(!ui.touchPanning() && ui.scroll(area).y == 0.f);
        UI_CHECK(near(f32(w::videoFrame(ui, vid)), 0.75f * f32(frames - 1), 1.5f));
        ui.touchUp({tx + tw * 0.75f, by - 2.f}, t += 0.2);
        ui.update(1.f / 60.f);
        const u32 at = w::videoFrame(ui, vid);
        const bool playing = w::videoPlaying(ui, vid);
        f.touchDrag({ctr(c).x, c.y + 120.f}, {ctr(c).x, c.y + 20.f}, t += 1.0);
        UI_CHECK(ui.touchPanning() && ui.touchPanTarget() == area && ui.scroll(area).y > 50.f);
        UI_CHECK(w::videoFrame(ui, vid) == at || w::videoPlaying(ui, vid) != playing); // the pan did not scrub
        ui.touchUp({ctr(c).x, c.y + 20.f}, t += 0.2);
        ui.stopFling();
    }
}
// ---- context menus on widgets that handle the pointer themselves ----------------------------------------------------
// setContextMenu listens to the event signal, which a widget's own handler used to cut off by consuming the right
// press (list, tree, table, grid). A right press now reaches it everywhere; the views still make the row current.
struct MenuProbe {
    UiContext& ui;
    u32 popupCls, itemCls;
    u32 openMenus() const {
        u32 n = 0;
        for (u32 i = 0; i < 8192; ++i) {
            const NodeHandle h = ui.handleOf(i);
            n += ui.alive(h) && ui.classOf(h) == popupCls && ui.computed(h).visible;
        }
        return n;
    }
    NodeHandle item(std::string_view label) const {
        for (u32 i = 0; i < 8192; ++i) {
            const NodeHandle h = ui.handleOf(i);
            if (ui.alive(h) && ui.classOf(h) == itemCls && ui.text(h) == label) return h;
        }
        return {};
    }
    void rightClick(Vec2 p) {
        ui.pointerMove(p);
        ui.pointerDown(p, MouseButton::Right);
        ui.pointerUp(p, MouseButton::Right);
        ui.update(1.f / 60.f);
    }
    void dismiss() {
        ui.keyDown(Key::Escape);
        for (int i = 0; i < 30; ++i) ui.update(1.f / 30.f); // (popups leave after their exit delay)
    }
};
void gateContextMenuViews() {
    Fixture f;
    UiContext& ui = f.ui;
    ui.setCss(ui.root(), "gap", "4px");
    MenuProbe probe{ui, ui.registry().find("popup"), ui.registry().find("menuitem")};
    w::StringTableModel m("Name\tSize");
    for (int i = 0; i < 40; ++i) m.addRow("file" + std::to_string(i) + "\t" + std::to_string(i));
    w::StringTreeModel tm("Name");
    const w::ItemId root = tm.add(w::kRootItem, "Root");
    tm.add(root, "Leaf");
    const NodeHandle row = w::panel(ui, ui.root(), "row");
    ui.setCss(row, "flex-direction", "row");
    ui.setCss(row, "gap", "4px");
    const NodeHandle row2 = w::panel(ui, ui.root(), "row2");
    ui.setCss(row2, "flex-direction", "row");
    ui.setCss(row2, "gap", "4px");
    struct Case { const char* name; NodeHandle node; bool view; };
    std::vector<Case> cases;
    auto sized = [&](NodeHandle n, const char* wd = "200px", const char* ht = "120px") {
        ui.setCss(n, "width", wd);
        ui.setCss(n, "height", ht);
        return n;
    };
    cases.push_back({"listview", sized(w::listView(ui, row, "one\ntwo\nthree\nfour", 0, "list")), true});
    cases.push_back({"treeview", sized(w::treeView(ui, row, "Scene\n\tCamera\n\tLights\nAssets", "tree")), true});
    cases.push_back({"tableview", sized(w::tableView(ui, row, &m.model(), "table"), "280px"), true});
    cases.push_back({"modellistview", sized(w::modelListView(ui, row, &m.model(), "mlist"), "160px"), true});
    cases.push_back({"gridview", sized(w::gridView(ui, row2, &m.model(), 60.f, 40.f, "grid"), "260px"), true});
    cases.push_back({"treemodelview", sized(w::treeModelView(ui, row2, &tm.model(), "mtree")), true});
    // the widgets a context menu worked on before still have theirs
    cases.push_back({"button", w::button(ui, row2, "Button", "button"), false});
    cases.push_back({"lineedit", w::lineEdit(ui, row2, "text", "edit"), false});
    cases.push_back({"checkbox", w::checkBox(ui, row2, "Check", false, "check"), false});
    cases.push_back({"slider", sized(w::slider(ui, row2, 0.f, 1.f, 0.5f, 0.f, "slider"), "100px", "20px"), false});
    cases.push_back({"spinbox", w::spinBox(ui, row2, 0.f, 9.f, 3.f, 1.f, "spin"), false});
    cases.push_back({"combobox", w::comboBox(ui, row2, "a\nb", 0, "combo"), false});
    std::vector<int> picked(cases.size(), -1);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        w::setContextMenu(ui, cases[i].node, "Rename\nDelete");
        ui.connect(cases[i].node, SignalKind::Submitted, [&picked, i](UiContext&, NodeHandle, const SignalArgs& a) {
            if (a.text == "Rename" || a.text == "Delete") picked[i] = int(a.value);
        });
    }
    ui.update(1.f / 60.f);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const Case& c = cases[i];
        const Rect r = ui.contentRect(c.node);
        // the second row of a view (below its header when it has one), the middle of anything else
        const Vec2 at = c.view ? Vec2{r.x + 30.f, r.y + (std::string_view(c.name) == "tableview" ? 52.f : 30.f)} : ctr(ui.rect(c.node));
        const f32 before = ui.value(c.node);
        UI_CHECK_EQ(probe.openMenus(), 0u);
        probe.rightClick(at);
        if (probe.openMenus() != 1u) std::fprintf(stderr, "  %s: a right press opened %u menus\n", c.name, probe.openMenus());
        UI_CHECK_EQ(probe.openMenus(), 1u);
        // picking an entry reports it on the widget
        if (const NodeHandle del = probe.item("Delete"); ui.alive(del)) f.click(ctr(ui.rect(del)));
        for (int k = 0; k < 30; ++k) ui.update(1.f / 30.f);
        if (picked[i] != 1) std::fprintf(stderr, "  %s: picking 'Delete' was not reported\n", c.name);
        UI_CHECK(picked[i] == 1 && probe.openMenus() == 0u);
        // a view made the row under the pointer current first; other widgets did not change their value
        if (std::string_view(c.name) == "listview" || std::string_view(c.name) == "tableview" || std::string_view(c.name) == "modellistview" ||
            std::string_view(c.name) == "treeview")
            UI_CHECK(ui.value(c.node) == 1.f);
        else if (!c.view && std::string_view(c.name) != "lineedit") { // (a line edit's value is its repaint counter)
            if (ui.value(c.node) != before) std::fprintf(stderr, "  %s: value %g -> %g\n", c.name, double(before), double(ui.value(c.node)));
            UI_CHECK(ui.value(c.node) == before);
        }
    }
    UI_CHECK(ui.text(ui.find("edit")) == "text" && w::comboText(ui, ui.find("combo")) == "a"); // a context pick is not a combo pick
    // the Shift+F10 keyboard route still opens it, at the widget
    ui.setFocus(cases[0].node, FocusReason::Keyboard);
    ui.keyDown(Key::F10, kModShift);
    ui.update(1.f / 60.f);
    UI_CHECK_EQ(probe.openMenus(), 1u);
    probe.dismiss();
    UI_CHECK_EQ(probe.openMenus(), 0u);

    // an item view's own per-cell hook (setViewContextMenu) still gets the press, with the cell; when it is set the
    // generic menu stays out of the way (one right press, one menu)
    struct Seen { int calls = 0; u32 row = 99, col = 99; w::ItemId item = 99; } seen;
    const NodeHandle table = cases[2].node;
    w::setViewContextMenu(ui, table, [](void* u, UiContext& c, NodeHandle view, u32 row, w::ItemId item, u32 col, Vec2 at) {
        auto* s = static_cast<Seen*>(u);
        ++s->calls;
        s->row = row;
        s->col = col;
        s->item = item;
        w::popupMenu(c, view, "Cell action", at);
    }, &seen);
    const Rect tr = ui.contentRect(table);
    probe.rightClick({tr.x + 150.f, tr.y + 52.f + 2.f * 20.f});
    UI_CHECK(seen.calls == 1 && seen.col == 1u && seen.row == u32(ui.value(table)) && seen.item == seen.row);
    UI_CHECK(probe.openMenus() == 1u && ui.alive(probe.item("Cell action")) && !ui.alive(probe.item("Rename")));
    probe.dismiss();
    // off the rows (the header) there is no cell: the generic menu answers
    probe.rightClick({tr.x + 30.f, tr.y + 6.f});
    UI_CHECK(seen.calls == 1 && probe.openMenus() == 1u && ui.alive(probe.item("Rename")));
    probe.dismiss();
    // a right press keeps a multi-selection the row is part of, and never starts an edit, a sort or a fold
    w::setViewContextMenu(ui, table, nullptr);
    w::setViewSelectionMode(ui, table, w::SelectionMode::Extended);
    w::SelectionModel* sel = w::viewSelectionModel(ui, table);
    UI_CHECK(sel != nullptr);
    if (sel) {
        sel->clear();
        sel->selectRows(0, 2);
        probe.rightClick({tr.x + 30.f, tr.y + 52.f});
        if (!(sel->selectedRows() == 3 && sel->currentRow() == 1))
            std::fprintf(stderr, "  extended: %llu rows selected, current %lld, %u menus\n", (unsigned long long)sel->selectedRows(), (long long)sel->currentRow(), probe.openMenus());
        UI_CHECK(sel->selectedRows() == 3 && sel->currentRow() == 1 && probe.openMenus() == 1u);
        probe.dismiss();
    }
    int toggles = 0;
    const NodeHandle tree = cases[1].node;
    ui.connect(tree, SignalKind::Toggled, [&toggles](UiContext&, NodeHandle, const SignalArgs&) { ++toggles; });
    const Rect tc = ui.contentRect(tree);
    probe.rightClick({tc.x + 8.f, tc.y + 8.f}); // on the expander of "Scene"
    UI_CHECK(toggles == 0 && w::treeExpanded(ui, tree, 0) && probe.openMenus() == 1u);
    probe.dismiss();
    // a disabled widget opens nothing
    ui.setDisabled(cases[0].node, true);
    probe.rightClick(ctr(ui.rect(cases[0].node)));
    UI_CHECK_EQ(probe.openMenus(), 0u);
}
// ---- the default theme dims disabled inputs -------------------------------------------------------------------------
// Each Tier A input next to a disabled twin: the disabled one must be visibly weaker (less contrast against the
// window), still be there, and take no input.
void gateDisabledTheme() {
    UiContext ui{{.viewport = {560, 620}, .font = &font()}};
    w::install(ui);
    ui.setCss(ui.root(), "padding", "10px");
    ui.setCss(ui.root(), "gap", "8px");
    struct Pair { const char* name; NodeHandle on, off; };
    std::vector<Pair> pairs;
    auto add = [&](const char* name, auto make) {
        const NodeHandle row = w::panel(ui, ui.root());
        ui.setCss(row, "flex-direction", "row");
        ui.setCss(row, "gap", "40px");
        ui.setCss(row, "align-items", "center");
        Pair p{name, make(row), make(row)};
        for (NodeHandle n : {p.on, p.off}) ui.setCss(n, "width", "200px");
        ui.setDisabled(p.off, true);
        pairs.push_back(p);
    };
    add("button", [&](NodeHandle r) { return w::button(ui, r, "Button"); });
    add("toolbutton", [&](NodeHandle r) { return w::toolButton(ui, r, "Tool button"); });
    add("link", [&](NodeHandle r) { return w::link(ui, r, "A link"); });
    add("checkbox", [&](NodeHandle r) { return w::checkBox(ui, r, "Check box", true); });
    add("radio", [&](NodeHandle r) { return w::radioButton(ui, r, "Radio", true); });
    add("switch", [&](NodeHandle r) { return w::toggleSwitch(ui, r, "Switch", true); });
    add("slider", [&](NodeHandle r) { return w::slider(ui, r, 0.f, 1.f, 0.6f); });
    add("scrollbar", [&](NodeHandle r) { const NodeHandle b = w::scrollBar(ui, r, false); ui.setValue(b, 0.3f); return b; });
    add("dial", [&](NodeHandle r) { return w::dial(ui, r, 0.f, 1.f, 0.6f); });
    add("spinbox", [&](NodeHandle r) { return w::spinBox(ui, r, 0.f, 100.f, 42.f, 1.f); });
    add("doublespinbox", [&](NodeHandle r) { return w::doubleSpinBox(ui, r, 0.f, 1.f, 0.5f, 0.1f); });
    add("combobox", [&](NodeHandle r) { return w::comboBox(ui, r, "Vulkan\nCPU raster", 0); });
    add("lineedit", [&](NodeHandle r) { return w::lineEdit(ui, r, "player_one"); });
    const NodeHandle bar = w::menuBar(ui, ui.root());
    const NodeHandle mOn = w::menuBarMenu(ui, bar, "File", "New"), mOff = w::menuBarMenu(ui, bar, "File", "New");
    ui.setDisabled(mOff, true);
    pairs.push_back({"menubutton", mOn, mOff});
    ui.update(1.f / 60.f);
    Image img;
    render(ui, img);
    const u32 bg = img.pixels[std::size_t(3) * std::size_t(img.width) + 3]; // the window colour (root padding)
    auto ink = [&](NodeHandle n) { // summed colour distance from the window colour over the node
        const Rect r = ui.rect(n);
        double sum = 0.0;
        for (int y = int(r.y); y < int(r.bottom()) && y < img.height; ++y)
            for (int x = int(r.x); x < int(r.right()) && x < img.width; ++x) {
                const u32 p = img.pixels[std::size_t(y) * std::size_t(img.width) + std::size_t(x)];
                for (int c = 0; c < 24; c += 8) sum += std::abs(int((p >> c) & 0xFFu) - int((bg >> c) & 0xFFu));
            }
        return sum;
    };
    for (const Pair& p : pairs) {
        const Rect a = ui.rect(p.on), b = ui.rect(p.off);
        UI_CHECK(a.w == b.w && a.h == b.h && a.w > 0.f); // dimming changes no geometry
        const double on = ink(p.on), off = ink(p.off);
        if (!(off < on * 0.8 && off > on * 0.15))
            std::fprintf(stderr, "  %s: enabled ink %.0f, disabled ink %.0f\n", p.name, on, off);
        UI_CHECK(off < on * 0.8);  // visibly weaker
        UI_CHECK(off > on * 0.15); // but still readable
    }
    // and a disabled input takes no input (value, text and checked state stay)
    for (const Pair& p : pairs) {
        const f32 v = ui.value(p.off);
        const bool c = ui.checked(p.off);
        const Vec2 at = ctr(ui.rect(p.off));
        ui.pointerMove(at);
        ui.pointerDown({at.x + 40.f, at.y});
        ui.pointerUp({at.x + 40.f, at.y});
        ui.wheel(at, {0.f, 1.f});
        ui.update(1.f / 60.f);
        UI_CHECK(ui.value(p.off) == v && ui.checked(p.off) == c && ui.focused() != p.off);
    }
    // enabling again restores the look exactly
    for (const Pair& p : pairs) ui.setDisabled(p.off, false);
    ui.pointerMove({1.f, 1.f});
    ui.update(1.f / 60.f);
    render(ui, img);
    for (const Pair& p : pairs) {
        const double on = ink(p.on), off = ink(p.off);
        if (on != off) std::fprintf(stderr, "  %s: after re-enabling %.0f vs %.0f\n", p.name, on, off);
        UI_CHECK(on == off);
    }
}
} // namespace

int main(int argc, char** argv) {
    struct Gate { const char* name; void (*fn)(); };
    static const Gate kGates[] = {{"double_spinbox", &gateDoubleSpinBox}, {"tabbar", &gateTabBar}, {"close_popups", &gateClose},
                                  {"timeline_view", &gateTimelineView}, {"viewport_overlay", &gateViewport},
                                  {"nodegraph_disconnect", &gateNodeGraphDisconnect}, {"haptic_theme", &gateHapticTheme},
                                  {"touch_hooks", &gateTouchHooks},
                                  {"context_menu_views", &gateContextMenuViews}, {"disabled_theme", &gateDisabledTheme}};
    const char* only = nullptr;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--gate=", 7) == 0) only = argv[i] + 7;
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
    return finish("fuse_ui_catalogue_gaps");
}
