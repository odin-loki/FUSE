// fuse_ui_platform_services (UI1, headless): the UiContext side of the desktop platform services.
//   * DPI scale: layout stays in UI units, the draw list is in device pixels, text is re-rasterised at the new size
//   * cursor: `cursor:` style (inherited), widget defaults (line edit I-beam, link hand, splitter resize)
//   * reduced motion: animate() jumps to the target
//   * touch: the primary contact clicks like a mouse, a second contact does not; pinchScale(); pen pressure
//   * theme slot: dark -> light replaces only the theme sheet, app sheets survive
#include "fuse/ui/platform_bridge.hpp"
#include "fuse/ui/widgets/platform_feel.hpp"
#include "test_util.hpp"

#include <cmath>

bool fuse::platform::EventPump::pollEvent(PlatformEvent&) { return false; }
#if defined(_WIN32)
fuse::platform::NativeWindowHandle fuse::platform::Window::nativeHandle() const { return {}; }
#endif

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
struct GlyphStats {
    int count = 0;
    f32 maxH = 0.f;
};
GlyphStats glyphs(const DrawList& dl) {
    GlyphStats g;
    for (const DrawCmd& c : dl.cmds())
        if (c.op == DrawOp::Glyph) { ++g.count; g.maxH = std::max(g.maxH, c.rect.h); }
    return g;
}
} // namespace

