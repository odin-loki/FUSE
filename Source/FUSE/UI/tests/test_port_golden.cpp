// fuse_ui_port_golden: golden images of the widgets and options ported from the merged branches
// (docs/ui/archive/PORT_*.md): line edit / spin box / editable combo / tab bar options, the extras widgets, dock splits.
// One scene per ctest gate (`--scene=<name>`, ctest fuse_ui_port_golden_<name>) against tests/golden/port_<name>.png
// (MINIMAL text profile; FULL would read port_<name>_full.png, which was not rendered), with the tolerance of
// fuse_ui_widget_golden. FUSE_UI_UPDATE_GOLDEN=1 rewrites the PNG, `--out=<dir>` also writes the rendered scenes
// there. Run from the tests directory.
#include "test_util.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_write_impl.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
u8 ch(u32 p, int c) { return u8((p >> (8 * c)) & 0xFF); }
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
/// Lets style transitions (focus ring, popup fade-in) run out: half a second in 60 Hz steps.
void settle(UiContext& ui) {
    for (int i = 0; i < 30; ++i) ui.update(1.f / 60.f);
    ui.update(0.f);
}
NodeHandle row(UiContext& ui, NodeHandle parent) {
    const NodeHandle r = w::panel(ui, parent);
    ui.addClass(r, "row");
    return r;
}

