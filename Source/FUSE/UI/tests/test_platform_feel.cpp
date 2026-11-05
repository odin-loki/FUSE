// UI10 platform feel gate: kinetic scroll physics per profile (fling, decay, rubber band, spring back, edge clamp),
// the haptics hook, and kineticScrollArea (drag to scroll, fling after release, clicks below the threshold, pinch /
// Ctrl+wheel zoom about the gesture point).
#include "test_util.hpp"

#include "fuse/ui/widgets/platform_feel.hpp"

#include <cmath>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
int g_edges = 0;
void onHaptic(void*, w::HapticKind k) { g_edges += k == w::HapticKind::Edge; }
// finger moves `dist` px up over `secs` in 60 Hz samples
void swipe(w::KineticScroller& k, f32 from, f32 dist, f32 secs, f32& t) {
    k.press(from, t);
    const int n = int(secs * 60.f);
    for (int i = 1; i <= n; ++i) { t += 1.f / 60.f; k.drag(from - dist * f32(i) / f32(n), t); }
    k.release(t);
}
f32 settle(w::KineticScroller& k, f32 maxSecs, f32& took) {
    took = 0.f;
    while (k.step(1.f / 120.f) && took < maxSecs) took += 1.f / 120.f;
    return k.offset();
}
} // namespace

