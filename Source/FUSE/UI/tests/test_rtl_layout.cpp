// Gate fuse_ui_rtl_layout: `direction: rtl` mirrors layout and widgets. Behaviour checks (box rows / justify / align /
// grid / flow mirrored with physical margins, inheritance, text-align start = right, slider pointer + keys, tab bar,
// tree view, scroll container bar side + horizontal start, menus open right-aligned and submenus to the left), a
// pixel check (text-free widgets in RTL render as the mirror image of LTR) and a CPU-raster golden of the RTL sample
// tree (tests/golden/rtl_layout.png; regenerate with FUSE_UI_UPDATE_GOLDEN=1).
#include "test_util.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdlib>

using namespace fuse::ui;
using namespace fuse::ui::test;
using namespace fuse::ui::widgets;

namespace {

u8 ch(u32 p, int c) { return u8((p >> (8 * c)) & 0xFF); }
f32 mirrored(const Rect& r, const Rect& box) { return 2.f * box.x + box.w - r.x - r.w; }

// A 300 px wide row of three buttons of different widths inside a padded container.
struct Row3 {
    NodeHandle row, a, b, c;
};
Row3 buildRow(UiContext& ui, bool rtl) {
    widgets::install(ui);
    if (rtl) ui.setCss(ui.root(), "direction", "rtl");
    const NodeHandle row = panel(ui, ui.root(), "row");
    ui.addClass(row, "row");
    ui.setCss(row, "width", "300px");
    ui.setCss(row, "padding", "10px");
    return {row, button(ui, row, "One", "a"), button(ui, row, "Second", "b"), button(ui, row, "3", "c")};
}

void boxRow() {
    UiContext l({.viewport = {400, 120}, .font = &font()}), r({.viewport = {400, 120}, .font = &font()});
    const Row3 L = buildRow(l, false), R = buildRow(r, true);
    l.update();
    r.update();
    UI_CHECK(r.computed(R.a).rtl);  // inherited from the root
    UI_CHECK(!l.computed(L.a).rtl);
    const Rect box = r.contentRect(R.row), lbox = l.contentRect(L.row);
    const Rect lrow = l.rect(L.row);
    UI_CHECK_RECT(r.rect(R.row), mirrored(lrow, {0.f, 0.f, 400.f, 120.f}), lrow.y, lrow.w, lrow.h); // the row itself too
    for (auto [ln, rn] : {std::pair{L.a, R.a}, std::pair{L.b, R.b}, std::pair{L.c, R.c}}) {
        const Rect lr = l.rect(ln), rr = r.rect(rn);
        UI_CHECK_RECT(rr, mirrored(lr, lbox) + (box.x - lbox.x), lr.y, lr.w, lr.h);
    }
    UI_CHECK(r.rect(R.a).x > r.rect(R.b).x && r.rect(R.b).x > r.rect(R.c).x); // first child on the right
    UI_CHECK_EQ(r.rect(R.a).right(), box.right());

    // justify-content: start / end follow the inline direction
    r.setCss(R.row, "justify-content", "end");
    r.update();
    UI_CHECK_EQ(r.rect(R.c).x, box.x);
    r.setCss(R.row, "justify-content", "start");
    // margins stay physical: margin-left of the middle button is the gap to its left neighbour (c in RTL)
    r.setCss(R.b, "margin-left", "20px");
    r.update();
    UI_CHECK_EQ(r.rect(R.b).x - r.rect(R.c).right(), 8.f + 20.f);
    UI_CHECK_EQ(r.rect(R.a).x - r.rect(R.b).right(), 8.f);
    r.setCss(R.b, "margin-left", "0px");
    // column: align-items start = the right edge, `direction: ltr` on a child subtree switches back
    r.setCss(R.row, "flex-direction", "column");
    r.setCss(R.row, "align-items", "start");
    r.update();
    UI_CHECK_EQ(r.rect(R.a).right(), box.right());
    UI_CHECK_EQ(r.rect(R.c).right(), box.right());
    r.setCss(R.row, "direction", "ltr");
    r.update();
    UI_CHECK(!r.computed(R.a).rtl);
    UI_CHECK_EQ(r.rect(R.a).x, box.x);
}

void gridAndFlow() {
    UiContext ui({.viewport = {400, 300}, .font = &font()});
    widgets::install(ui);
    const NodeHandle g = panel(ui, ui.root(), "grid");
    ui.setCss(g, "direction", "rtl");
    ui.setCss(g, "layout", "grid");
    ui.setCss(g, "grid-template-columns", "100px 50px 1fr");
    ui.setCss(g, "width", "300px");
    NodeHandle cell[3];
    for (int k = 0; k < 3; ++k) cell[k] = button(ui, g, "x");
    const NodeHandle f = panel(ui, ui.root(), "flow");
    ui.setCss(f, "direction", "rtl");
    ui.setCss(f, "layout", "flow");
    ui.setCss(f, "width", "200px");
    ui.setCss(f, "gap", "4px");
    NodeHandle chip[4];
    for (int k = 0; k < 4; ++k) {
        chip[k] = panel(ui, f);
        ui.setCss(chip[k], "width", "60px");
        ui.setCss(chip[k], "height", "10px");
    }
    ui.update();
    const Rect gc = ui.contentRect(g);
    UI_CHECK_EQ(ui.rect(cell[0]).right(), gc.right()); // column 1 is the rightmost (100 px)
    UI_CHECK_EQ(ui.rect(cell[0]).w, 100.f);
    UI_CHECK_EQ(ui.rect(cell[1]).right(), gc.right() - 100.f);
    UI_CHECK_EQ(ui.rect(cell[2]).x, gc.x);
    const Rect fc = ui.contentRect(f);
    UI_CHECK_EQ(ui.rect(chip[0]).right(), fc.right()); // lines fill from the right
    UI_CHECK_EQ(ui.rect(chip[1]).right(), fc.right() - 64.f);
    UI_CHECK_EQ(ui.rect(chip[3]).right(), fc.right()); // second line starts at the right again
    UI_CHECK(ui.rect(chip[3]).y > ui.rect(chip[0]).y);
}

void textAlign() {
    UiContext ui({.viewport = {400, 200}, .font = &font()});
    widgets::install(ui);
    ui.setCss(ui.root(), "direction", "rtl");
    auto make = [&](const char* align) {
        const NodeHandle n = label(ui, ui.root(), "Hi");
        ui.setCss(n, "width", "200px");
        if (align) ui.setCss(n, "text-align", align);
        return n;
    };
    const NodeHandle start = make(nullptr), end = make("end"), left = make("left"), right = make("right");
    ui.update();
    auto firstX = [&](NodeHandle n) {
        const auto* tl = ui.textLayout(n);
        return tl && !tl->glyphs.empty() ? tl->glyphs[0].x : -1.f;
    };
    UI_CHECK(firstX(start) > 150.f); // start = right in RTL
    UI_CHECK(firstX(right) > 150.f);
    UI_CHECK(firstX(end) >= 0.f && firstX(end) < 10.f);
    UI_CHECK(firstX(left) >= 0.f && firstX(left) < 10.f);
    ui.setCss(ui.root(), "direction", "ltr");
    ui.update();
    UI_CHECK(firstX(start) >= 0.f && firstX(start) < 10.f);
    UI_CHECK(firstX(right) > 150.f);
}

void sliderAndTabs() {
    UiContext ui({.viewport = {400, 200}, .font = &font()});
    widgets::install(ui);
    ui.setCss(ui.root(), "direction", "rtl");
    const NodeHandle s = slider(ui, ui.root(), 0.f, 10.f, 5.f, 1.f, "s");
    ui.setCss(s, "width", "200px");
    const NodeHandle tabs = tabBar(ui, ui.root(), "One\nTwo\nThree", 0, "tabs");
    ui.setCss(tabs, "align-self", "start"); // sized to its tabs, at the start (right) edge
    ui.update();
    UiTestDriver d(ui);
    ui.setFocus(s, FocusReason::Keyboard);
    d.key(Key::Left);
    UI_CHECK_EQ(ui.value(s), 6.f); // Left increases (moves the knob left, towards the maximum)
    d.key(Key::Right);
    d.key(Key::Right);
    UI_CHECK_EQ(ui.value(s), 4.f);
    const Rect c = ui.contentRect(s);
    d.clickAt({c.right() - 8.f, c.y + c.h * 0.5f}); // the right end is the minimum
    UI_CHECK(ui.value(s) <= 0.5f);
    d.clickAt({c.x + 8.f, c.y + c.h * 0.5f});
    UI_CHECK(ui.value(s) >= 9.5f);
    // tab bar: the first tab on the right, Left selects the next one
    const Rect tr = ui.contentRect(tabs);
    d.clickAt({tr.x + 4.f, tr.y + tr.h * 0.5f});
    UI_CHECK_EQ(ui.value(tabs), 2.f);
    d.clickAt({tr.right() - 4.f, tr.y + tr.h * 0.5f});
    UI_CHECK_EQ(ui.value(tabs), 0.f);
    ui.setFocus(tabs, FocusReason::Keyboard);
    d.key(Key::Left);
    UI_CHECK_EQ(ui.value(tabs), 1.f);
}

void treeKeys() {
    UiContext ui({.viewport = {400, 300}, .font = &font()});
    widgets::install(ui);
    ui.setCss(ui.root(), "direction", "rtl");
    const NodeHandle t = treeView(ui, ui.root(), "Root\n\tChild\n\tOther", "tree");
    setTreeExpanded(ui, t, 0, false);
    ui.update();
    ui.setFocus(t, FocusReason::Keyboard);
    UiTestDriver d(ui);
    d.key(Key::Down); // selects Root
    UI_CHECK_EQ(ui.value(t), 0.f);
    d.key(Key::Left); // expands in RTL
    UI_CHECK(treeExpanded(ui, t, 0));
    d.key(Key::Right);
    UI_CHECK(!treeExpanded(ui, t, 0));
    // the expander sits at the right edge
    const Rect c = ui.contentRect(t);
    d.clickAt({c.right() - 9.f, c.y + 6.f});
    UI_CHECK(treeExpanded(ui, t, 0));
}

void scrollContainer() {
    UiContext ui({.viewport = {400, 300}, .font = &font()});
    widgets::install(ui);
    ui.setCss(ui.root(), "direction", "rtl");
    const NodeHandle sa = scrollArea(ui, ui.root(), "sa");
    ui.setCss(sa, "width", "200px");
    ui.setCss(sa, "height", "100px");
    ui.setCss(sa, "flex-direction", "row");
    const NodeHandle wide = panel(ui, sa, "wide");
    ui.setCss(wide, "width", "500px");
    ui.setCss(wide, "height", "400px");
    ui.setCss(wide, "flex-shrink", "0");
    ui.update();
    const Rect r = ui.rect(sa), c = ui.contentRect(sa);
    UI_CHECK_EQ(ui.rect(wide).right(), c.right()); // horizontal overflow goes to the left
    // the vertical bar is on the left: a press there scrolls (jump to the pointer)
    ui.pointerMove({r.x + 4.f, r.bottom() - 20.f});
    ui.pointerDown({r.x + 4.f, r.bottom() - 20.f});
    ui.pointerUp({r.x + 4.f, r.bottom() - 20.f});
    ui.update();
    UI_CHECK(ui.scroll(sa).y > 100.f);
    // horizontal: scroll.x counts from the right edge; the content moves right as it grows
    ui.scrollTo(sa, {50.f, 0.f});
    ui.update();
    UI_CHECK_EQ(ui.rect(wide).right(), c.right() + 50.f);
    // a horizontal bar press near the left end scrolls to the far (left) end of the content
    ui.pointerDown({r.x + 20.f, r.bottom() - 4.f});
    ui.pointerUp({r.x + 20.f, r.bottom() - 4.f});
    ui.update();
    UI_CHECK(ui.scroll(sa).x > 250.f);
}

void menus() {
    UiContext ui({.viewport = {600, 400}, .font = &font()});
    widgets::install(ui);
    ui.setCss(ui.root(), "direction", "rtl");
    const NodeHandle bar = menuBar(ui, ui.root(), "bar");
    const NodeHandle file = menuBarMenu(ui, bar, "File", "Open\nRecent\n\tA.txt\n\tB.txt\nQuit", "file");
    const NodeHandle edit = menuBarMenu(ui, bar, "Edit", "Undo", "edit");
    ui.update();
    UI_CHECK(ui.rect(file).x > ui.rect(edit).x); // menu bar order: the first title on the right
    UI_CHECK_EQ(ui.rect(file).right(), ui.contentRect(bar).right());
    UiTestDriver d(ui);
    d.click("file");
    ui.update();
    const NodeHandle menu = ui.parent(ui.focused());
    UI_CHECK(ui.alive(menu));
    UI_CHECK(ui.computed(menu).rtl);
    UI_CHECK_EQ(ui.rect(menu).right(), ui.rect(file).right()); // right-aligned to its button
    d.key(Key::Down); // Recent
    UI_CHECK_EQ(ui.text(ui.focused()).substr(0, 6), std::string_view("Recent"));
    d.key(Key::Left); // opens the submenu (to the left)
    ui.update();
    const NodeHandle sub = ui.parent(ui.focused());
    UI_CHECK(ui.alive(sub) && sub != menu);
    UI_CHECK(ui.rect(sub).right() <= ui.rect(menu).x + 4.f);
    UI_CHECK(ui.rect(sub).x < ui.rect(menu).x);
    d.key(Key::Right); // back to the parent menu
    ui.update();
    UI_CHECK(ui.parent(ui.focused()) == menu);
}

// Text-free widgets: the RTL frame is the LTR frame mirrored.
void buildPlain(UiContext& ui, bool rtl) {
    widgets::install(ui);
    if (rtl) ui.setCss(ui.root(), "direction", "rtl");
    ui.setCss(ui.root(), "padding", "12px 20px 12px 20px");
    ui.setCss(ui.root(), "gap", "8px");
    const NodeHandle r1 = panel(ui, ui.root());
    ui.addClass(r1, "row");
    checkBox(ui, r1, "", false);
    radioButton(ui, r1, "", true);
    toggleSwitch(ui, r1, "", true);
    toggleSwitch(ui, r1, "", false);
    const NodeHandle b = button(ui, r1, "");
    ui.setCss(b, "width", "60px");
    ui.setCss(b, "height", "20px");
    const NodeHandle s = slider(ui, ui.root(), 0.f, 1.f, 0.3f, 0.f);
    ui.setCss(s, "width", "180px");
    progressBar(ui, ui.root(), 0.35f);
    const NodeHandle sb = scrollBar(ui, ui.root(), false);
    ui.setCss(sb, "width", "150px");
    ui.setValue(sb, 0.25f);
    const NodeHandle sa = scrollArea(ui, ui.root());
    ui.setCss(sa, "height", "40px");
    ui.setCss(sa, "width", "120px");
    const NodeHandle tall = panel(ui, sa);
    ui.setCss(tall, "height", "200px");
    ui.update();
}

void mirrorPixels() {
    UiContext l({.viewport = {260, 200}, .font = &font()}), r({.viewport = {260, 200}, .font = &font()});
    buildPlain(l, false);
    buildPlain(r, true);
    Image a, b;
    render(l, a);
    render(r, b);
    int bad = 0;
    for (int y = 0; y < a.height; ++y)
        for (int x = 0; x < a.width; ++x) {
            const u32 p = a.at(a.width - 1 - x, y), q = b.at(x, y);
            for (int c = 0; c < 4; ++c)
                if (std::abs(int(ch(p, c)) - int(ch(q, c))) > 3) { ++bad; break; }
        }
    std::printf("mirror diff: %d pixels > 3\n", bad);
    UI_CHECK(bad <= a.width * a.height / 500);
}

// The Tier A sample in RTL plus a menu bar, tab bar, tree and a scroll area.
void golden(const std::string& path) {
    UiContext ui({.viewport = {488, 560}, .font = &font()});
    widgets::install(ui);
    ui.setCss(ui.root(), "direction", "rtl");
    buildSample(ui);
    const NodeHandle card = ui.find("card");
    const NodeHandle bar = menuBar(ui, card);
    menuBarMenu(ui, bar, "File", "Open");
    menuBarMenu(ui, bar, "Edit", "Undo");
    menuBarMenu(ui, bar, "View", "Zoom");
    ui.moveTo(bar, card, ui.firstChild(card));
    tabBar(ui, card, "General\nAdvanced\nAbout", 1);
    const NodeHandle row = panel(ui, card);
    ui.addClass(row, "row");
    const NodeHandle tree = treeView(ui, row, "Scene\n\tCamera\n\tLights\n\t\tSun\nAssets");
    ui.setCss(tree, "height", "90px");
    ui.setValue(tree, 1.f);
    const NodeHandle sa = scrollArea(ui, row);
    ui.setCss(sa, "height", "90px");
    ui.setCss(sa, "flex-grow", "1");
    for (const char* t : {"Alpha", "Beta", "Gamma", "Delta", "Epsilon", "Zeta", "Eta", "Theta"}) label(ui, sa, t);
    ui.setFocus(ui.find("name"), FocusReason::Keyboard); // caret at the end of the line edit
    ui.update();
    ui.update();
    Image img;
    render(ui, img);
    if (std::getenv("FUSE_UI_UPDATE_GOLDEN")) {
        stbi_write_png(path.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("golden written: %s\n", path.c_str());
        return;
    }
    int w = 0, h = 0, n = 0;
    unsigned char* ref = stbi_load(path.c_str(), &w, &h, &n, 4);
    UI_CHECK(ref != nullptr);
    if (!ref) return;
    UI_CHECK_EQ(w, img.width);
    UI_CHECK_EQ(h, img.height);
    if (w == img.width && h == img.height) {
        int bad = 0, maxd = 0;
        for (int i = 0; i < w * h; ++i)
            for (int c = 0; c < 4; ++c) {
                const int d = std::abs(int(ref[i * 4 + c]) - int(ch(img.pixels[std::size_t(i)], c)));
                maxd = std::max(maxd, d);
                if (d > 3) { ++bad; break; }
            }
        std::printf("golden diff: %d pixels > 3, max channel diff %d\n", bad, maxd);
        UI_CHECK(bad <= w * h / 1000);
    }
    stbi_image_free(ref);
}

} // namespace

int main(int argc, char** argv) {
    boxRow();
    gridAndFlow();
    textAlign();
    sliderAndTabs();
    treeKeys();
    scrollContainer();
    menus();
    mirrorPixels();
#if FUSE_UI_TEST_TEXT_FULL
    golden(argc > 1 ? argv[1] : "golden/rtl_layout_full.png"); // FreeType coverage differs from stb_truetype
#else
    golden(argc > 1 ? argv[1] : "golden/rtl_layout.png");
#endif
    return finish("fuse_ui_rtl_layout");
}
