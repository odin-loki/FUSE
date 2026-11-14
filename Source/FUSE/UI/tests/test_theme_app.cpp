// fuse_app_theme_sheets: AppWindow::applyTheme keeps one token override sheet however often the system theme changes
// (it used to add a sheet per call), takes its token sets from the widget library (light, dark, high contrast), and
// the paint-code tokens follow (a slider track pixel changes with the theme).
#include "fuse/app/app.hpp"
#include "fuse/app/chrome.hpp"
#include "test_util.hpp"

#include <cmath>
#include <cstdio>

using namespace fuse::ui;
namespace app = fuse::app;
namespace w = fuse::ui::widgets;

namespace {
struct VClock { double t = 0.0; };
double vnow(void* u) { return static_cast<VClock*>(u)->t; }
bool vpump(void* u, app::Application&, f32 wait) {
    static_cast<VClock*>(u)->t += std::isinf(wait) ? 0.25 : double(wait);
    return true;
}
} // namespace

int main() {
    VClock clock;
    app::Application a{{.name = "gate_theme_sheets", .loadSettings = false}};
    a.setClock({&clock, &vnow});
    a.setEventSource({&clock, &vpump});
    app::AppWindow& win = a.createWindow({.title = "theme", .width = 320, .height = 200});
    UiContext& ui = win.ui();
    const NodeHandle b = w::button(ui, ui.root(), "Button", "b");
    auto settle = [&] { for (int i = 0; i < 3; ++i) a.step(0.016f); };
    settle();
    win.setFollowSystemTheme(true);
    const u32 before = ui.styles().sheetCount();

    const app::SystemTheme light{.scheme = app::ColorScheme::Light};
    const app::SystemTheme dark{.scheme = app::ColorScheme::Dark, .hasAccent = true, .accent = Color::rgb(0xCC3366)};
    const app::SystemTheme contrast{.scheme = app::ColorScheme::Dark, .highContrast = true};
    // the app layer's token sheet is the widget library's
    UI_CHECK(app::themeTokensCss(light) == w::themeTokensCss({.variant = w::ThemeVariant::Light}));
    UI_CHECK(app::themeTokensCss(contrast) == w::themeTokensCss({.variant = w::ThemeVariant::HighContrast}));

    a.setSystemTheme(light);
    settle();
    UI_CHECK_EQ(ui.styles().sheetCount(), before + 1); // the one override sheet
    Color want;
    UI_CHECK(w::themeTokenColor({.variant = w::ThemeVariant::Light}, "--bg", want));
    UI_CHECK(ui.computed(ui.root()).background == want);
    UI_CHECK(w::themeTokenColor({.variant = w::ThemeVariant::Light}, "--text", want));
    UI_CHECK(ui.computed(b).color == want);
    UI_CHECK(w::themeTokenColor({.variant = w::ThemeVariant::Light}, "--track", want));
    UI_CHECK(ui.themeColor(hashId("--track"), {}) == want); // paint-code tokens follow

    for (int i = 0; i < 25; ++i) {
        a.setSystemTheme(i % 3 == 0 ? dark : i % 3 == 1 ? contrast : light);
        settle();
        UI_CHECK_EQ(ui.styles().sheetCount(), before + 1);
    }
    // 24 % 3 == 0: dark with the system accent
    UI_CHECK(ui.themeColor(hashId("--accent"), {}) == Color::rgb(0xCC3366));
    UI_CHECK(w::themeTokenColor({.variant = w::ThemeVariant::Dark}, "--bg", want));
    UI_CHECK(ui.computed(ui.root()).background == want);

    a.setSystemTheme(contrast);
    settle();
    const w::HighContrastPalette pal;
    UI_CHECK(ui.computed(ui.root()).background == pal.window);
    UI_CHECK(ui.computed(b).color == pal.windowText);
    UI_CHECK(ui.computed(b).borderWidth >= 1.f && ui.computed(b).borderColor == pal.windowText); // outlined controls
    ui.setFocus(b, FocusReason::Keyboard);
    settle();
    for (int i = 0; i < 30; ++i) a.step(0.016f);
    UI_CHECK(ui.computed(b).borderWidth >= 2.f && ui.computed(b).borderColor == pal.hotlight); // 2px hot-light ring
    a.setSystemTheme(light);
    settle();
    for (int i = 0; i < 30; ++i) a.step(0.016f);
    UI_CHECK(ui.computed(b).borderWidth < 2.f); // the outline rules left with the high-contrast sheet
    UI_CHECK_EQ(ui.styles().sheetCount(), before + 1);

    // The application's own @media (prefers-color-scheme / prefers-contrast) rules follow the theme the window shows:
    // applyTheme / widgets::setTheme drive UiContext::setMediaTheme. Tokens defined in such a block count only while
    // it applies (paint code reads them through themeColor).
    {
        UI_CHECK(ui.addStyleSheet("@media (prefers-color-scheme: dark) { .probe { width: 11px; } :root { --probe: #112233; } } "
                                  "@media (prefers-color-scheme: light) { .probe { width: 22px; } :root { --probe: #445566; } } "
                                  ".probe { height: 5px; } "
                                  "@media (prefers-contrast: more) { .probe { height: 33px; } } "
                                  "@media (prefers-contrast: no-preference) { .probe { min-width: 3px; } }").has_value());
        const NodeHandle probe = w::panel(ui, ui.root());
        ui.addClass(probe, "probe");
        settle();
        UI_CHECK(!ui.mediaDark() && !ui.mediaMoreContrast()); // light was applied last
        UI_CHECK(ui.computed(probe).width == Length::px(22.f) && ui.computed(probe).height == Length::px(5.f));
        UI_CHECK(ui.themeColor(hashId("--probe"), {}) == Color::rgb(0x445566));
        a.setSystemTheme(dark);
        settle();
        UI_CHECK(ui.mediaDark() && ui.computed(probe).width == Length::px(11.f));
        UI_CHECK(ui.themeColor(hashId("--probe"), {}) == Color::rgb(0x112233));
        UI_CHECK(ui.computed(probe).minWidth == Length::px(3.f));
        a.setSystemTheme(contrast); // the default high-contrast palette is a dark one
        settle();
        UI_CHECK(ui.mediaDark() && ui.mediaMoreContrast());
        UI_CHECK(ui.computed(probe).width == Length::px(11.f) && ui.computed(probe).height == Length::px(33.f));
        UI_CHECK(!(ui.computed(probe).minWidth == Length::px(3.f)));
        a.setSystemTheme(light);
        settle();
        UI_CHECK(ui.computed(probe).width == Length::px(22.f) && ui.computed(probe).height == Length::px(5.f));
        // pinned by the application (ProjectHost ui.theme "dark" / "light"): widgets::setTheme answers the same way
        win.setFollowSystemTheme(false);
        UI_CHECK(w::setTheme(ui, w::ThemeVariant::Dark).has_value());
        settle();
        UI_CHECK(ui.mediaDark() && !ui.mediaMoreContrast());
        UI_CHECK(w::setTheme(ui, w::ThemeVariant::HighContrast).has_value());
        settle();
        UI_CHECK(ui.mediaDark() && ui.mediaMoreContrast());
        UI_CHECK(w::setTheme(ui, {.variant = w::ThemeVariant::HighContrast, .contrast = {.window = Color::rgb(0xFFFFFF), .windowText = Color::rgb(0x000000)}}).has_value());
        settle();
        UI_CHECK(!ui.mediaDark() && ui.mediaMoreContrast()); // a light high-contrast palette
        UI_CHECK(w::setTheme(ui, w::ThemeVariant::Light).has_value());
        win.setFollowSystemTheme(true); // applies the system theme (light) again
        settle();
        UI_CHECK(!ui.mediaDark() && !ui.mediaMoreContrast());
    }

    // The client-side title bar (fuse::app::titleBar, also the Wayland client-side decoration) is styled from theme
    // tokens only: every theme switch restyles the bar and its buttons, and the close button's hover face is a token.
    {
        app::AppWindow& cw = a.createWindow({.title = "chrome", .width = 320, .height = 200, .frame = app::WindowFrame::Custom});
        UiContext& cu = cw.ui();
        const NodeHandle bar = app::titleBar(cw, cu.root(), "Chrome");
        const NodeHandle close = cw.chrome().close, mini = cw.chrome().minimize;
        cw.setFollowSystemTheme(true);
        const u32 sheets = cu.styles().sheetCount();
        struct Case { app::SystemTheme sys; w::ThemeDesc desc; };
        const Case cases[] = {{dark, {.variant = w::ThemeVariant::Dark, .hasAccent = true, .accent = Color::rgb(0xCC3366)}},
                              {light, {.variant = w::ThemeVariant::Light}},
                              {contrast, {.variant = w::ThemeVariant::HighContrast}},
                              {light, {.variant = w::ThemeVariant::Light}}};
        for (const Case& c : cases) {
            a.setSystemTheme(c.sys);
            cu.pointerMove({-10.f, -10.f});
            for (int i = 0; i < 40; ++i) a.step(0.016f);
            Color surface, text, hi, closeBg, onAccent;
            UI_CHECK(w::themeTokenColor(c.desc, "--surface", surface) && w::themeTokenColor(c.desc, "--text", text));
            UI_CHECK(w::themeTokenColor(c.desc, "--surface-hi", hi) && w::themeTokenColor(c.desc, "--window-close", closeBg));
            UI_CHECK(w::themeTokenColor(c.desc, "--on-accent", onAccent));
            UI_CHECK(cu.computed(bar).background == surface);
            UI_CHECK(cu.computed(cu.firstChild(bar)).color == text); // the title
            UI_CHECK(cu.computed(close).color == text && cu.computed(close).background.a == 0.f);
            const Rect rc = cu.rect(close), rm = cu.rect(mini);
            cu.pointerMove({rc.x + rc.w * 0.5f, rc.y + rc.h * 0.5f});
            for (int i = 0; i < 40; ++i) a.step(0.016f);
            UI_CHECK(cu.computed(close).background == closeBg && cu.computed(close).color == onAccent);
            if (c.desc.variant != w::ThemeVariant::HighContrast) { // (high contrast: every hovered tool button is the highlight)
                cu.pointerMove({rm.x + rm.w * 0.5f, rm.y + rm.h * 0.5f});
                for (int i = 0; i < 40; ++i) a.step(0.016f);
                UI_CHECK(cu.computed(mini).background == hi);
            }
        }
        UI_CHECK_EQ(cu.styles().sheetCount(), sheets); // the one override sheet, replaced in place
    }

    const int f = fuse::ui::test::failures();
    std::printf("fuse_app_theme_sheets: %s\n", f ? "FAILED" : "OK");
    return f ? 1 : 0;
}