int main() {
    w::setHapticsHandler(onHaptic, nullptr);
    // iOS: a 300 px swipe in 0.15 s (2000 px/s) glides well past the finger and stops by itself
    {
        w::KineticScroller k(w::ScrollProfile::ios());
        k.setBounds(0.f, 10000.f);
        f32 t = 0.f, took = 0.f;
        swipe(k, 500.f, 300.f, 0.15f, t);
        UI_CHECK(std::abs(k.velocity() - 2000.f) < 60.f);
        const f32 end = settle(k, 5.f, took);
        // analytic glide v / decay = 2000 / 4.5 ~ 444 px beyond the finger
        UI_CHECK(end > 300.f + 380.f && end < 300.f + 470.f);
        UI_CHECK(took > 0.5f && took < 2.5f && !k.active());
    }
    // a slow drag (below minVelocity at release) does not fling
    {
        w::KineticScroller k;
        k.setBounds(0.f, 1000.f);
        f32 t = 0.f;
        k.press(100.f, t);
        for (int i = 0; i < 30; ++i) { t += 1.f / 60.f; k.drag(100.f - f32(i < 10 ? i : 10), t); }
        k.release(t);
        UI_CHECK(k.velocity() == 0.f && k.offset() == 10.f);
    }
    // rubber band: dragging 200 px past the top moves less than 200 and springs back to 0
    {
        w::KineticScroller k(w::ScrollProfile::ios());
        k.setBounds(0.f, 400.f);
        f32 t = 0.f, took = 0.f;
        k.press(0.f, t);
        t += 0.5f;
        k.drag(200.f, t);
        UI_CHECK(k.offset() < 0.f && k.offset() > -200.f * 0.6f);
        UI_CHECK(k.overscroll() < 0.f);
        k.release(t + 0.5f);
        settle(k, 3.f, took);
        UI_CHECK(k.offset() == 0.f && took < 1.2f);
        // a fling into the bottom edge overshoots, fires an Edge haptic, and bounces back to the edge
        g_edges = 0;
        k.setOffset(300.f);
        swipe(k, 500.f, 200.f, 0.1f, t);
        f32 peak = 0.f;
        while (k.step(1.f / 120.f)) peak = std::max(peak, k.offset());
        UI_CHECK(peak > 400.f && k.offset() == 400.f && g_edges >= 1);
    }
    // desktop / android clamp at the ends (no overscroll)
    for (const w::ScrollProfile p : {w::ScrollProfile::desktop(), w::ScrollProfile::android()}) {
        w::KineticScroller k(p);
        k.setBounds(0.f, 400.f);
        f32 t = 0.f, took = 0.f;
        k.press(0.f, t);
        k.drag(100.f, t + 0.1f);
        UI_CHECK(k.offset() == 0.f);
        k.release(t + 0.1f);
        k.setOffset(300.f);
        swipe(k, 500.f, 300.f, 0.1f, t);
        settle(k, 5.f, took);
        UI_CHECK(k.offset() == 400.f);
    }
    // desktop glides less than android for the same swipe
    {
        w::KineticScroller a(w::ScrollProfile::desktop()), b(w::ScrollProfile::android());
        a.setBounds(0.f, 1e6f); b.setBounds(0.f, 1e6f);
        f32 t1 = 0.f, t2 = 0.f, took = 0.f;
        swipe(a, 500.f, 200.f, 0.1f, t1);
        swipe(b, 500.f, 200.f, 0.1f, t2);
        UI_CHECK(settle(a, 5.f, took) < settle(b, 5.f, took));
    }

    // the widget
    UiContext ui{{.viewport = {400, 300}, .font = &font()}};
    w::install(ui);
    const NodeHandle area = w::kineticScrollArea(ui, ui.root(), w::ScrollProfile::ios(), "list");
    ui.setCss(area, "height", "300px");
    int clicks = 0;
    NodeHandle first;
    for (int i = 0; i < 60; ++i) {
        const NodeHandle b = w::button(ui, area, "Item " + std::to_string(i));
        if (i == 0) first = b;
        ui.connect(b, SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
    }
    ui.update(1.f / 60.f);
    UI_CHECK(ui.role(area) == A11yRole::ScrollArea);
    UI_CHECK(ui.scrollExtent(area).h > 1000.f);
    // tap: a click, no scroll
    const Rect r0 = ui.rect(first);
    const Vec2 tap{r0.x + 20.f, r0.y + r0.h * 0.5f};
    ui.pointerMove(tap); ui.pointerDown(tap); ui.update(1.f / 60.f); ui.pointerUp(tap); ui.update(1.f / 60.f);
    UI_CHECK_EQ(clicks, 1);
    UI_CHECK(ui.scroll(area).y == 0.f);
    // swipe up 150 px over 0.1 s, release: scrolls past 150 and keeps gliding, then settles in range; no click
    Vec2 p{200.f, 250.f};
    ui.pointerMove(p); ui.pointerDown(p); ui.update(1.f / 60.f);
    for (int i = 0; i < 6; ++i) { p.y -= 25.f; ui.pointerMove(p); ui.update(1.f / 60.f); }
    const f32 atRelease = ui.scroll(area).y;
    UI_CHECK(atRelease > 120.f);
    ui.pointerUp(p);
    for (int i = 0; i < 180; ++i) ui.update(1.f / 60.f);
    const f32 settled = ui.scroll(area).y;
    UI_CHECK(settled > atRelease + 100.f);
    UI_CHECK(settled <= ui.scrollExtent(area).h - ui.contentRect(area).h + 0.5f);
    UI_CHECK_EQ(clicks, 1);
    UI_CHECK(!w::kineticScroller(ui, area)->active());
    std::printf("platform feel: swipe released at %.0f px, settled at %.0f px\n", double(atRelease), double(settled));
    w::setHapticsHandler(nullptr, nullptr);

    // zoom: pinch (two touches) and Ctrl+wheel scale the content, the point under the gesture stays put
    {
        UiContext z{{.viewport = {400, 300}, .font = &font()}};
        w::install(z);
        w::ScrollProfile prof = w::ScrollProfile::ios();
        prof.minZoom = 0.5f;
        prof.maxZoom = 4.f;
        const NodeHandle za = w::kineticScrollArea(z, z.root(), prof, "zoom");
        z.setCss(za, "height", "300px");
        NodeHandle row10;
        int zclicks = 0;
        for (int i = 0; i < 30; ++i) {
            const NodeHandle b = w::button(z, za, "Row " + std::to_string(i));
            z.setCss(b, "height", "40px");
            if (i == 10) row10 = b;
            z.connect(b, SignalKind::Clicked, [&zclicks](UiContext&, NodeHandle, const SignalArgs&) { ++zclicks; });
        }
        z.update(1.f / 60.f);
        const f32 ext0 = z.scrollExtent(za).h, font0 = z.computed(row10).fontSize;
        // two fingers at y 100 / 140 spread to 60 / 180 (3x apart): zoom 3 about y 120
        const Vec2 f0{200.f, 100.f}, f1{200.f, 140.f};
        const f32 contentY = z.scroll(za).y + 120.f - z.contentRect(za).y; // content point under the midpoint
        z.touchDown(1, f0); z.update(1.f / 60.f);
        z.touchDown(2, f1); z.update(1.f / 60.f);
        for (int k = 1; k <= 4; ++k) {
            z.touchMove(1, {200.f, 100.f - 10.f * f32(k)});
            z.touchMove(2, {200.f, 140.f + 10.f * f32(k)});
            z.update(1.f / 60.f);
        }
        z.touchUp(2, {200.f, 180.f}); z.touchUp(1, {200.f, 60.f});
        for (int i = 0; i < 4; ++i) z.update(1.f / 60.f);
        const f32 zoom = w::kineticZoom(z, za);
        const f32 contentY1 = (z.scroll(za).y + 120.f - z.contentRect(za).y) / zoom;
        std::printf("platform feel: pinch zoom %.2f, extent %.0f -> %.0f, font %.0f -> %.0f, anchor %.1f -> %.1f, clicks %d\n",
                    double(zoom), double(ext0), double(z.scrollExtent(za).h), double(font0),
                    double(z.computed(row10).fontSize), double(contentY), double(contentY1), zclicks);
        UI_CHECK(std::abs(zoom - 3.f) < 0.01f);
        UI_CHECK(std::abs(z.scrollExtent(za).h - ext0 * 3.f) < 2.f);
        UI_CHECK(std::abs(z.computed(row10).fontSize - font0 * 3.f) < 0.01f);
        UI_CHECK(std::abs(z.rect(row10).h - 120.f) < 0.5f);
        UI_CHECK(std::abs(contentY1 - contentY) < 1.f); // the pinch centre stayed on the same content
        UI_CHECK_EQ(zclicks, 0);                         // a pinch is not a tap
        // Ctrl+wheel down zooms out about the pointer; clamped to the profile range
        z.setModifiers(kModCtrl);
        for (int i = 0; i < 40; ++i) { z.wheel({200.f, 150.f}, {0.f, -1.f}); z.update(1.f / 60.f); }
        UI_CHECK(std::abs(w::kineticZoom(z, za) - 0.5f) < 1e-4f);
        UI_CHECK(std::abs(z.computed(row10).fontSize - font0 * 0.5f) < 0.01f);
        z.setModifiers(0);
        const f32 before = z.scroll(za).y;
        z.wheel({200.f, 150.f}, {0.f, -1.f}); // plain wheel still scrolls (or stays at the end)
        z.update(1.f / 60.f);
        UI_CHECK(w::kineticZoom(z, za) == 0.5f && z.scroll(za).y >= before);
        // zoom off by default: the iOS profile ignores Ctrl+wheel
        ui.setModifiers(kModCtrl);
        ui.wheel({200.f, 150.f}, {0.f, 1.f});
        ui.update(1.f / 60.f);
        ui.setModifiers(0);
        UI_CHECK(w::kineticZoom(ui, area) == 1.f && ui.computed(first).zoom == 1.f);
    }
    return finish("fuse_ui_platform_feel");
}
