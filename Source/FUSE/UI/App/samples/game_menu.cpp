// game_menu (plan UI5 sample): a game-style main menu. Title, Play / Options / Quit over an animated background,
// on a backdrop-blurred panel; an Options page (resolution, volume sliders, key bindings list, Back) reached with
// animated page transitions; gamepad navigation through fuse_ui_game's GamepadNav (d-pad / stick move focus
// spatially, A accepts, B goes back; left / right adjust a focused slider).
//   game_menu [--screenshot=out.png] [--script]
// --screenshot runs headless on a virtual 20 Hz clock and writes the frame. --script first plays a simulated
// gamepad session (Down, A -> Options page; Down to "Master volume"; Right x3) and checks where it ended up: the
// exit code is non-zero when the menu did not respond as expected.
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#include "fuse/ui/game/game_ui.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace w = fuse::ui::widgets;
namespace game = fuse::ui::game;
using namespace fuse::ui;
using Anim = UiContext::AnimProp;

namespace {
constexpr const char* kStyle = R"(
#menu { background: rgba(14, 16, 28, 0.55); backdrop-blur: 18px; border-radius: 18px; border-width: 1px;
        border-color: rgba(255, 255, 255, 0.18); padding: 28px; gap: 10px; width: 380px;
        shadow-color: rgba(0, 0, 0, 0.45); shadow-blur: 30px; shadow-y: 10px; }