int main() {
    // ---- DPI scale ----
    {
        UiContext ui({.viewport = {320, 200}, .font = &font()});
        w::install(ui);
        const NodeHandle b = w::button(ui, ui.root(), "Scale me", "b");
        ui.update();
        const Rect r1 = ui.rect(b);
        const GlyphStats g1 = glyphs(ui.drawList());
        f32 bgW1 = 0.f;
        for (const DrawCmd& c : ui.drawList().cmds())
            if (c.op == DrawOp::Rect && c.node == b.index) bgW1 = std::max(bgW1, c.rect.w);
        ui.setScale(1.5f);
        ui.update();
        UI_CHECK(ui.scale() == 1.5f);
        const Rect r2 = ui.rect(b);
        UI_CHECK(r1.x == r2.x && r1.y == r2.y && r1.w == r2.w && r1.h == r2.h); // layout in UI units
        const GlyphStats g2 = glyphs(ui.drawList());
        f32 bgW2 = 0.f;
        for (const DrawCmd& c : ui.drawList().cmds())
            if (c.op == DrawOp::Rect && c.node == b.index) bgW2 = std::max(bgW2, c.rect.w);
        std::printf("  scale 1: %d glyphs, max h %.0f, button bg %.1f; scale 1.5: %d glyphs, max h %.0f, bg %.1f\n", g1.count,
                    g1.maxH, bgW1, g2.count, g2.maxH, bgW2);
        UI_CHECK(g1.count == g2.count && g1.count > 0);
        UI_CHECK(std::abs(bgW2 - bgW1 * 1.5f) < 0.01f);
        UI_CHECK(g2.maxH >= g1.maxH * 1.3f && g2.maxH <= g1.maxH * 1.7f + 1.f); // re-rasterised, not stretched
        for (const DrawCmd& c : ui.drawList().cmds())
            if (c.op == DrawOp::Glyph) UI_CHECK(c.rect.w == f32(c.tw) && c.rect.x == std::round(c.rect.x)); // texel exact
        // device-pixel raster: the button face is where rect * scale says
        Image img;
        img.resize(480, 300, 0xFF000000u);
        CpuRasterizer rz;
        rz.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        rz.render(ui.drawList(), img);
        const int cx = int((r2.x + 3.f) * 1.5f), cy = int((r2.y + r2.h * 0.5f) * 1.5f);
        UI_CHECK(img.at(cx, cy) != img.at(470, 290));
        // input stays in UI units
        int clicks = 0;
        ui.connect(b, SignalKind::Clicked, [&](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
        UiTestDriver(ui).click("b");
        UI_CHECK(clicks == 1);
        // platform_bridge DpiChanged sets the scale
        fuse::platform::EventV2 e;
        e.type = fuse::platform::EventV2Type::DpiChanged;
        e.dpiScale = 2.f;
        feed(ui, e);
        UI_CHECK(ui.scale() == 2.f);
    }

    // ---- cursors ----
    {
        UiContext ui({.viewport = {480, 320}, .font = &font()});
        w::install(ui);
        const NodeHandle col = w::panel(ui, ui.root(), "col");
        const NodeHandle le = w::lineEdit(ui, col, "text", "le");
        const NodeHandle ln = w::link(ui, col, "a link", "ln");
        const NodeHandle bt = w::button(ui, col, "button", "bt");
        const NodeHandle cross = w::panel(ui, col, "cross");
        ui.setCss(cross, "cursor", "crosshair");
        const NodeHandle inner = w::label(ui, cross, "inside", "inner");
        const NodeHandle split = w::splitter(ui, col, false, "split");
        ui.setCss(split, "height", "60px");
        w::splitterPane(ui, split, 100.f);
        w::splitterPane(ui, split);
        const NodeHandle vsplit = w::splitter(ui, col, true, "vsplit");
        ui.setCss(vsplit, "height", "80px");
        w::splitterPane(ui, vsplit, 30.f);
        w::splitterPane(ui, vsplit);
        ui.update();
        auto at = [&](NodeHandle n) {
            const Rect r = ui.rect(n);
            ui.pointerMove({r.x + r.w * 0.5f, r.y + r.h * 0.5f});
            ui.update();
            return ui.cursor();
        };
        auto handleOf = [&](NodeHandle s) {
            for (NodeHandle c = ui.firstChild(s); ui.alive(c); c = ui.nextSibling(c))
                if (ui.registry().get(ui.classOf(c)).name == "splithandle") return c;
            return NodeHandle{};
        };
        UI_CHECK(at(le) == CursorShape::Text);
        UI_CHECK(at(ln) == CursorShape::Hand);
        UI_CHECK(at(bt) == CursorShape::Arrow);
        UI_CHECK(at(inner) == CursorShape::Crosshair); // inherited from the parent's style
        const NodeHandle h = handleOf(split), vh = handleOf(vsplit);
        UI_CHECK(ui.alive(h) && ui.alive(vh));
        UI_CHECK(at(h) == CursorShape::ResizeEW);
        UI_CHECK(at(vh) == CursorShape::ResizeNS);
        // a drag keeps the pressed handle's cursor even when the pointer leaves it
        const Rect hr = ui.rect(h);
        ui.pointerDown({hr.x + 3.f, hr.y + 10.f});
        ui.pointerMove({hr.x + 40.f, hr.y + 10.f});
        ui.update();
        UI_CHECK(ui.cursor() == CursorShape::ResizeEW);
        ui.pointerUp({hr.x + 40.f, hr.y + 10.f});
        UI_CHECK(std::string_view(cursorShapeName(CursorShape::NotAllowed)) == "not-allowed");
        PropValue v;
        UI_CHECK(parsePropValue(Prop::Cursor, "nwse-resize", v) && v.e == u8(CursorShape::ResizeNWSE));
        UI_CHECK(!parsePropValue(Prop::Cursor, "banana", v));
    }

    // ---- reduced motion ----
    {
        UiContext ui({.viewport = {200, 100}, .font = &font()});
        w::install(ui);
        const NodeHandle p = w::panel(ui, ui.root(), "p");
        ui.update();
        ui.animate(p, UiContext::AnimProp::OffsetX, 50.f, 1.f);
        ui.update(0.1f);
        const f32 normal = ui.rect(p).x;
        ui.setReducedMotion(true);
        ui.animate(p, UiContext::AnimProp::OffsetX, 80.f, 1.f);
        ui.update(0.016f);
        ui.update(0.f);
        std::printf("  animated x after 0.1 s: %.1f; reduced motion after one frame: %.1f\n", normal, ui.rect(p).x);
        UI_CHECK(normal < 40.f);
        UI_CHECK(std::abs(ui.rect(p).x - 80.f) < 0.5f && !ui.animating());
    }

    // ---- touch and pen ----
    {
        UiContext ui({.viewport = {400, 300}, .font = &font()});
        w::install(ui);
        const NodeHandle a = w::button(ui, ui.root(), "A", "a");
        const NodeHandle b = w::button(ui, ui.root(), "B", "b");
        ui.update();
        int ca = 0, cb = 0;
        ui.connect(a, SignalKind::Clicked, [&](UiContext&, NodeHandle, const SignalArgs&) { ++ca; });
        ui.connect(b, SignalKind::Clicked, [&](UiContext&, NodeHandle, const SignalArgs&) { ++cb; });
        const Rect ra = ui.rect(a), rb = ui.rect(b);
        const Vec2 pa{ra.x + ra.w * 0.5f, ra.y + ra.h * 0.5f}, pb{rb.x + rb.w * 0.5f, rb.y + rb.h * 0.5f};
        using T = fuse::platform::EventV2Type;
        auto touch = [&](T t, u32 id, Vec2 p) {
            fuse::platform::EventV2 e;
            e.type = t;
            e.pointerId = id;
            e.x = p.x * 2.f; // physical px at scale 2
            e.y = p.y * 2.f;
            feed(ui, e, 2.f);
            ui.update();
        };
        touch(T::TouchDown, 7, pa);
        UI_CHECK(ui.pointerKind() == UiContext::PointerKind::Touch);
        UI_CHECK(ui.touches().size() == 1 && ui.pressed() == a);
        touch(T::TouchDown, 9, pb); // second finger: gesture only, no press on B
        UI_CHECK(ui.touches().size() == 2 && ui.pressed() == a);
        UI_CHECK(std::abs(ui.pinchScale() - 1.f) < 1e-4f);
        touch(T::TouchMove, 9, {pb.x, pb.y + (pb.y - pa.y)}); // spread to twice the distance
        std::printf("  pinch scale %.3f\n", ui.pinchScale());
        UI_CHECK(std::abs(ui.pinchScale() - 2.f) < 0.01f);
        touch(T::TouchUp, 9, pb);
        touch(T::TouchUp, 7, pa);
        UI_CHECK(ca == 1 && cb == 0 && ui.touches().empty());
        UI_CHECK(!ui.hovered().valid()); // a lifted finger hovers nothing
        // pen: acts as the mouse, with pressure
        fuse::platform::EventV2 e;
        e.type = T::PenDown;
        e.x = pb.x;
        e.y = pb.y;
        e.pressure = 0.4f;
        feed(ui, e);
        UI_CHECK(ui.pointerKind() == UiContext::PointerKind::Pen && std::abs(ui.pressure() - 0.4f) < 1e-6f);
        e.type = T::PenUp;
        feed(ui, e);
        ui.update();
        UI_CHECK(cb == 1);
        // the kinetic scroll area still scrolls by touch drag (primary contact = pointer)
        UiContext k({.viewport = {200, 200}, .font = &font()});
        w::install(k);
        const NodeHandle area = w::kineticScrollArea(k, k.root(), w::ScrollProfile::ios(), "k");
        k.setCss(area, "height", "200px");
        for (int i = 0; i < 30; ++i) w::label(k, area, "row " + std::to_string(i));
        k.update();
        k.touchDown(1, {100.f, 150.f});
        for (int i = 1; i <= 10; ++i) { k.touchMove(1, {100.f, 150.f - f32(i) * 8.f}); k.update(1.f / 60.f); }
        k.touchUp(1, {100.f, 70.f});
        for (int i = 0; i < 30; ++i) k.update(1.f / 60.f);
        std::printf("  kinetic area scrolled by touch drag: %.1f\n", k.scroll(area).y);
        UI_CHECK(k.scroll(area).y > 20.f);
    }

    // ---- theme slot: dark -> light ----
    {
        UiContext ui({.viewport = {200, 100}, .font = &font()});
        w::install(ui);
        (void)ui.addStyleSheet(".mine { background: #ff0000; }");
        const NodeHandle p = w::panel(ui, ui.root(), "p");
        ui.addClass(p, "mine");
        ui.update();
        const Color dark = ui.computed(ui.root()).background;
        UI_CHECK(ui.setThemeStyleSheet(w::themeCss(w::ThemeVariant::Light)).has_value());
        ui.update();
        const Color light = ui.computed(ui.root()).background;
        std::printf("  root background dark %.2f, light %.2f\n", dark.r, light.r);
        UI_CHECK(dark.r < 0.3f && light.r > 0.8f);
        UI_CHECK(ui.computed(p).background.r > 0.99f && ui.computed(p).background.g < 0.01f); // app sheet kept
        UI_CHECK(ui.setThemeStyleSheet(w::defaultThemeCss()).has_value());
        ui.update();
        UI_CHECK(ui.computed(ui.root()).background.r == dark.r);
    }
    return finish("fuse_ui_platform_services");
}
