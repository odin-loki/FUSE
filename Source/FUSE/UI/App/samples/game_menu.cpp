// game_menu (plan UI5 / §7 sample): a game's main menu, pause menu and settings on one .fuseui document.
//   game_menu [--ui=path/game_menu.fuseui] [--pad=<script>] [--screenshot=out.png]
// One tree, four ways in: keyboard (Tab / arrows / Enter / Escape), gamepad (d-pad or stick moves the focus by
// spatial navigation, A accepts, B goes back, Start pauses; left / right on a slider or the settings tabs changes
// it), mouse and touch. Settings are two-way bindings to a model; the Controls tab rebinds keys (select an action,
// Rebind, press a key); Quit / Reset ask through a confirm dialog.
// This sample has no gamepad backend of its own: feed your platform's pad into `pad` below. `--pad=` plays a script
// instead (u d l r = d-pad, a b s = A / B / Start, . = an idle frame), which is also how the ctest gate drives it:
// with --screenshot the app runs a scripted session through all four input paths, checks every step, writes the
// frame and exits non-zero if a step did not do what it should.
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#include "fuse/ui/doc/document.hpp"
#include "fuse/ui/game/game_ui.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#ifndef FUSE_APP_SAMPLE_UI_DIR
#define FUSE_APP_SAMPLE_UI_DIR "ui"
#endif

#include "fuse/app/main.hpp" // FUSE_APP_MAIN: main on desktop; run by the platform entry point on Android / iOS

namespace {
namespace w = fuse::ui::widgets;
namespace g = fuse::ui::game;
using namespace fuse::ui;

enum Screen { kMain = 0, kSettings = 1, kPause = 2, kGame = 3 };
enum class Pending : u8 { None, Quit, ToMenu, ResetBindings };

struct KeyName { Key key; const char* name; };
constexpr KeyName kKeyNames[] = {
    {Key::Space, "Space"}, {Key::Enter, "Enter"}, {Key::Tab, "Tab"}, {Key::Backspace, "Backspace"}, {Key::Delete, "Delete"},
    {Key::Left, "Left"}, {Key::Right, "Right"}, {Key::Up, "Up"}, {Key::Down, "Down"}, {Key::Home, "Home"}, {Key::End, "End"},
    {Key::PageUp, "Page Up"}, {Key::PageDown, "Page Down"},
    {Key::A, "A"}, {Key::B, "B"}, {Key::C, "C"}, {Key::D, "D"}, {Key::E, "E"}, {Key::F, "F"}, {Key::G, "G"}, {Key::H, "H"},
    {Key::I, "I"}, {Key::J, "J"}, {Key::K, "K"}, {Key::L, "L"}, {Key::M, "M"}, {Key::N, "N"}, {Key::O, "O"}, {Key::P, "P"},
    {Key::Q, "Q"}, {Key::R, "R"}, {Key::S, "S"}, {Key::T, "T"}, {Key::U, "U"}, {Key::V, "V"}, {Key::W, "W"}, {Key::X, "X"},
    {Key::Y, "Y"}, {Key::Z, "Z"},
    {Key::Num0, "0"}, {Key::Num1, "1"}, {Key::Num2, "2"}, {Key::Num3, "3"}, {Key::Num4, "4"}, {Key::Num5, "5"},
    {Key::Num6, "6"}, {Key::Num7, "7"}, {Key::Num8, "8"}, {Key::Num9, "9"},
    {Key::F1, "F1"}, {Key::F2, "F2"}, {Key::F3, "F3"}, {Key::F4, "F4"}, {Key::F5, "F5"}, {Key::F6, "F6"},
    {Key::BracketLeft, "["}, {Key::BracketRight, "]"}};
const char* bindableKeyName(Key k) {
    for (const KeyName& n : kKeyNames)
        if (n.key == k) return n.name;
    return nullptr; // not a bindable key
}

struct Binding { const char* action; Key key; };
constexpr Binding kDefaults[] = {{"Move forward", Key::W}, {"Move back", Key::S}, {"Move left", Key::A}, {"Move right", Key::D},
                                 {"Jump", Key::Space}, {"Interact", Key::E}, {"Reload", Key::R}, {"Map", Key::M}};
constexpr u32 kBindings = u32(std::size(kDefaults));

// The root of the menu: Escape (and the gamepad's B, which GamepadNav sends as Escape) that nothing below consumed
// arrives here while bubbling and becomes the "back" of the current screen.
struct MenuRoot {
    static constexpr std::string_view kName = "menuroot";
    static constexpr A11yRole kRole = A11yRole::None;
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Bubble || e.type != EventType::KeyDown || e.key != Key::Escape) return false;
        ec.ui.emit(ec.node, SignalKind::Submitted, {.text = "escape"});
        return true;
    }
};

