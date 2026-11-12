// widget_gallery (plan §7 sample): every widget of the catalogue on tabbed pages - a manual test bed for devices.
//   widget_gallery [--ui=path/gallery.fuseui] [--page=N] [--screenshot=out.png]
// The pages are a .fuseui document (ui/gallery.fuseui, loaded through fuse::app::readResource on Android / iOS); the
// widgets that only exist as calls (message box, wizard, popover, toast, file dialog) open from the tool bar, and
// the Game page is filled with the fuse_ui_game HUD widgets when the app is built with them.
// A few things are set up in code on top of the document: the Edit menu is built from an ActionRegistry (shortcut
// texts, a disabled and a checkable action), the table selects several rows at once (Ctrl / Shift click) and the
// tree table next to it is editable (double click, F2 or just type).
// With --screenshot the app shows page N, checks that the page is really there, writes the frame and exits.
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#include "fuse/ui/actions.hpp"
#include "fuse/ui/doc/document.hpp"
#include "fuse/ui/widgets/modelview.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#if FUSE_GALLERY_HAVE_GAME
#include "fuse/ui/game/game_extras.hpp"
#include "fuse/ui/game/game_ui.hpp"
#endif

#include <cstdio>
#include <string>

#ifndef FUSE_APP_SAMPLE_UI_DIR
#define FUSE_APP_SAMPLE_UI_DIR "ui"
#endif

#include "fuse/app/main.hpp" // Android / iOS: this main runs under the platform entry point

