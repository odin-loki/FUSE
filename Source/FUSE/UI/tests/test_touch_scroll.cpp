// Gate fuse_ui_touch_scroll: the UiContext touch path (touchDown / touchMove / touchUp / touchCancel). A tap clicks;
// a drag past the slop cancels the press and pans the nearest scrollable ancestor 1:1; a release in motion flings,
// decays and stops (nextWakeSeconds() back to +inf); a new touch stops a fling; widgets that own their drags
// (slider, setCapturesDrag) keep them; widgets that scroll on Wheel (list / table / text / code edit) follow the
// finger; nested containers chain; the drag axis picks the container; the mouse path is unchanged.
#include "test_util.hpp"

#include <atomic>
#include <cmath>
#include <limits>
#include <new>

namespace {
std::atomic<long> g_allocs{0};
bool g_counting = false;
} // namespace

void* operator new(std::size_t n) {
    if (g_counting) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
Vec2 centre(const Rect& r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
bool near(f32 a, f32 b, f32 eps = 0.01f) { return std::abs(a - b) <= eps; }
} // namespace

int main() {
    UiContext ui({.viewport = {420, 900}, .font = &font()});
    w::install(ui);
    const NodeHandle root = ui.root();
    ui.setCss(root, "flex-direction", "column");

    // page: a vertical scroll container with buttons, a slider, a horizontal strip and a nested vertical area
    const NodeHandle page = w::scrollArea(ui, root, "page");
    ui.setCss(page, "height", "300px");
    const NodeHandle b0 = w::button(ui, page, "Button 0", "b0");
    const NodeHandle slider = w::slider(ui, page, 0.f, 1.f, 0.f, 0.f, "slider");
    ui.setCss(slider, "width", "300px");
    const NodeHandle strip = w::scrollArea(ui, page, "strip");
    ui.setCss(strip, "flex-direction", "row");
    ui.setCss(strip, "height", "40px");
    ui.setCss(strip, "flex-shrink", "0");
    for (int i = 0; i < 12; ++i) {
        const NodeHandle c = w::button(ui, strip, "Cell " + std::to_string(i));
        ui.setCss(c, "width", "120px");
        ui.setCss(c, "flex-shrink", "0");
    }
    const NodeHandle inner = w::scrollArea(ui, page, "inner");
    ui.setCss(inner, "height", "90px");
    ui.setCss(inner, "flex-shrink", "0");
    for (int i = 0; i < 20; ++i) ui.setCss(w::label(ui, inner, "inner row " + std::to_string(i)), "flex-shrink", "0");
    const NodeHandle b1 = w::button(ui, page, "Button 1", "b1");
    const NodeHandle grip = w::panel(ui, page, "grip"); // a custom drag surface (per-node capture)
    ui.setCss(grip, "height", "40px");
    ui.setCss(grip, "flex-shrink", "0");
    ui.setCapturesDrag(grip, true);
    for (int i = 2; i < 40; ++i) ui.setCss(w::button(ui, page, "Button " + std::to_string(i), "b" + std::to_string(i)), "flex-shrink", "0");
    for (const NodeHandle n : {b0, slider, b1}) ui.setCss(n, "flex-shrink", "0");

    // widgets that scroll themselves on Wheel events
    std::string items;
    for (int i = 0; i < 200; ++i) items += (i ? "\n" : "") + std::string("Item ") + std::to_string(i);
    const NodeHandle list = w::listView(ui, root, items, -1, "list");
    ui.setCss(list, "height", "150px");
    w::StringTableModel model("Name\tValue");
    for (int i = 0; i < 200; ++i) model.addRow("row " + std::to_string(i) + "\t" + std::to_string(i * 3));
    const NodeHandle table = w::tableView(ui, root, &model.model(), "table");
    ui.setCss(table, "height", "150px");
    std::string text;
    for (int i = 0; i < 200; ++i) text += "line " + std::to_string(i) + "\n";
    const NodeHandle code = w::codeEdit(ui, root, text, w::CodeLanguage::Plain, "code");
    ui.setCss(code, "height", "100px");
    const NodeHandle edit = w::textEdit(ui, root, text, "edit");
    ui.setCss(edit, "height", "100px");
    ui.update();

    const f32 inf = std::numeric_limits<f32>::infinity();
    const f32 pageMax = ui.scrollExtent(page).h - ui.contentRect(page).h;
    UI_CHECK(pageMax > 400.f);
    UI_CHECK(ui.scrollExtent(strip).w - ui.contentRect(strip).w > 400.f);
    UI_CHECK(ui.scrollExtent(inner).h - ui.contentRect(inner).h > 100.f);
    UI_CHECK(ui.scrollExtent(strip).h <= ui.contentRect(strip).h + 0.5f); // the strip only scrolls sideways

    int clicks = 0, cancels = 0, ups = 0;
    auto onClick = [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; };
    auto onEvent = [&cancels, &ups](UiContext&, NodeHandle, const SignalArgs& a) {
        if (a.event && a.event->type == EventType::PointerUp) { ++ups; if (a.event->cancel) ++cancels; }
    };
    for (const NodeHandle b : {b0, b1}) {
        ui.connect(b, SignalKind::Clicked, onClick);
        ui.connect(b, SignalKind::Event, onEvent);
    }
    double t = 0.0; // event clock (seconds)
    auto settle = [&] { // run until idle; returns the number of frames
        int frames = 0;
        while (ui.needsUpdate() && frames < 2000) { ui.update(1.f / 60.f); ++frames; }
        return frames;
    };
    auto resetPage = [&] { ui.stopFling(); ui.scrollTo(page, {}); ui.scrollTo(strip, {}); ui.scrollTo(inner, {}); settle(); };

    // --- a tap clicks, leaves nothing pressed or hovered -----------------------------------------------------------
    {
        const Vec2 p = centre(ui.rect(b0));
        UI_CHECK(ui.touchDown(p, t));
        UI_CHECK(ui.pressed() == b0);
        UI_CHECK(ui.states(b0) & kStatePressed);
        UI_CHECK(ui.touchUp(p, t += 0.05));
        UI_CHECK_EQ(clicks, 1);
        UI_CHECK(!ui.pressed().valid());
        UI_CHECK(!ui.hovered().valid()); // a lifted finger hovers nothing
        UI_CHECK(!(ui.states(b0) & (kStatePressed | kStateHover)));
        UI_CHECK(!ui.flinging());
    }
    // --- jitter under the slop is still a tap ----------------------------------------------------------------------
    {
        const Vec2 p = centre(ui.rect(b0));
        ui.touchDown(p, t += 1.0);
        ui.touchMove({p.x + 3.f, p.y + 4.f}, t += 0.01); // 5 px < 8 px slop
        ui.touchMove({p.x - 2.f, p.y - 5.f}, t += 0.01);
        UI_CHECK(ui.pressed() == b0);
        UI_CHECK(!ui.touchPanning());
        ui.touchUp({p.x - 2.f, p.y - 5.f}, t += 0.01);
        UI_CHECK_EQ(clicks, 2);
        UI_CHECK_EQ(ui.scroll(page).y, 0.f);
        UI_CHECK_EQ(cancels, 0);
    }
    // --- a drag past the slop pans the page by the finger delta; the button under the finger does not click ------------
    {
        const Vec2 p = centre(ui.rect(b1));
        const int upsBefore = ups;
        ui.touchDown(p, t += 1.0);
        UI_CHECK(ui.pressed() == b1);
        ui.touchMove({p.x, p.y - 30.f}, t += 0.02);
        UI_CHECK(ui.touchPanning());
        UI_CHECK(ui.touchPanTarget() == page);
        UI_CHECK(!ui.pressed().valid());
        UI_CHECK(!(ui.states(b1) & kStatePressed));
        UI_CHECK_EQ(cancels, 1); // the widget saw a cancelling PointerUp
        UI_CHECK_EQ(ups, upsBefore + 1);
        UI_CHECK(near(ui.scroll(page).y, 30.f));
        ui.update(1.f / 60.f);
        ui.touchMove({p.x + 5.f, p.y - 70.f}, t += 0.02);
        UI_CHECK(near(ui.scroll(page).y, 70.f));
        UI_CHECK_EQ(ui.scroll(page).x, 0.f);
        ui.touchMove({p.x, p.y - 50.f}, t += 0.02); // back down: content follows
        UI_CHECK(near(ui.scroll(page).y, 50.f));
        ui.touchUp({p.x, p.y - 50.f}, t += 0.5); // the finger rested: no fling
        UI_CHECK(!ui.flinging());
        UI_CHECK_EQ(clicks, 2);
        UI_CHECK_EQ(ups, upsBefore + 1); // no second PointerUp, no Click
        settle();
        UI_CHECK(near(ui.scroll(page).y, 50.f));
        UI_CHECK_EQ(ui.nextWakeSeconds(), inf);
        // clamped at the extents: dragging down past the top stops at 0
        const Vec2 p2 = centre(ui.rect(b1)); // it moved with the page
        ui.touchDown(p2, t += 1.0);
        ui.touchMove({p2.x, p2.y + 200.f}, t += 0.02);
        UI_CHECK(ui.touchPanTarget() == page);
        UI_CHECK_EQ(ui.scroll(page).y, 0.f);
        ui.touchUp({p2.x, p2.y + 200.f}, t += 0.5);
        resetPage();
    }
    // --- release in motion: the fling continues, decays, stops; the loop can sleep again ---------------------------------
    {
        Vec2 p = centre(ui.rect(b1));
        ui.touchDown(p, t += 1.0);
        for (int i = 0; i < 8; ++i) { p.y -= 6.f; ui.touchMove(p, t += 0.01); } // 600 px/s upwards
        const f32 atRelease = ui.scroll(page).y;
        UI_CHECK(near(atRelease, 48.f));
        ui.touchUp(p, t);
        UI_CHECK(ui.flinging());
        UI_CHECK(near(ui.flingVelocity().y, -600.f, 5.f));
        UI_CHECK(ui.needsUpdate());
        UI_CHECK_EQ(ui.nextWakeSeconds(), 0.f);
        f32 prev = atRelease, prevStep = 1e9f;
        int frames = 0;
        bool monotone = true, decays = true;
        while (ui.flinging() && frames < 2000) {
            ui.update(1.f / 60.f);
            ++frames;
            const f32 y = ui.scroll(page).y, step = y - prev;
            if (step < 0.f) monotone = false;
            if (step > prevStep + 1e-3f) decays = false;
            prev = y;
            prevStep = step;
        }
        UI_CHECK(monotone && decays);
        UI_CHECK(frames > 10 && frames < 600); // it glides for a while, and it ends
        // glide length = v0 / decay minus the tail below minVelocity
        const TouchScrollConfig cfg = ui.touchScroll();
        UI_CHECK(near(prev - atRelease, (600.f - cfg.minVelocity) / cfg.decay, 6.f));
        UI_CHECK(prev < pageMax);
        settle();
        UI_CHECK(!ui.needsUpdate());
        UI_CHECK_EQ(ui.nextWakeSeconds(), inf);
        UI_CHECK_EQ(ui.scroll(page).y, prev);
        UI_CHECK_EQ(clicks, 2);
        // a hard fling runs into the end of the page and stops there
        p = {};
        for (int i = 2; i < 12; ++i) { // a button low enough in the visible part of the page to swipe up 200 px
            const Vec2 c = centre(ui.rect(ui.find("b" + std::to_string(i))));
            if (c.y - 205.f > ui.rect(page).y && c.y + 5.f < ui.rect(page).bottom()) { p = c; break; }
        }
        UI_CHECK(p.y > 0.f);
        ui.touchDown(p, t += 1.0);
        for (int i = 0; i < 5; ++i) { p.y -= 40.f; ui.touchMove(p, t += 0.005); } // 8000 px/s
        ui.touchUp(p, t);
        UI_CHECK(ui.flinging());
        frames = settle();
        UI_CHECK(near(ui.scroll(page).y, pageMax, 0.5f)); // (layout rounding)
        UI_CHECK(!ui.flinging() && frames < 120);
        UI_CHECK_EQ(ui.nextWakeSeconds(), inf);
        resetPage();
    }
    // --- a new touch stops a fling and does not click what it lands on ---------------------------------------------------
    {
        Vec2 p = centre(ui.rect(b1));
        ui.touchDown(p, t += 1.0);
        for (int i = 0; i < 8; ++i) { p.y -= 10.f; ui.touchMove(p, t += 0.01); }
        ui.touchUp(p, t);
        UI_CHECK(ui.flinging());
        for (int i = 0; i < 5; ++i) ui.update(1.f / 60.f);
        UI_CHECK(ui.flinging());
        const f32 y = ui.scroll(page).y;
        const NodeHandle under = ui.hitTest(p);
        UI_CHECK(under.valid() && under != page);
        const int before = clicks;
        ui.touchDown(p, t += 0.1);
        UI_CHECK(!ui.flinging());
        UI_CHECK(!ui.pressed().valid());
        ui.touchUp(p, t += 0.05);
        UI_CHECK_EQ(clicks, before);
        settle();
        UI_CHECK_EQ(ui.scroll(page).y, y);
        UI_CHECK_EQ(ui.nextWakeSeconds(), inf);
        // ... and the catching finger can pan again
        ui.touchDown(p, t += 1.0);
        for (int i = 0; i < 8; ++i) { p.y -= 10.f; ui.touchMove(p, t += 0.01); }
        ui.touchUp(p, t);
        for (int i = 0; i < 3; ++i) ui.update(1.f / 60.f);
        ui.touchDown(p, t += 0.05);
        const f32 y2 = ui.scroll(page).y;
        ui.touchMove({p.x, p.y + 20.f}, t += 0.02);
        UI_CHECK(ui.touchPanning());
        UI_CHECK(near(ui.scroll(page).y, y2 - 20.f));
        ui.touchUp({p.x, p.y + 20.f}, t += 0.5);
        resetPage();
    }
    // --- a slider keeps its drag ------------------------------------------------------------------------------------
    {
        const Rect r = ui.rect(slider);
        const Vec2 p{r.x + 20.f, r.y + r.h * 0.5f};
        ui.touchDown(p, t += 1.0);
        const f32 v0 = ui.value(slider);
        ui.touchMove({p.x + 150.f, p.y}, t += 0.02);
        UI_CHECK(ui.value(slider) > v0 + 0.3f);
        ui.touchMove({p.x + 150.f, p.y + 60.f}, t += 0.02); // wandering off vertically does not turn it into a pan
        UI_CHECK(!ui.touchPanning());
        UI_CHECK(ui.pressed() == slider);
        UI_CHECK_EQ(ui.scroll(page).y, 0.f);
        ui.touchUp({p.x + 150.f, p.y + 60.f}, t);
        UI_CHECK(!ui.flinging());
        UI_CHECK(ui.value(slider) > v0 + 0.3f);
        UI_CHECK(!ui.pressed().valid());
        // a node marked with setCapturesDrag is not hijacked either
        const Vec2 g = centre(ui.rect(grip));
        ui.touchDown(g, t += 1.0);
        ui.touchMove({g.x, g.y - 40.f}, t += 0.02);
        UI_CHECK(!ui.touchPanning());
        UI_CHECK_EQ(ui.scroll(page).y, 0.f);
        ui.touchUp({g.x, g.y - 40.f}, t);
        settle();
    }
    // --- drag axis picks the container; nested containers chain ------------------------------------------------------------
    {
        Vec2 p = centre(ui.rect(strip));
        ui.touchDown(p, t += 1.0);
        ui.touchMove({p.x - 40.f, p.y - 6.f}, t += 0.02); // mostly horizontal: the strip
        UI_CHECK(ui.touchPanTarget() == strip);
        UI_CHECK(near(ui.scroll(strip).x, 40.f));
        UI_CHECK_EQ(ui.scroll(page).y, 0.f);
        ui.touchMove({p.x - 60.f, p.y - 50.f}, t += 0.02); // latched: later vertical travel does not move the page
        UI_CHECK(near(ui.scroll(strip).x, 60.f));
        UI_CHECK_EQ(ui.scroll(page).y, 0.f);
        ui.touchUp({p.x - 60.f, p.y - 50.f}, t += 0.5);
        settle();
        p = centre(ui.rect(strip));
        ui.touchDown(p, t += 1.0);
        ui.touchMove({p.x - 5.f, p.y - 25.f}, t += 0.02); // mostly vertical on the strip: the page
        UI_CHECK(ui.touchPanTarget() == page);
        UI_CHECK(near(ui.scroll(page).y, 25.f));
        UI_CHECK(near(ui.scroll(strip).x, 60.f));
        ui.touchUp({p.x - 5.f, p.y - 25.f}, t += 0.5);
        resetPage();

        // inner vertical area inside the vertical page: the inner one scrolls first ...
        p = centre(ui.rect(inner));
        ui.touchDown(p, t += 1.0);
        ui.touchMove({p.x, p.y - 30.f}, t += 0.02);
        UI_CHECK(ui.touchPanTarget() == inner);
        UI_CHECK(near(ui.scroll(inner).y, 30.f));
        UI_CHECK_EQ(ui.scroll(page).y, 0.f);
        ui.touchUp({p.x, p.y - 30.f}, t += 0.5);
        settle();
        // ... and when it is at its end in the drag direction the page takes the gesture
        ui.scrollTo(inner, {0.f, 1e6f});
        settle();
        const f32 innerMax = ui.scroll(inner).y;
        p = centre(ui.rect(inner));
        ui.touchDown(p, t += 1.0);
        ui.touchMove({p.x, p.y - 30.f}, t += 0.02);
        UI_CHECK(ui.touchPanTarget() == page);
        UI_CHECK(near(ui.scroll(page).y, 30.f));
        UI_CHECK_EQ(ui.scroll(inner).y, innerMax);
        ui.touchUp({p.x, p.y - 30.f}, t += 0.5);
        settle();
        // the other way (pulling down) the inner area can still move
        p = centre(ui.rect(inner));
        ui.touchDown(p, t += 1.0);
        ui.touchMove({p.x, p.y + 20.f}, t += 0.02);
        UI_CHECK(ui.touchPanTarget() == inner);
        UI_CHECK(near(ui.scroll(inner).y, innerMax - 20.f));
        ui.touchUp({p.x, p.y + 20.f}, t += 0.5);
        resetPage();
    }
    // --- list / table / code / text edit follow the finger (through Wheel events) ------------------------------------------
    {
        const Rect lc = ui.contentRect(list);
        Vec2 p{lc.x + 30.f, lc.y + 100.f};
        UI_CHECK_EQ(w::listFirstVisible(ui, list), 0u);
        ui.touchDown(p, t += 1.0);
        // find the row height: rows change every row height of travel (rounded to the nearest row)
        f32 rowH = 0.f, at1 = 0.f;
        for (int i = 1; i <= 90 && rowH == 0.f; ++i) {
            ui.touchMove({p.x, p.y - f32(i)}, t += 0.01);
            if (at1 == 0.f && w::listFirstVisible(ui, list) == 1u) at1 = f32(i);
            if (w::listFirstVisible(ui, list) == 2u) rowH = f32(i) - at1;
        }
        UI_CHECK(rowH >= 10.f && rowH <= 40.f);
        UI_CHECK(ui.touchPanTarget() == list);
        ui.touchMove({p.x, p.y - rowH * 4.f - 1.f}, t += 0.5);
        UI_CHECK_EQ(w::listFirstVisible(ui, list), 4u); // 1:1 in whole rows
        ui.touchMove({p.x, p.y - rowH * 2.f - 1.f}, t += 0.5);
        UI_CHECK_EQ(w::listFirstVisible(ui, list), 2u);
        ui.touchUp({p.x, p.y - rowH * 2.f - 1.f}, t += 0.5);
        UI_CHECK(!ui.flinging());
        // slow drag: sub-row steps accumulate
        ui.touchDown(p, t += 1.0);
        ui.touchMove({p.x, p.y - 9.f}, t += 0.1);
        for (int i = 0; i < int(rowH) * 3; ++i) ui.touchMove({p.x, p.y - 9.f - f32(i + 1)}, t += 0.1);
        UI_CHECK(w::listFirstVisible(ui, list) >= 4u && w::listFirstVisible(ui, list) <= 6u);
        ui.touchUp({p.x, p.y - 9.f - rowH * 3.f}, t += 0.5);
        // fling
        const u32 f0 = w::listFirstVisible(ui, list);
        ui.touchDown(p, t += 1.0);
        Vec2 q = p;
        for (int i = 0; i < 8; ++i) { q.y -= 10.f; ui.touchMove(q, t += 0.01); }
        ui.touchUp(q, t);
        UI_CHECK(ui.flinging());
        const u32 f1 = w::listFirstVisible(ui, list);
        const int frames = settle();
        UI_CHECK(w::listFirstVisible(ui, list) > f1 && f1 > f0);
        UI_CHECK(frames < 600);
        UI_CHECK_EQ(ui.nextWakeSeconds(), inf);

        const Rect tc = ui.contentRect(table);
        p = {tc.x + 30.f, tc.y + 100.f};
        UI_CHECK_EQ(w::tableFirstVisible(ui, table), 0u);
        ui.touchDown(p, t += 1.0);
        ui.touchMove({p.x, p.y - rowH * 3.f - 1.f}, t += 0.5);
        UI_CHECK(ui.touchPanTarget() == table);
        UI_CHECK_EQ(w::tableFirstVisible(ui, table), 3u);
        ui.touchUp({p.x, p.y - rowH * 3.f - 1.f}, t += 0.5);
        // columns wider than the view: a sideways drag pans them, a drag on the horizontal bar stays with the bar
        w::setTableColumnWidth(ui, table, 0, tc.w);
        w::setTableColumnWidth(ui, table, 1, tc.w);
        settle();
        UI_CHECK_EQ(w::viewScrollX(ui, table), 0.f);
        ui.touchDown(p, t += 1.0);
        ui.touchMove({p.x - 80.f, p.y}, t += 0.5);
        UI_CHECK(ui.touchPanTarget() == table);
        UI_CHECK(std::abs(w::viewScrollX(ui, table) - 80.f) <= 8.f);
        UI_CHECK_EQ(w::tableFirstVisible(ui, table), 3u);
        ui.touchUp({p.x - 80.f, p.y}, t += 0.5);
        settle();
        const f32 sx = w::viewScrollX(ui, table);
        const Vec2 bar{tc.x + 20.f, tc.bottom() - 3.f};
        ui.touchDown(bar, t += 1.0);
        ui.touchMove({bar.x + 60.f, bar.y}, t += 0.5);
        UI_CHECK(!ui.touchPanTarget().valid() && ui.pressed() == table);
        UI_CHECK(w::viewScrollX(ui, table) != sx);
        ui.touchUp({bar.x + 60.f, bar.y}, t += 0.5);
        w::setTableColumnWidth(ui, table, 0, 120.f);
        w::setTableColumnWidth(ui, table, 1, 160.f);
        settle();
        UI_CHECK_EQ(w::viewScrollX(ui, table), 0.f);

        for (const NodeHandle ed : {code, edit}) {
            const Vec2 c = centre(ui.rect(ed));
            ui.touchDown(c, t += 1.0);
            ui.touchMove({c.x, c.y - 40.f}, t += 0.5);
            UI_CHECK(ui.touchPanTarget() == ed); // scrolls instead of selecting
            UI_CHECK(!ui.pressed().valid());
            ui.touchUp({c.x, c.y - 40.f}, t += 0.5);
        }
        u32 line = 0, col = 0;
        w::codeEditCaret(ui, code, line, col);
        UI_CHECK(line < 12u); // the caret stayed where the finger went down: the drag did not select towards line 40+
        settle();
        UI_CHECK_EQ(ui.scroll(page).y, 0.f);
    }
    // --- steady state: the touch calls themselves allocate nothing (tap, press cancel, pan, release) -----------------------
    // (update() is left out: repainting this tree's table / code edit allocates with any input device)
    {
        auto gesture = [&](bool count) {
            Vec2 p = centre(ui.rect(b1));
            g_counting = count;
            ui.touchDown(p, t += 1.0);
            ui.touchUp(p, t += 0.05);
            g_counting = false;
            ui.update(1.f / 60.f);
            g_counting = count;
            ui.touchDown(p, t += 1.0);
            g_counting = false;
            for (int i = 0; i < 8; ++i) {
                p.y -= 8.f;
                g_counting = count;
                ui.touchMove(p, t += 0.01);
                g_counting = false;
                ui.update(1.f / 60.f);
            }
            g_counting = count;
            ui.touchUp(p, t);
            g_counting = false;
            UI_CHECK(ui.flinging());
            settle();
            ui.scrollTo(page, {});
            settle();
        };
        gesture(false); // warm-up
        for (int i = 0; i < 5; ++i) gesture(true);
        UI_CHECK_EQ(g_allocs.load(), 0L);
    }
    // --- touchCancel releases without a click ---------------------------------------------------------------------------
    {
        const Vec2 p = centre(ui.rect(b0));
        const int before = clicks, cancelsBefore = cancels;
        ui.touchDown(p, t += 1.0);
        UI_CHECK(ui.pressed() == b0);
        UI_CHECK(ui.touchCancel());
        UI_CHECK(!ui.pressed().valid());
        UI_CHECK(!(ui.states(b0) & kStatePressed));
        UI_CHECK_EQ(clicks, before);
        UI_CHECK_EQ(cancels, cancelsBefore + 1);
        UI_CHECK(!ui.touchUp(p, t += 0.1)); // nothing left to release
    }
    // --- the mouse path is unchanged: a mouse drag over the page does not scroll it ------------------------------------------
    {
        const Vec2 p = centre(ui.rect(b1));
        const int before = clicks;
        ui.pointerMove(p);
        ui.pointerDown(p);
        ui.pointerMove({p.x, p.y - 60.f});
        UI_CHECK(ui.pressed() == b1);
        UI_CHECK(!ui.touchPanning());
        ui.pointerMove(p);
        ui.pointerUp(p);
        UI_CHECK_EQ(clicks, before + 1);
        UI_CHECK_EQ(ui.scroll(page).y, 0.f);
        UI_CHECK(!ui.flinging());
        UI_CHECK(ui.hovered().valid()); // the mouse still hovers
        ui.pointerDown(p);
        ui.pointerMove({p.x, p.y - 60.f});
        ui.pointerUp({p.x, p.y - 60.f});
        UI_CHECK_EQ(ui.scroll(page).y, 0.f);
        settle();
        UI_CHECK_EQ(ui.nextWakeSeconds(), inf);
    }
    return finish("fuse_ui_touch_scroll");
}
