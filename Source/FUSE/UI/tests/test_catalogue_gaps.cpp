// fuse_ui_catalogue_gaps: behaviour gates for the public widget functions no other gate called (found by grepping
// the tests for every function of widgets.hpp / platform_feel.hpp): doubleSpinBox, standalone tabBar, closePopup /
// closeDialog / closePopover, timelineSetView, viewportSetImage / overlay, nodeGraphDisconnect, haptic,
// defaultThemeCss, and the touch-drag hooks of gridview / header / video (table, text and code edit are in
// fuse_ui_touch_scroll). `--gate=<name>` runs one group (ctest fuse_ui_gap_<name>).
#include "test_util.hpp"

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
} // namespace

int main(int argc, char** argv) {
    struct Gate { const char* name; void (*fn)(); };
    static const Gate kGates[] = {{"double_spinbox", &gateDoubleSpinBox}, {"tabbar", &gateTabBar}, {"close_popups", &gateClose},
                                  {"timeline_view", &gateTimelineView}, {"viewport_overlay", &gateViewport},
                                  {"nodegraph_disconnect", &gateNodeGraphDisconnect}, {"haptic_theme", &gateHapticTheme},
                                  {"touch_hooks", &gateTouchHooks}};
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
