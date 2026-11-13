// fuse_app_theme_sheets: AppWindow::applyTheme keeps one token override sheet however often the system theme changes
// (it used to add a sheet per call), takes its token sets from the widget library (light, dark, high contrast), and
// the paint-code tokens follow (a slider track pixel changes with the theme).
#include "fuse/app/app.hpp"
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

    const int f = fuse::ui::test::failures();
    std::printf("fuse_app_theme_sheets: %s\n", f ? "FAILED" : "OK");
    return f ? 1 : 0;
}
