// fuse_ui_interaction_golden: golden image of the interaction visuals against tests/golden/interaction.png (MINIMAL
// text profile only; FUSE_UI_UPDATE_GOLDEN=1 rewrites it, `--out=<dir>` also writes the render there). Two cells:
//   left  - a menu-bar menu built from an action list: shortcut texts right-aligned, a checked and a disabled item
//   right - a row dragged from one list view to another with Ctrl held: the ghost chip with its copy mark and the
//           insertion line in the target list
// Same tolerance as fuse_ui_widget_golden. Deterministic: fixed viewports, no clocks.
#include "test_util.hpp"

#include "fuse/ui/actions.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdlib>
#include <cstring>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
u8 ch(u32 p, int c) { return u8((p >> (8 * c)) & 0xFF); }
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
void blit(Image& sheet, const Image& cell, int x, int y) {
    for (int row = 0; row < cell.height; ++row)
        std::memcpy(&sheet.pixels[std::size_t(y + row) * std::size_t(sheet.width) + std::size_t(x)],
                    &cell.pixels[std::size_t(row) * std::size_t(cell.width)], std::size_t(cell.width) * sizeof(u32));
}

void cellMenu(Image& img) {
    UiContext ui({.viewport = {300, 240}, .font = &font()});
    w::install(ui);
    setPrimaryModifier(kModCtrl);
    ActionRegistry reg(ui);
    const ActionHandle aNew = reg.add({.id = "new", .text = "New scene", .shortcut = primaryShortcut(Key::N)});
    const ActionHandle aOpen = reg.add({.id = "open", .text = "Open...", .shortcut = primaryShortcut(Key::O)});
    const ActionHandle aSave = reg.add({.id = "save", .text = "Save", .shortcut = primaryShortcut(Key::S)});
    const ActionHandle aSaveAs = reg.add({.id = "saveas", .text = "Save as...", .shortcut = primaryShortcut(Key::S, kModShift), .enabled = false});
    const ActionHandle aGrid = reg.add({.id = "grid", .text = "Show grid", .shortcut = {Key::G, 0}, .checkable = true, .checked = true});
    const ActionHandle aRun = reg.add({.id = "run", .text = "Run", .shortcut = {Key::F5, 0}});
    const NodeHandle bar = w::menuBar(ui, ui.root(), "bar");
    w::actionMenuBarMenu(ui, reg, bar, "File", reg.addList({aNew, aOpen, aSave, aSaveAs, ActionHandle{}, aGrid, aRun}), "file");
    w::menuBarMenu(ui, bar, "Edit", "Undo\nRedo", "edit");
    ui.update(0.f);
    UiTestDriver drv(ui);
    drv.click("file");
    ui.keyDown(Key::Down); // focus on the second item
    ui.pointerMove({280.f, 230.f});
    for (int i = 0; i < 4; ++i) ui.update(1.f / 60.f);
    render(ui, img);
    setPrimaryModifier(0);
}

void cellDrag(Image& img) {
    UiContext ui({.viewport = {420, 240}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "flex-direction", "row");
    ui.setCss(ui.root(), "gap", "16px");
    ui.setCss(ui.root(), "padding", "16px");
    ui.setCss(ui.root(), "align-items", "start");
    const NodeHandle la = w::listView(ui, ui.root(), "Albedo\nNormal\nRoughness\nEmissive", 1, "la");
    const NodeHandle lb = w::listView(ui, ui.root(), "Layer 1\nLayer 2\nLayer 3", -1, "lb");
    ui.setCss(lb, "height", "150px");
    w::setListDragDrop(ui, la, w::kViewDragDrop);
    w::setListDragDrop(ui, lb, w::kViewDragDrop);
    ui.update(0.f);
    const Vec2 from = ctr(w::listRowRect(ui, la, 2));
    const Rect r1 = w::listRowRect(ui, lb, 1);
    const Vec2 to{r1.x + 30.f, r1.y + r1.h * 0.8f};
    ui.setModifiers(kModCtrl);
    ui.pointerMove(from);
    ui.pointerDown(from);
    ui.pointerMove({from.x + 20.f, from.y});
    ui.pointerMove(to);
    for (int i = 0; i < 4; ++i) ui.update(1.f / 60.f);
    UI_CHECK(ui.dragging() && ui.dragTarget() == lb && ui.dropAction() == DropAction::Copy);
    render(ui, img);
    ui.cancelDrag();
}

void scene(Image& img) {
    Image a, b;
    cellMenu(a);
    cellDrag(b);
    img.resize(a.width + b.width, std::max(a.height, b.height), 0xFF000000u);
    blit(img, a, 0, 0);
    blit(img, b, a.width, 0);
}
} // namespace

int main(int argc, char** argv) {
    const char* outDir = nullptr;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--out=", 6) == 0) outDir = argv[i] + 6;
    const std::string golden = "golden/interaction.png";
    Image img;
    scene(img);
    if (outDir) stbi_write_png((std::string(outDir) + "/interaction.png").c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
    if (std::getenv("FUSE_UI_UPDATE_GOLDEN")) {
        stbi_write_png(golden.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("golden written: %s\n", golden.c_str());
        return finish("fuse_ui_interaction_golden");
    }
    int gw = 0, gh = 0, n = 0;
    unsigned char* ref = stbi_load(golden.c_str(), &gw, &gh, &n, 4);
    UI_CHECK(ref != nullptr);
    if (!ref) {
        std::fprintf(stderr, "missing golden %s (run from the tests directory; FUSE_UI_UPDATE_GOLDEN=1 writes it)\n", golden.c_str());
        return finish("fuse_ui_interaction_golden");
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
        std::printf("interaction golden diff: %d pixels > 3, max channel diff %d\n", bad, maxd);
        UI_CHECK(bad <= gw * gh / 1000); // <= 0.1% of pixels may differ (compiler / libm rounding)
    }
    stbi_image_free(ref);
    return finish("fuse_ui_interaction_golden");
}