struct Menu {
    fuse::app::Application& app;
    UiContext& ui;
    doc::Model& model;
    Binding bindings[kBindings];
    Pending pending = Pending::None;
    int returnTo = kMain; ///< where Back leaves the settings to
    int capturing = -1;   ///< binding waiting for a key (the rebind prompt is open)
    bool started = false, quit = false;
    g::InputDevice device = g::InputDevice::KeyboardMouse;

    Menu(fuse::app::Application& a, UiContext& u, doc::Model& m) : app(a), ui(u), model(m) {
        for (u32 i = 0; i < kBindings; ++i) bindings[i] = kDefaults[i];
    }
    int screen() const { return int(model.number("screen")); }
    void listBindings() {
        std::string items;
        for (u32 i = 0; i < kBindings; ++i) {
            if (i) items += '\n';
            items += bindings[i].action;
            items += ":  ";
            items += bindableKeyName(bindings[i].key);
        }
        ui.setText(ui.find("controls.bindings"), items);
    }
    void hint() {
        std::string h(g::promptGlyph(device, g::Action::Accept));
        h += " select    ";
        h += g::promptGlyph(device, g::Action::Back);
        h += screen() == kGame ? " pause" : " back";
        model.set("hint", h);
    }
    void note(g::InputDevice d) {
        if (device == d) return;
        device = d;
        hint();
    }
    void show(int s) {
        model.set("screen", double(s));
        model.set("playLabel", started ? "Continue" : "New game");
        static constexpr const char* kFirst[] = {"main.play", "audio.master", "pause.resume", "game.pause"};
        ui.setFocus(ui.find(kFirst[s]), FocusReason::Keyboard);
        hint();
    }
    void confirm(Pending what, std::string_view text) {
        pending = what;
        model.set("confirm.text", text);
        model.set("confirm.open", 1.0);
    }
    void back() { // Escape / B, by screen
        switch (screen()) {
        case kSettings: show(returnTo); break;
        case kPause: show(kGame); break;
        case kGame: show(kPause); break;
        default: break; // the main menu has nowhere to go back to
        }
    }
    void action(std::string_view a) {
        if (a == "play") { started = true; show(kGame); }
        else if (a == "settings") {
            returnTo = screen();
            ui.setValue(w::tabWidgetBar(ui, ui.find("settings.tabs")), 0.f, true); // always opens on Audio
            show(kSettings);
        }
        else if (a == "back") show(returnTo);
        else if (a == "pause") show(kPause);
        else if (a == "resume") show(kGame);
        else if (a == "quit") confirm(Pending::Quit, "Quit to the desktop?");
        else if (a == "tomenu") confirm(Pending::ToMenu, "Quit to the main menu? Unsaved progress is lost.");
        else if (a == "reset") confirm(Pending::ResetBindings, "Reset every key binding to its default?");
        else if (a == "confirm.no") { pending = Pending::None; model.set("confirm.open", 0.0); }
        else if (a == "confirm.yes") {
            const Pending p = pending;
            pending = Pending::None;
            model.set("confirm.open", 0.0);
            if (p == Pending::Quit) { quit = true; app.quit(); }
            else if (p == Pending::ToMenu) { started = false; show(kMain); }
            else if (p == Pending::ResetBindings) {
                for (u32 i = 0; i < kBindings; ++i) bindings[i] = kDefaults[i];
                listBindings();
            }
        } else if (a == "rebind") {
            const int row = int(ui.value(ui.find("controls.bindings")));
            if (row < 0 || row >= int(kBindings)) return;
            capturing = row;
            model.set("rebind.action", bindings[row].action);
            model.set("rebind.open", 1.0);
        } else if (a == "rebind.cancel") { capturing = -1; model.set("rebind.open", 0.0); }
    }
    /// The rebind prompt is open and a key arrived at its (focused) Cancel button.
    void captured(Key k) {
        if (capturing < 0 || !bindableKeyName(k)) return;
        bindings[capturing].key = k;
        capturing = -1;
        model.set("rebind.open", 0.0);
        listBindings();
    }
    /// A dialog the user closed with Escape / an outside click: the model follows.
    void syncDialogs() {
        if (model.number("confirm.open") != 0.0 && !w::popupOpen(ui, ui.find("confirm"))) { pending = Pending::None; model.set("confirm.open", 0.0); }
        if (model.number("rebind.open") != 0.0 && !w::popupOpen(ui, ui.find("rebind"))) { capturing = -1; model.set("rebind.open", 0.0); }
    }
    /// One frame of gamepad: left / right change the focused slider or the settings tab, the rest is navigation.
    void pad(g::GamepadNav& nav, g::GamepadState s, f32 dt, bool& startHeld) {
        if (s.up || s.down || s.left || s.right || s.a || s.b || s.start || s.stick.x != 0.f || s.stick.y != 0.f) note(g::InputDevice::Xbox);
        if (s.start && !startHeld) {
            if (screen() == kGame) show(kPause);
            else if (screen() == kPause) show(kGame);
        }
        startHeld = s.start;
        const NodeHandle f = ui.focused();
        if ((s.left || s.right) && ui.alive(f) && capturing < 0) {
            const A11yRole role = ui.role(f);
            if (role == A11yRole::Slider || role == A11yRole::ComboBox) {
                if (!padHeld) ui.performA11yAction(f, s.right ? A11yAction::Increment : A11yAction::Decrement);
                padHeld = true;
                s.left = s.right = false; // not a focus move
            } else if (role == A11yRole::TabList) {
                if (!padHeld) ui.setValue(f, ui.value(f) + (s.right ? 1.f : -1.f), true);
                padHeld = true;
                s.left = s.right = false;
            }
        } else if (!s.left && !s.right) {
            padHeld = false;
        }
        nav.update(s, dt);
    }
    bool padHeld = false;
};