// ---- line edit, spin box, editable combo and tab bar options ---------------------------------------------------------
void sceneEditOptions(Image& img) {
    UiContext ui({.viewport = {640, 420}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "12px");
    ui.setCss(ui.root(), "gap", "10px");
    NodeHandle r = row(ui, ui.root());
    const NodeHandle empty = w::lineEdit(ui, r, "", "empty");
    w::setLineEditOptions(ui, empty, {.placeholder = "Placeholder"});
    const NodeHandle pw = w::lineEdit(ui, r, "", "pw");
    w::setLineEditOptions(ui, pw, {.echo = w::EchoMode::Password});
    w::setLineEditText(ui, pw, "hunter2");
    const NodeHandle mask = w::lineEdit(ui, r, "", "mask");
    w::setLineEditOptions(ui, mask, {.inputMask = "000.000.000.000;_"});
    w::setLineEditText(ui, mask, "192168");
    r = row(ui, ui.root());
    const NodeHandle price = w::spinBox(ui, r, 0.f, 500.f, 20.f, 1.f, "price");
    w::setSpinBoxOptions(ui, price, {.prefix = "$ ", .suffix = " ea"});
    const NodeHandle timeout = w::spinBox(ui, r, 0.f, 120.f, 0.f, 5.f, "timeout");
    w::setSpinBoxOptions(ui, timeout, {.suffix = " min", .specialValueText = "Never"});
    const NodeHandle angle = w::doubleSpinBox(ui, r, 0.f, 360.f, 360.f, 0.5f, "angle");
    w::setSpinBoxOptions(ui, angle, {.suffix = " deg", .decimals = 2});
    const NodeHandle typed = w::spinBox(ui, r, 0.f, 100.f, 42.f, 1.f, "typed");
    w::setSpinBoxOptions(ui, typed, {});
    // tab bars: closable + movable with a disabled tab; an overflowing one with scroll buttons; one shrunk to fit
    const NodeHandle tabs = w::tabBar(ui, ui.root(), "main.cpp\nwidgets.hpp\nREADME.md\ntheme.css", 1, "tabs");
    w::setTabsClosable(ui, tabs, true);
    w::setTabsMovable(ui, tabs, true);
    w::setTabEnabled(ui, tabs, 2, false);
    std::string many;
    for (int i = 0; i < 12; ++i) many += (i ? "\nDocument " : "Document ") + std::to_string(i);
    const NodeHandle scroll = w::tabBar(ui, ui.root(), many, 8, "scroll");
    w::setTabsClosable(ui, scroll, true);
    const NodeHandle fit = w::tabBar(ui, ui.root(), "Overview\nPerformance\nMemory\nNetwork\nStorage\nSecurity\nAccessibility\nDiagnostics\nUpdates", 3, "fit");
    w::setTabUsesScrollButtons(ui, fit, false);
    // an editable combo with its list open, and a completer popup under a field
    r = row(ui, ui.root());
    const NodeHandle combo = w::editableComboBox(ui, r, "Red\nGreen\nBlue\nIndigo", 1, w::InsertPolicy::InsertAtBottom, "combo");
    ui.setCss(combo, "width", "180px");
    const NodeHandle fruit = w::lineEdit(ui, r, "", "fruit");
    w::setCompleter(ui, fruit, {.items = "Apple\nApricot\nAvocado\nBanana\nBlueberry"});
    ui.update();
    // the typed spin box holds the focus: its value is selected
    ui.setFocus(typed, FocusReason::Keyboard);
    ui.update();
    ui.setFocus(fruit, FocusReason::Keyboard);
    ui.textInput("a");
    ui.keyDown(Key::Down); // the first completion is highlighted (and shown in the field)
    ui.update();
    UI_CHECK(w::completerPopupVisible(ui, fruit) && w::completionCount(ui, fruit) == 3);
    settle(ui);
    // the current tab of the overflowing bar was scrolled into view
    UI_CHECK(w::spinBoxText(ui, price) == "$ 20 ea" && w::spinBoxText(ui, timeout) == "Never" && w::tabBarScroll(ui, scroll) > 0.f);
    render(ui, img);
}

// ---- the extras widgets ------------------------------------------------------------------------------------------------
void sceneExtra(Image& img) {
    UiContext ui({.viewport = {900, 640}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "10px");
    ui.setCss(ui.root(), "flex-direction", "row");
    ui.setCss(ui.root(), "gap", "12px");
    const NodeHandle left = w::panel(ui, ui.root());
    ui.setCss(left, "width", "340px");
    ui.setCss(left, "gap", "8px");
    const NodeHandle lcd = w::lcdNumber(ui, left, 6);
    w::setLcdText(ui, lcd, "12:34.5");
    const NodeHandle hex = w::lcdNumber(ui, left, 4);
    w::setLcdBase(ui, hex, 16);
    ui.setValue(hex, 48879.f);
    w::keySequenceEdit(ui, left, Shortcut{Key::S, kModCtrl | kModShift});
    w::keySequenceEdit(ui, left);
    const NodeHandle tb = w::toolBox(ui, left, "General\nAdvanced\nAbout");
    w::label(ui, w::toolBoxPage(ui, tb, 1), "the open page");
    w::setToolBoxIndex(ui, tb, 1);
    w::dialogButtonBox(ui, left, w::StdButton::Ok | w::StdButton::Cancel | w::StdButton::Help, {.layout = w::ButtonBoxLayout::Windows});
    w::dialogButtonBox(ui, left, w::StdButton::Save | w::StdButton::Discard | w::StdButton::Cancel, {.layout = w::ButtonBoxLayout::Mac});
    const NodeHandle kb = w::virtualKeyboard(ui, left);
    const NodeHandle right = w::panel(ui, ui.root());
    ui.setCss(right, "flex-grow", "1");
    ui.setCss(right, "gap", "8px");
    const NodeHandle area = w::mdiArea(ui, right);
    ui.setCss(area, "height", "260px");
    const NodeHandle a = w::mdiSubWindow(ui, area, "Document 1", {16, 14, 240, 150});
    w::label(ui, w::mdiBody(ui, a), "an MDI sub-window");
    const NodeHandle b = w::mdiSubWindow(ui, area, "Document 2", {150, 70, 260, 160});
    w::label(ui, w::mdiBody(ui, b), "the active one");
    static w::GraphicsScene scene;
    scene = {};
    scene.add({.rect = {20, 20, 90, 60}, .text = "box"});
    scene.add({.shape = w::GraphicsShape::Ellipse, .rect = {150, 30, 70, 70}, .fill = Color::rgb(0x81C784)});
    scene.add({.shape = w::GraphicsShape::Ellipse, .rect = {240, 40, 110, 50}, .fill = Color::rgb(0xFFB454)});
    scene.add({.shape = w::GraphicsShape::Line, .rect = {40, 130, 200, 40}, .strokeWidth = 2.f});
    scene.add({.shape = w::GraphicsShape::Polygon, .points = {{380, 30}, {460, 60}, {410, 140}}});
    scene.add({.shape = w::GraphicsShape::Text, .rect = {260, 150, 200, 24}, .fill = Color::rgb(0xFFFFFF), .text = "graphics view"});
    scene.select(0, true);
    const NodeHandle view = w::graphicsView(ui, right, &scene);
    ui.setCss(view, "flex-grow", "1");
    ui.update();
    // shift is latched on the keyboard; a rubber band is being dragged in the view
    const NodeHandle shift = w::virtualKeyboardKey(ui, kb, "Shift");
    ui.pointerMove(ctr(ui.rect(shift)));
    ui.pointerDown(ctr(ui.rect(shift)));
    ui.pointerUp(ctr(ui.rect(shift)));
    const Vec2 p0 = w::sceneToView(ui, view, {130, 110}), p1 = w::sceneToView(ui, view, {370, 190});
    ui.pointerMove(p0);
    ui.pointerDown(p0);
    ui.pointerMove(p1);
    ui.update();
    Rect band;
    UI_CHECK(w::viewRubberBand(ui, view, band) && ui.text(w::virtualKeyboardKey(ui, kb, "Q")) == "Q");
    settle(ui);
    render(ui, img);
    ui.pointerUp(p1);
    w::setGraphicsScene(ui, view, nullptr); // (the static scene outlives the context)
}

// ---- dock splits ---------------------------------------------------------------------------------------------------------
void sceneDockSplit(Image& img) {
    UiContext ui({.viewport = {900, 520}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "0px");
    const NodeHandle area = w::dockArea(ui, ui.root(), "area");
    const char* names[] = {"Outline", "Assets", "History", "Log", "Console", "Problems", "Properties", "Materials"};
    const w::DockSide sides[] = {w::DockSide::Left, w::DockSide::Left, w::DockSide::Left, w::DockSide::Bottom,
                                 w::DockSide::Bottom, w::DockSide::Bottom, w::DockSide::Right, w::DockSide::Right};
    NodeHandle d[8];
    for (int i = 0; i < 8; ++i) {
        d[i] = w::dockWidget(ui, area, names[i], sides[i], std::string("dock.") + names[i]);
        w::label(ui, w::dockBody(ui, d[i]), names[i]);
    }
    w::label(ui, w::dockRegion(ui, area, w::DockSide::Center), "centre");
    ui.update();
    // left: outline above [assets | history-as-a-tab]; bottom: log | [console over problems]; right: a column split
    UI_CHECK(w::dockSplit(ui, d[1], d[0], w::DockEdge::Bottom));
    UI_CHECK(w::dockSplit(ui, d[2], d[1], w::DockEdge::Center));
    UI_CHECK(w::dockSplit(ui, d[4], d[3], w::DockEdge::Right, 320.f));
    UI_CHECK(w::dockSplit(ui, d[5], d[4], w::DockEdge::Bottom));
    UI_CHECK(w::dockSplit(ui, d[7], d[6], w::DockEdge::Top, 120.f));
    w::dockRaise(ui, d[1]);
    ui.update();
    UI_CHECK(w::dockSaveLayout(ui, area).rfind("fusedock 2\n", 0) == 0);
    // the handle between console and problems has the keyboard focus (drawn lit)
    ui.setFocus(ui.nextSibling(ui.parent(d[4])), FocusReason::Keyboard);
    settle(ui);
    render(ui, img);
}

struct Scene { const char* name; void (*fn)(Image&); };
const Scene kScenes[] = {{"edit_options", &sceneEditOptions}, {"extra", &sceneExtra}, {"dock_split", &sceneDockSplit}};

void compare(const Scene& s, const char* outDir) {
#if FUSE_UI_TEST_TEXT_FULL
    const std::string golden = std::string("golden/port_") + s.name + "_full.png"; // FreeType coverage differs from stb_truetype
#else
    const std::string golden = std::string("golden/port_") + s.name + ".png";
#endif
    Image img;
    s.fn(img);
    if (outDir) stbi_write_png((std::string(outDir) + "/port_" + s.name + ".png").c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
    if (std::getenv("FUSE_UI_UPDATE_GOLDEN")) {
        stbi_write_png(golden.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("golden written: %s\n", golden.c_str());
        return;
    }
    int gw = 0, gh = 0, n = 0;
    unsigned char* ref = stbi_load(golden.c_str(), &gw, &gh, &n, 4);
    UI_CHECK(ref != nullptr);
    if (!ref) {
        std::fprintf(stderr, "missing golden %s (run from the tests directory; FUSE_UI_UPDATE_GOLDEN=1 writes it)\n", golden.c_str());
        return;
    }
    UI_CHECK_EQ(gw, img.width);
    UI_CHECK_EQ(gh, img.height);
    if (gw == img.width && gh == img.height) {
        int bad = 0, maxd = 0;
        for (int i = 0; i < gw * gh; ++i)
            for (int c = 0; c < 4; ++c) {
                const int d = std::abs(int(ref[i * 4 + c]) - int(ch(img.pixels[std::size_t(i)], c)));
                maxd = std::max(maxd, d);
                if (d > 3) { ++bad; break; }
            }
        std::printf("%s golden diff: %d pixels > 3, max channel diff %d\n", s.name, bad, maxd);
        UI_CHECK(bad <= gw * gh / 1000); // <= 0.1% of pixels may differ (compiler / libm rounding)
    }
    stbi_image_free(ref);
}
} // namespace

int main(int argc, char** argv) {
    const char* only = nullptr;
    const char* outDir = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--scene=", 8) == 0) only = argv[i] + 8;
        else if (std::strncmp(argv[i], "--out=", 6) == 0) outDir = argv[i] + 6;
    }
    bool ran = false;
    for (const Scene& s : kScenes)
        if (!only || std::strcmp(only, s.name) == 0) {
            compare(s, outDir);
            ran = true;
        }
    if (!ran) {
        std::fprintf(stderr, "unknown scene %s\n", only);
        return 2;
    }
    return finish("fuse_ui_port_golden");
}