namespace {
namespace w = fuse::ui::widgets;
using fuse::ui::NodeHandle;
using fuse::ui::SignalArgs;
using fuse::ui::SignalKind;
using fuse::ui::UiContext;

constexpr const char* kPageIds[] = {"page.basic", "page.containers", "page.views", "page.text", "page.pro", "page.window", "page.game"};
/// One widget on each page that must be laid out for the page to count as shown.
constexpr const char* kPageProbe[] = {"g.button", "g.groupbox", "g.tableview", "g.codeedit", "g.calendar", "dock.outline", "g.game.note"};

/// A checker board with a gradient: stands in for a rendered scene / a picture.
void checker(fuse::ui::Image& img, int wd, int ht) {
    img.resize(wd, ht, 0xFF000000u);
    for (int y = 0; y < ht; ++y)
        for (int x = 0; x < wd; ++x) {
            const bool on = ((x / 8) + (y / 8)) & 1;
            const fuse::ui::u32 r = fuse::ui::u32(40 + x * 180 / wd), g = fuse::ui::u32(on ? 150 : 70), b = fuse::ui::u32(60 + y * 160 / ht);
            img.pixels[std::size_t(y) * std::size_t(wd) + std::size_t(x)] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
}

/// True when the node and every ancestor is displayed.
bool shownOnScreen(const UiContext& ui, NodeHandle n) {
    if (!ui.alive(n)) return false;
    for (; ui.alive(n); n = ui.parent(n))
        if (!ui.computed(n).visible) return false;
    return true;
}

#if FUSE_GALLERY_HAVE_GAME
void buildGamePage(UiContext& ui, NodeHandle page) {
    namespace g = fuse::ui::game;
    g::install(ui);
    g::installExtras(ui);
    w::label(ui, page, "HUD bar, cooldown, compass");
    const NodeHandle row = w::panel(ui, page, "game.row1");
    ui.addClass(row, "row");
    const NodeHandle hp = g::hudBar(ui, row, 1.f, "game.hp");
    ui.setCss(hp, "width", "220px");
    g::setHudBar(ui, hp, 0.55f);
    g::setCooldown(ui, g::cooldownDial(ui, row, "Q", "game.cooldown"), 0.35f);
    const NodeHandle comp = g::compass(ui, row, "game.compass");
    ui.setCss(comp, "width", "260px");
    g::setCompass(ui, comp, 40.f, 95.f);
    w::label(ui, page, "Minimap, ammo counter, radial menu");
    const NodeHandle row2 = w::panel(ui, page, "game.row2");
    ui.addClass(row2, "row");
    const NodeHandle map = g::minimap(ui, row2, "game.minimap");
    ui.setCss(map, "width", "150px");
    ui.setCss(map, "height", "150px");
    const g::MinimapBlip blips[] = {{{10.f, 20.f}, fuse::ui::Color::rgb(0xFF5C5C)},
                                    {{-25.f, 5.f}, fuse::ui::Color::rgb(0x5CC8FF), g::MinimapBlipKind::Square},
                                    {{80.f, -60.f}, fuse::ui::Color::rgb(0xFFD75C), g::MinimapBlipKind::Diamond}};
    g::minimapSetBlips(ui, map, blips);
    g::setMinimapHeading(ui, map, 30.f, true);
    const NodeHandle ammo = g::ammoCounter(ui, row2, "game.ammo");
    g::setAmmo(ui, ammo, 12, 30, 90);
    const NodeHandle radial = g::radialMenu(ui, row2, "Attack\nDefend\nItem\nFlee", "game.radial");
    ui.setCss(radial, "width", "180px");
    ui.setCss(radial, "height", "180px");
    g::radialSelect(ui, radial, {1.f, 0.f});
}
#endif
} // namespace

int main(int argc, char** argv) {
    fuse::app::Application app{{.name = "widget_gallery", .argc = argc, .argv = argv, .loadSettings = false}};
    auto& window = app.createWindow({.title = "FUSE UI widget gallery", .width = 1100, .height = 760});
    UiContext& ui = window.ui();
    fuse::ui::doc::Model model;
    model.set("clicks", 0.0);
    model.set("flag", 1.0);
    model.set("level", 40.0);
    model.set("name", "FUSE");
    model.set("stackPage", 0.0);
    model.set("dialogOpen", 0.0);
    const int page = int(app.args().number("page", 0));
    model.set("page", double(page < 0 || page > 6 ? 0 : page));
    const std::string path(app.args().get("ui", FUSE_APP_SAMPLE_UI_DIR "/gallery.fuseui"));
    if (auto loaded = window.load(path, &model); !loaded) return app.fail(loaded.error());

    // images: one checker board for the image / viewport widgets, the video widget's own frame image
    static fuse::ui::Image picture;
    checker(picture, 64, 36);
    const fuse::ui::u32 pictureIndex = window.addImage(&picture);
    ui.setUserData(ui.find("g.image"), pictureIndex);
    w::viewportSetImage(ui, ui.find("g.viewport"), pictureIndex, 64, 36);
    if (const NodeHandle video = ui.find("g.video"); ui.alive(video))
        if (const fuse::ui::Image* frame = w::videoImage(ui, video)) w::videoSetImageIndex(ui, video, window.addImage(frame));
#if FUSE_GALLERY_HAVE_GAME
    if (const NodeHandle gp = ui.find("page.game"); ui.alive(gp)) {
        ui.setText(ui.find("g.game.note"), "Game HUD widgets (fuse_ui_game)");
        buildGamePage(ui, gp);
    }
#endif

    // the Edit menu: actions with shortcuts (the menu shows the shortcut texts; the keys work anywhere in the window)
    fuse::ui::ActionRegistry actions(ui);
    const auto say = [&ui](std::string_view what) { w::statusMessage(ui, ui.find("g.statusbar"), what, 4.f); };
    const fuse::ui::ActionHandle undo = actions.add({.id = "edit.undo", .text = "Undo", .shortcut = fuse::ui::primaryShortcut(fuse::ui::Key::Z)},
                                                    [say](UiContext&, fuse::ui::ActionHandle, bool) { say("action: Undo"); });
    const fuse::ui::ActionHandle redo =
        actions.add({.id = "edit.redo", .text = "Redo", .shortcut = fuse::ui::primaryShortcut(fuse::ui::Key::Z, fuse::ui::kModShift), .enabled = false});
    const fuse::ui::ActionHandle grid =
        actions.add({.id = "view.grid", .text = "Snap to grid", .shortcut = fuse::ui::primaryShortcut(fuse::ui::Key::G), .checkable = true, .checked = true},
                    [say](UiContext&, fuse::ui::ActionHandle, bool on) { say(on ? "action: snap on" : "action: snap off"); });
    const fuse::ui::ActionHandle prefs = actions.add({.id = "edit.preferences", .text = "Preferences...", .shortcut = {fuse::ui::Key::F2, fuse::ui::kModAlt}},
                                                     [say](UiContext&, fuse::ui::ActionHandle, bool) { say("action: Preferences"); });
    const fuse::ui::u32 editList = actions.addList({undo, redo, fuse::ui::ActionHandle{}, grid, prefs});
    if (const NodeHandle bar = ui.find("menubar"); ui.alive(bar)) w::actionMenuBarMenu(ui, actions, bar, "Edit", editList, "menu.edit");
    // model / view: the table takes an extended selection (two rows to start with), the tree table has a current row
    if (const NodeHandle table = ui.find("g.tableview"); ui.alive(table)) {
        w::setViewSelectionMode(ui, table, w::SelectionMode::Extended);
        if (w::SelectionModel* sel = w::viewSelectionModel(ui, table)) {
            sel->selectRows(1, 2);
            sel->setCurrent(2);
        }
    }
    if (const NodeHandle tree = ui.find("g.treemodelview"); ui.alive(tree)) w::setViewCurrent(ui, tree, 2);

    window.document()->onAction([&](std::string_view action, NodeHandle node, const SignalArgs& a) {
        if (action == "count") model.set("clicks", model.number("clicks") + 1.0);
        else if (action == "toast") w::toast(ui, "A toast: it fades out on its own", 2.5f);
        else if (action == "messagebox") w::messageBox(ui, "Message box", "A modal message with buttons.", "OK\nCancel", w::MessageIcon::Info);
        else if (action == "dialog.open") model.set("dialogOpen", 1.0);
        else if (action == "dialog.close") model.set("dialogOpen", 0.0);
        else if (action == "popover") w::label(ui, w::popover(ui, node), "A popover anchored to its button");
        else if (action == "filedialog") w::fileDialog(ui, "Open a file", ".", "*.fuseui;*.png");
        else if (action == "wizard") {
            const NodeHandle wiz = w::wizard(ui, "Wizard");
            w::label(ui, w::wizardAddPage(ui, wiz, "Welcome"), "Page one of two");
            w::label(ui, w::wizardAddPage(ui, wiz, "Done"), "Finish closes the wizard");
            w::openWizard(ui, wiz);
        } else if (action == "menu" || action == "context" || action == "link") {
            const std::string msg = std::string(action) + ": " + std::string(a.text);
            w::statusMessage(ui, ui.find("g.statusbar"), msg, 4.f);
            w::toast(ui, msg, 2.f);
        }
    });

    if (const std::string_view shot = app.args().get("screenshot"); !shot.empty()) {
        app.step(0.f);
        app.step(0.f);
        // the page asked for is the one shown, and its widgets were laid out
        const int p = int(model.number("page"));
        const NodeHandle shown = ui.find(kPageIds[p]), probe = ui.find(kPageProbe[p]);
        const fuse::ui::Rect pr = ui.rect(probe);
        if (!shownOnScreen(ui, shown) || !shownOnScreen(ui, probe) || pr.w <= 0.f || pr.h <= 0.f || pr.y >= 760.f) {
            std::fprintf(stderr, "widget_gallery: page %d is not shown (probe %s: %g x %g at %g, %g)\n", p, kPageProbe[p], double(pr.w),
                         double(pr.h), double(pr.x), double(pr.y));
            return 1;
        }
        for (int i = 0; i < 7; ++i)
            if (i != p && shownOnScreen(ui, ui.find(kPageIds[i]))) return app.fail("another page is shown too");
        if (!window.writePng(std::string(shot))) return app.fail("cannot write screenshot");
        std::printf("widget_gallery: page %d, %u nodes, %zu draw commands, wrote %.*s\n", p, ui.nodeCount(), ui.drawList().size(),
                    int(shot.size()), shot.data());
        return 0;
    }
    fuse::app::NativePlatform native(app);
    native.show(window);
    return app.run();
}