/// Slot for the rebind prompt (signal slots hold trivially copyable captures: a pointer to the menu).
void onRebindKey(Menu* m, const SignalArgs& a) {
    if (a.event && a.event->type == EventType::KeyDown && a.event->key != Key::Escape) m->captured(a.event->key);
}

g::GamepadState padFrom(char c) {
    g::GamepadState s;
    switch (c) {
    case 'u': s.up = true; break;
    case 'd': s.down = true; break;
    case 'l': s.left = true; break;
    case 'r': s.right = true; break;
    case 'a': s.a = true; break;
    case 'b': s.b = true; break;
    case 's': s.start = true; break;
    default: break;
    }
    return s;
}
} // namespace

int FUSE_APP_MAIN(int argc, char** argv) {
    fuse::app::Application app{{.name = "game_menu", .argc = argc, .argv = argv, .loadSettings = false}};
    auto& window = app.createWindow({.title = "Starfall", .width = 800, .height = 540});
    UiContext& ui = window.ui();
    ui.registry().registerWidget<MenuRoot>();
    doc::Model model;
    model.set("screen", double(kMain));
    model.set("settingsTab", 0.0);
    model.set("playLabel", "New game");
    model.set("hint", "");
    model.set("audio.master", 0.8);
    model.set("audio.music", 0.6);
    model.set("audio.sfx", 0.7);
    model.set("audio.muteBackground", 1.0);
    model.set("video.quality", 2.0);
    model.set("video.brightness", 1.0);
    model.set("video.vsync", 1.0);
    model.set("video.fullscreen", 0.0);
    model.set("controls.sensitivity", 1.0);
    model.set("confirm.open", 0.0);
    model.set("confirm.text", "");
    model.set("rebind.open", 0.0);
    model.set("rebind.action", "");
    const std::string path(app.args().get("ui", FUSE_APP_SAMPLE_UI_DIR "/game_menu.fuseui"));
    if (auto loaded = window.load(path, &model); !loaded) return app.fail(loaded.error());
    // the document's root becomes a child of a "menuroot" that turns unconsumed Escape into Back
    const NodeHandle root = ui.create("menuroot", ui.root()).value_or(NodeHandle{});
    ui.setCss(root, "flex-grow", "1");
    ui.moveTo(ui.find("game"), root);

    Menu menu(app, ui, model);
    Menu* m = &menu;
    menu.listBindings();
    window.document()->onAction([m](std::string_view a, NodeHandle, const SignalArgs&) { m->action(a); });
    ui.connect(root, SignalKind::Submitted, [m](UiContext&, NodeHandle, const SignalArgs&) { m->back(); });
    ui.connect(ui.find("rebind.cancel"), SignalKind::Event, [m](UiContext&, NodeHandle, const SignalArgs& a) { onRebindKey(m, a); });
    g::GamepadNav nav(ui);
    bool startHeld = false;
    menu.show(kMain);

    // one frame: gamepad, the document's bindings, the UI
    std::string script(app.args().get("pad"));
    std::size_t scriptAt = 0;
    auto frame = [&](g::GamepadState s) {
        menu.pad(nav, s, 1.f / 60.f, startHeld);
        window.document()->refresh();
        app.step(0.f);
        menu.syncDialogs();
        window.document()->refresh();
        app.step(0.f);
    };
    auto press = [&](char c) { // a button down for one frame, then released
        frame(padFrom(c));
        frame({});
    };

    if (const std::string_view shot = app.args().get("screenshot"); !shot.empty()) {
        int failed = 0;
        auto check = [&](bool ok, const char* what) {
            if (!ok) { std::fprintf(stderr, "game_menu: FAILED: %s\n", what); ++failed; }
        };
        auto centre = [&](std::string_view id) {
            const Rect r = ui.rect(ui.find(id));
            return Vec2{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
        };
        frame({});
        // gamepad: down to Settings, A opens it
        check(ui.focused() == ui.find("main.play"), "the main menu starts on its first button");
        press('d');
        check(ui.focused() == ui.find("main.settings"), "d-pad down moves the focus to Settings");
        press('a');
        check(menu.screen() == kSettings && ui.focused() == ui.find("audio.master"), "A on Settings opens the settings");
        // gamepad: right on a slider changes it (two steps of 0.05)
        press('r');
        press('r');
        check(std::abs(model.number("audio.master") - 0.9) < 1e-4, "d-pad right raises the focused slider");
        press('d');
        check(ui.focused() == ui.find("audio.music"), "d-pad down goes to the next slider");
        // mouse: the Video tab, then touch: the V-Sync switch
        const NodeHandle bar = w::tabWidgetBar(ui, ui.find("settings.tabs"));
        std::vector<A11ySnapshotNode> snap;
        ui.snapshotA11y(snap);
        Rect video{};
        for (const A11ySnapshotNode& s : snap)
            if (s.node == bar && s.role == A11yRole::Tab && s.name == "Video") video = s.bounds;
        window.click({video.x + video.w * 0.5f, video.y + video.h * 0.5f});
        frame({});
        check(ui.value(bar) == 1.f && ui.rect(ui.find("video.vsync")).w > 0.f, "a mouse click on the Video tab shows it");
        const Vec2 sw = centre("video.vsync");
        ui.touchDown(sw, 0.0);
        ui.touchUp(sw, 0.05);
        menu.note(g::InputDevice::Touch);
        frame({});
        check(model.number("video.vsync") == 0.0, "a tap on the V-Sync switch turns it off");
        check(std::string(model.string("hint")).find("Tap") != std::string::npos, "the prompts follow the input device (touch)");
        // gamepad on the tab bar: right goes to Controls
        ui.setFocus(bar, FocusReason::Keyboard);
        press('r');
        check(ui.value(bar) == 2.f && ui.rect(ui.find("controls.bindings")).h > 0.f, "d-pad right on the tabs shows Controls");
        check(std::string(model.string("hint")).find("A ") == 0, "the prompts follow the input device (gamepad)");
        // keyboard: pick "Jump" in the list, Enter asks for a key, J rebinds it
        const NodeHandle list = ui.find("controls.bindings");
        ui.setFocus(list, FocusReason::Keyboard);
        for (int i = 0; i < 5; ++i) window.key(Key::Down);
        frame({});
        check(ui.value(list) == 4.f && w::listItem(ui, list, 4) == "Jump:  Space", "the keyboard selects the Jump binding");
        window.key(Key::Enter);
        frame({});
        check(menu.capturing == 4 && w::popupOpen(ui, ui.find("rebind")), "Enter on a binding opens the rebind prompt");
        window.key(Key::J);
        frame({});
        check(menu.bindings[4].key == Key::J && w::listItem(ui, list, 4) == "Jump:  J" && !w::popupOpen(ui, ui.find("rebind")),
              "the next key press becomes the binding and closes the prompt");
        // Escape cancels a prompt without rebinding
        window.key(Key::Enter);
        frame({});
        window.key(Key::Escape);
        frame({});
        check(menu.capturing == -1 && menu.bindings[4].key == Key::J && model.number("rebind.open") == 0.0, "Escape cancels the rebind prompt");
        // mouse on Reset, gamepad A on the confirm dialog's Yes
        window.click(centre("controls.reset"));
        frame({});
        check(w::popupOpen(ui, ui.find("confirm")) && ui.focused() == ui.find("confirm.yes"), "Reset asks first (focus on Yes)");
        press('a');
        check(menu.bindings[4].key == Key::Space && !w::popupOpen(ui, ui.find("confirm")), "A on Yes resets the bindings");
        // gamepad B: back to the main menu; New game; Start pauses and resumes; B in the pause menu resumes
        press('b');
        check(menu.screen() == kMain, "B leaves the settings");
        press('a');
        check(menu.screen() == kGame && menu.started, "A on New game starts");
        press('s');
        check(menu.screen() == kPause, "Start pauses");
        press('b');
        check(menu.screen() == kGame, "B resumes");
        window.key(Key::Escape);
        frame({});
        check(menu.screen() == kPause, "Escape pauses");
        // quit: No keeps running, Yes quits
        ui.setFocus(ui.find("pause.menu"), FocusReason::Keyboard);
        window.key(Key::Enter);
        frame({});
        check(w::popupOpen(ui, ui.find("confirm")), "Quit to main menu asks first");
        window.click(centre("confirm.no"));
        frame({});
        check(menu.screen() == kPause && !w::popupOpen(ui, ui.find("confirm")), "No changes nothing");
        window.key(Key::Enter);
        frame({});
        window.click(centre("confirm.yes"));
        frame({});
        check(menu.screen() == kMain && !menu.started && std::string(model.string("playLabel")) == "New game", "Yes returns to the main menu");
        window.click(centre("main.quit"));
        frame({});
        window.click(centre("confirm.yes"));
        check(menu.quit && app.quitting(), "Quit, confirmed, ends the app");
        // the picture: the settings, Audio tab, as the gamepad sees it
        menu.action("confirm.no");
        menu.action("settings");
        menu.note(g::InputDevice::Xbox);
        window.document()->refresh();
        ui.update(0.5f);
        ui.update(0.f);
        window.requestRedraw();
        window.rasterize({});
        if (!window.writePng(std::string(shot))) return app.fail("cannot write screenshot");
        std::printf("game_menu: %d failed step(s), %u nodes, wrote %.*s\n", failed, ui.nodeCount(), int(shot.size()), shot.data());
        return failed ? 1 : 0;
    }

    fuse::app::NativePlatform native(app);
    native.show(window);
    // the loop: one frame at least every 1/60 s while a pad script plays, on demand otherwise
    while (!app.quitting() && app.windowCount() > 0) {
        g::GamepadState pad; // <- your platform's gamepad goes here
        if (scriptAt < script.size()) pad = padFrom(script[scriptAt++]);
        menu.pad(nav, pad, 1.f / 60.f, startHeld);
        menu.syncDialogs();
        window.document()->refresh();
        if (!app.step(scriptAt < script.size() ? 1.f / 60.f : 1e30f)) break;
    }
    return 0;
}
