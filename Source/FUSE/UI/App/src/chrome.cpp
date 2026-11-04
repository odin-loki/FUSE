// fuse_app custom window chrome: title-bar widget + the platform-free hit test (see chrome.hpp).
#include "fuse/app/chrome.hpp"

#include "fuse/ui/widgets/widgets.hpp"

#include <string>

namespace fuse::app {
using namespace fuse::ui;

std::string_view hitAreaName(HitArea a) {
    static constexpr std::string_view kNames[] = {"client", "caption", "left", "right", "top", "bottom", "top-left",
                                                  "top-right", "bottom-left", "bottom-right", "minimize", "maximize", "close"};
    return kNames[int(a)];
}

HitArea chromeHitTest(const AppWindow& w, Vec2 p) {
    const WindowChrome& c = w.chrome();
    if (c.frame == WindowFrame::System) return HitArea::Client;
    const Size sz = w.size();
    const f32 b = c.maximized ? 0.f : c.resizeBorder;
    if (b > 0.f) {
        const bool l = p.x < b, r = p.x >= sz.w - b, t = p.y < b, bo = p.y >= sz.h - b;
        // corners grab a longer stretch along each edge, like the OS frame
        const f32 k = b * 2.f;
        const bool lk = p.x < k, rk = p.x >= sz.w - k, tk = p.y < k, bk = p.y >= sz.h - k;
        if ((t && lk) || (l && tk)) return HitArea::TopLeft;
        if ((t && rk) || (r && tk)) return HitArea::TopRight;
        if ((bo && lk) || (l && bk)) return HitArea::BottomLeft;
        if ((bo && rk) || (r && bk)) return HitArea::BottomRight;
        if (l) return HitArea::Left;
        if (r) return HitArea::Right;
        if (t) return HitArea::Top;
        if (bo) return HitArea::Bottom;
    }
    const UiContext& ui = w.ui();
    bool interactive = false;
    for (NodeHandle n = ui.hitTest(p); ui.alive(n); n = ui.parent(n)) {
        if (n == c.minimize) return HitArea::Minimize;
        if (n == c.maximize) return HitArea::Maximize;
        if (n == c.close) return HitArea::Close;
        if (n == c.caption) return interactive ? HitArea::Client : HitArea::Caption;
        if (ui.registry().get(ui.classOf(n)).interactive) interactive = true;
    }
    if (!interactive && ui.alive(c.caption) && ui.rect(c.caption).contains(p)) return HitArea::Caption;
    return HitArea::Client;
}

NodeHandle titleBar(AppWindow& w, NodeHandle parent, std::string_view title, f32 height) {
    namespace wd = fuse::ui::widgets;
    UiContext& ui = w.ui();
    const NodeHandle bar = wd::panel(ui, parent, "titlebar");
    const std::string h = std::to_string(int(height)) + "px";
    ui.setCss(bar, "flex-direction", "row");
    ui.setCss(bar, "align-items", "center");
    ui.setCss(bar, "height", h);
    ui.setCss(bar, "min-height", h);
    ui.setCss(bar, "padding", "0px");
    ui.setCss(bar, "padding-left", "10px");
    ui.setCss(bar, "gap", "0px");
    ui.setCss(bar, "border-width", "0px");
    ui.setCss(bar, "border-radius", "0px");
    ui.setCss(bar, "background", "#202028");
    const NodeHandle text = wd::label(ui, bar, title, "titlebar.title");
    ui.setCss(text, "flex-grow", "1");
    ui.setCss(text, "pointer-events", "false");
    WindowChrome& c = w.chrome();
    c.caption = bar;
    struct Btn { const char* glyph; const char* id; WindowCommand cmd; NodeHandle* slot; const char* cls; };
    const Btn btns[] = {{"\xE2\x80\x93", "titlebar.min", WindowCommand::Minimize, &c.minimize, "fuse-titlebar-btn"},       // –
                        {"\xE2\x96\xA1", "titlebar.max", WindowCommand::ToggleMaximize, &c.maximize, "fuse-titlebar-btn"}, // □
                        {"\xE2\x9C\x95", "titlebar.close", WindowCommand::Close, &c.close, "fuse-titlebar-close"}};       // ✕
    for (const Btn& b : btns) {
        const NodeHandle n = wd::toolButton(ui, bar, b.glyph, b.id);
        ui.setCss(n, "width", "46px");
        ui.setCss(n, "height", h);
        ui.setCss(n, "text-align", "center");
        ui.addClass(n, b.cls);
        *b.slot = n;
        AppWindow* win = &w;
        const WindowCommand cmd = b.cmd;
        ui.connect(n, SignalKind::Clicked, [win, cmd](UiContext&, NodeHandle, const SignalArgs&) { win->command(cmd); });
    }
    (void)ui.addStyleSheet(".fuse-titlebar-btn, .fuse-titlebar-close { background: #00000000; border-width: 0px; border-radius: 0px; }"
                           " .fuse-titlebar-btn:hover { background: #3A3A46; }"
                           " .fuse-titlebar-close:hover { background: #C42B1C; }");
    return bar;
}

} // namespace fuse::app