#title { font-size: 34px; color: #FFFFFF; text-align: center; }
#subtitle { font-size: 13px; color: #A8B4D8; text-align: center; margin-bottom: 14px; }
.menu-btn { font-size: 20px; padding: 10px; border-radius: 10px; border-width: 0px; background: rgba(255, 255, 255, 0.06);
            color: #E8ECFF; }
.menu-btn:hover { background: rgba(255, 255, 255, 0.14); }
.menu-btn:focus-visible { background: #3B6CFF; color: #FFFFFF; }
.row { flex-direction: row; align-items: center; gap: 10px; }
.row-label { width: 130px; color: #C8D0F0; }
.value { width: 44px; color: #FFFFFF; text-align: end; }
.page { gap: 10px; }
#hint { font-size: 12px; color: #8A94B8; text-align: center; margin-top: 8px; }
)";

struct Menu {
    fuse::app::Application& app;
    UiContext& ui;
    NodeHandle bg, menu, mainPage, optionsPage, play, options, quit, resolution, master, music, bindings, back, hint;
    NodeHandle masterValue, musicValue;
    std::vector<NodeHandle> orbs;
    NodeHandle current, next;
    u32 orbStep = 0;

    static void setPct(UiContext& ui, NodeHandle label, f32 v) { ui.setText(label, std::to_string(int(std::lround(v * 100.f))) + "%"); }

    // page transition: the old page fades and slides out, then the new one slides in from the right
    void show(NodeHandle page) {
        if (page == current || next.valid()) return;
        next = page;
        ui.animate(current, Anim::Opacity, 0.f, 0.15f);
        ui.animate(current, Anim::OffsetX, -40.f, 0.15f);
        ui.animate(page, Anim::Opacity, 0.f, 0.f);
        ui.animate(page, Anim::OffsetX, 60.f, 0.f);
        ui.startTimer(0.15f, false, [this](UiContext&) { swapPages(); });
    }
    void swapPages() {
        ui.setCss(current, "display", "none");
        ui.animate(current, Anim::Opacity, 1.f, 0.f);
        ui.animate(current, Anim::OffsetX, 0.f, 0.f);
        ui.setCss(next, "display", "flex");
        ui.animate(next, Anim::Opacity, 1.f, 0.25f);
        ui.animate(next, Anim::OffsetX, 0.f, 0.25f, UiContext::Ease::OutBack);
        current = next;
        next = {};
        ui.setFocus(current == optionsPage ? resolution : options, FocusReason::Keyboard);
    }
    // background orbs drift to new spots every few seconds (deterministic, so screenshots are stable)
    void driftOrbs() {
        for (std::size_t i = 0; i < orbs.size(); ++i) {
            const f32 a = f32(orbStep) * 1.7f + f32(i) * 2.4f;
            ui.animate(orbs[i], Anim::OffsetX, std::sin(a) * 120.f, 4.f);
            ui.animate(orbs[i], Anim::OffsetY, std::cos(a * 1.3f) * 80.f, 4.f);
        }
        ++orbStep;
    }

    void build() {
        (void)ui.addStyleSheet(kStyle);
        const NodeHandle root = ui.root();
        ui.setCss(root, "layout", "anchor");
        ui.setCss(root, "padding", "0px");
        // animated background: a vertical gradient with soft coloured orbs drifting behind the panel
        bg = w::panel(ui, root, "bg");
        for (const char* p : {"left", "top", "right", "bottom"}) ui.setCss(bg, p, "0px");
        ui.setCss(bg, "layout", "anchor");
        ui.setCss(bg, "border-width", "0px");
        ui.setCss(bg, "border-radius", "0px");
        ui.setCss(bg, "gradient", "vertical");
        ui.setCss(bg, "background", "#141A3A");
        ui.setCss(bg, "background-end", "#3A1240");
        ui.setCss(bg, "pointer-events", "none");
        const char* colors[] = {"#FF4D8D", "#3BC9FF", "#FFB23B", "#7A5CFF"};
        const char* lefts[] = {"12%", "62%", "30%", "75%"};
        const char* tops[] = {"18%", "12%", "62%", "58%"};
        for (int i = 0; i < 4; ++i) {
            const NodeHandle o = w::panel(ui, bg);
            ui.setCss(o, "left", lefts[i]);
            ui.setCss(o, "top", tops[i]);
            ui.setCss(o, "width", "180px");
            ui.setCss(o, "height", "180px");
            ui.setCss(o, "border-radius", "90px");
            ui.setCss(o, "border-width", "0px");
            ui.setCss(o, "background", colors[i]);
            ui.setCss(o, "opacity", "0.75");
            orbs.push_back(o);
        }
        // the menu panel, centred, frosted glass over the background
        menu = w::panel(ui, root, "menu");
        ui.setCss(menu, "center-x", "true");
        ui.setCss(menu, "center-y", "true");
        w::label(ui, menu, "FUSE ARENA", "title");
        w::label(ui, menu, "a FUSE UI game menu sample", "subtitle");

        mainPage = w::panel(ui, menu, "main");
        optionsPage = w::panel(ui, menu, "options");
        for (NodeHandle p : {mainPage, optionsPage}) {
            ui.addClass(p, "page");
            ui.setCss(p, "background", "#00000000");
            ui.setCss(p, "border-width", "0px");
            ui.setCss(p, "padding", "0px");
        }
        auto menuButton = [&](NodeHandle parent, const char* text, const char* id) {
            const NodeHandle b = w::button(ui, parent, text, id);
            ui.addClass(b, "menu-btn");
            return b;
        };
        play = menuButton(mainPage, "Play", "play");
        options = menuButton(mainPage, "Options", "options-btn");
        quit = menuButton(mainPage, "Quit", "quit");

        auto row = [&](const char* text) {
            const NodeHandle r = w::panel(ui, optionsPage);
            ui.addClass(r, "row");
            ui.setCss(r, "background", "#00000000");
            ui.setCss(r, "border-width", "0px");
            ui.setCss(r, "padding", "0px");
            const NodeHandle l = w::label(ui, r, text);
            ui.addClass(l, "row-label");
            return r;
        };
        resolution = w::comboBox(ui, row("Resolution"), "1280 x 720\n1920 x 1080\n2560 x 1440\n3840 x 2160", 1, "resolution");
        ui.setCss(resolution, "flex-grow", "1");
        auto volume = [&](const char* text, const char* id, f32 v, NodeHandle& valueLabel) {
            const NodeHandle r = row(text);
            const NodeHandle s = w::slider(ui, r, 0.f, 1.f, v, 0.05f, id);
            ui.setCss(s, "flex-grow", "1");
            valueLabel = w::label(ui, r, "");
            ui.addClass(valueLabel, "value");
            setPct(ui, valueLabel, v);
            const NodeHandle vl = valueLabel;
            ui.connect(s, SignalKind::ValueChanged, [vl](UiContext& c, NodeHandle, const SignalArgs& a) { setPct(c, vl, a.value); });
            return s;
        };
        master = volume("Master volume", "master", f32(app.settings().number("audio.master", 0.8)), masterValue);
        music = volume("Music volume", "music", f32(app.settings().number("audio.music", 0.6)), musicValue);
        const NodeHandle kb = w::label(ui, optionsPage, "Key bindings");
        ui.addClass(kb, "row-label");
        bindings = w::listView(ui, optionsPage,
                               "[W]   Move forward\n[S]   Move back\n[A]   Strafe left\n[D]   Strafe right\n[Space]   Jump\n"
                               "[E]   Interact",
                               -1, "bindings");
        ui.setCss(bindings, "height", "120px");
        back = menuButton(optionsPage, "Back", "back");
        ui.setCss(optionsPage, "display", "none");
        hint = w::label(ui, menu, "D-pad / arrows: move   A / Enter: select   B: back", "hint");

        ui.connect(options, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { show(optionsPage); });
        ui.connect(back, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { show(mainPage); });
        ui.connect(quit, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { app.quit(); });
        ui.connect(play, SignalKind::Clicked, [this](UiContext& c, NodeHandle, const SignalArgs&) { c.setText(hint, "Loading arena..."); });
        ui.connect(master, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { app.settings().set("audio.master", double(a.value)); });
        ui.connect(music, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { app.settings().set("audio.music", double(a.value)); });

        current = mainPage;
        ui.setSpatialNavigation(true);
        ui.setFocus(play, FocusReason::Keyboard);
        // entrance: the panel fades in and rises
        ui.animate(menu, Anim::Opacity, 0.f, 0.f);
        ui.animate(menu, Anim::OffsetY, 30.f, 0.f);
        ui.startTimer(0.05f, false, [this](UiContext& c) {
            c.animate(menu, Anim::Opacity, 1.f, 0.4f);
            c.animate(menu, Anim::OffsetY, 0.f, 0.5f, UiContext::Ease::OutBack);
        });
        driftOrbs();
        ui.startTimer(4.f, true, [this](UiContext&) { driftOrbs(); });
    }
};

/// Gamepad -> menu: GamepadNav moves focus and accepts; left / right on a focused slider step it; B goes back.
struct Pad {
    Menu& m;
    game::GamepadNav nav;
    bool left = false, right = false;
    u32 backs = 0;
    Pad(Menu& menu) : m(menu), nav(menu.ui) {}
    void update(game::GamepadState s, f32 dt) {
        UiContext& ui = m.ui;
        const bool onSlider = ui.focused() == m.master || ui.focused() == m.music;
        if (onSlider) {
            if (s.left && !left) ui.keyDown(Key::Left);
            if (s.right && !right) ui.keyDown(Key::Right);
            left = s.left;
            right = s.right;
            s.left = s.right = false;
        } else {
            left = right = false;
        }
        nav.update(s, dt);
        if (nav.backs() != backs) {
            backs = nav.backs();
            if (m.current == m.optionsPage) m.show(m.mainPage);
        }
    }
};

struct VirtualClock {
    double t = 0.0;
    static double now(void* u) { return static_cast<VirtualClock*>(u)->t; }
};
} // namespace

int main(int argc, char** argv) {
    const fuse::app::CommandLine cl(argc, argv);
    const std::string shot(cl.get("screenshot"));
    fuse::app::Application app{{.name = "game_menu", .argc = argc, .argv = argv, .loadSettings = shot.empty()}};
    auto& window = app.createWindow({.title = "FUSE Arena", .width = 960, .height = 600});
    Menu menu{app, window.ui()};
    menu.build();
    Pad pad(menu);

    if (!shot.empty()) {
        // headless, virtual 20 Hz clock
        VirtualClock clock;
        app.setClock({&clock, &VirtualClock::now});
        constexpr f32 kDt = 1.f / 20.f; // CPU raster of every animated frame: keep the run short
        auto run = [&](f32 seconds, game::GamepadState s = {}) {
            for (int i = 0, n = int(std::lround(seconds / kDt)); i < n; ++i) {
                pad.update(s, kDt);
                clock.t += kDt;
                app.step(0.f);
            }
        };
        run(1.f); // entrance animation
        int failures = 0;
        auto expect = [&](bool ok, const char* what) {
            std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
            failures += ok ? 0 : 1;
        };
        if (cl.has("script")) {
            UiContext& ui = window.ui();
            expect(ui.focused() == menu.play, "starts focused on Play");
            run(0.1f, {.down = true});
            run(0.1f);
            expect(ui.focused() == menu.options, "d-pad down focuses Options");
            run(0.1f, {.a = true});
            run(0.6f);
            expect(menu.current == menu.optionsPage && ui.computed(menu.optionsPage).visible && !ui.computed(menu.mainPage).visible,
                   "A opens the Options page (animated transition done)");
            expect(ui.focused() == menu.resolution, "Options page focuses Resolution");
            run(0.1f, {.down = true});
            run(0.1f);
            expect(ui.focused() == menu.master, "d-pad down focuses Master volume");
            const f32 before = ui.value(menu.master);
            for (int i = 0; i < 3; ++i) {
                run(0.1f, {.right = true});
                run(0.1f);
            }
            const f32 after = ui.value(menu.master);
            std::printf("  master volume %.2f -> %.2f (label \"%.*s\")\n", before, after, int(ui.text(menu.masterValue).size()),
                        ui.text(menu.masterValue).data());
            expect(std::fabs(after - std::min(1.f, before + 0.15f)) < 1e-3f, "d-pad right x3 raises Master volume by 3 steps");
            expect(ui.text(menu.masterValue) == std::to_string(int(std::lround(after * 100.f))) + "%", "value label follows the slider");
            expect(ui.focused() == menu.master, "focus stays on the slider while adjusting");
            run(1.f);
        }
        window.ui().pointerMove({1.f, 1.f});
        run(0.1f);
        if (!window.writePng(shot)) return app.fail("cannot write screenshot");
        std::FILE* f = std::fopen(shot.c_str(), "rb");
        long size = 0;
        if (f) {
            std::fseek(f, 0, SEEK_END);
            size = std::ftell(f);
            std::fclose(f);
        }
        expect(size > 1000, "screenshot written");
        std::printf("game_menu: %llu frames, wrote %s (%ld bytes)%s\n", static_cast<unsigned long long>(window.framesPresented()),
                    shot.c_str(), size, failures ? ", FAILED" : "");
        return failures ? 1 : 0;
    }
    fuse::app::NativePlatform native(app);
    native.show(window);
    window.setRedrawPolicy(fuse::app::RedrawPolicy::OnDemand); // the drifting background keeps it animating
    return app.run();
}
