// fuse_ui_widget_<class> (UI6 gates, plan §5 "every widget ships with ..."): one ctest per registered widget class,
// one binary (--gate=<class>). Each gate builds the widget in a small window and checks:
//   - it exists as a live node of that class with the class's accessibility role in the a11y snapshot
//   - it is laid out (non-empty rect) and paints (draw commands of its subtree, pixels inside its rect)
//   - theme states: a disabled interactive widget looks different and ignores pointer and keyboard input
//   - keyboard: a focusable widget is reached by Tab
//   - its own behaviour (click / keys / drag / wheel per widget)
//   - golden: the CPU raster of the gate scene against tests/golden/widgets/<class>.png (MINIMAL text profile;
//     FUSE_UI_UPDATE_GOLDEN=1 rewrites it), and two renders are identical
// --gate=catalogue checks that every class in the registry has a gate here or in fuse_ui_tier_d2
// (FUSE_UI_WIDGET_GATES / FUSE_UI_WIDGET_D2_CLASSES from CMake), --list prints the registry.
#include "fuse/ui/icons.hpp"
#include "fuse/ui/widgets/platform_feel.hpp"
#include "test_util.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers" // stb_image_write's `= { 0 }`
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

#ifndef FUSE_UI_WIDGET_GATES
#define FUSE_UI_WIDGET_GATES ""
#endif
#ifndef FUSE_UI_WIDGET_D2_CLASSES
#define FUSE_UI_WIDGET_D2_CLASSES ""
#endif

namespace {
constexpr f32 kW = 360.f, kH = 240.f;

/// CPU raster of the context with image 0 = an 8x8 checker (the image widget shows it).
void raster(UiContext& ui, Image& img) {
    static const Image checker = [] {
        Image c;
        c.resize(8, 8);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) c.pixels[std::size_t(y) * 8 + std::size_t(x)] = ((x ^ y) & 1) ? 0xFFF08020u : 0xFF404040u;
        return c;
    }();
    img.resize(int(ui.viewport().w), int(ui.viewport().h), 0xFF000000u);
    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    r.addImage(&checker);
    r.render(ui.drawList(), img);
}

/// Displayed: the node and every ancestor (display != none).
bool shown(const UiContext& ui, NodeHandle n) {
    for (; ui.alive(n); n = ui.parent(n))
        if (!ui.computed(n).visible) return false;
    return true;
}

/// Every signal of a node, counted.
struct Signals {
    int clicked = 0, toggled = 0, value = 0, text = 0, submitted = 0, focus = 0;
    int any() const { return clicked + toggled + value + text + submitted; }
    void watch(UiContext& ui, NodeHandle n) {
        ui.connect(n, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { ++clicked; });
        ui.connect(n, SignalKind::Toggled, [this](UiContext&, NodeHandle, const SignalArgs&) { ++toggled; });
        ui.connect(n, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs&) { ++value; });
        ui.connect(n, SignalKind::TextChanged, [this](UiContext&, NodeHandle, const SignalArgs&) { ++text; });
        ui.connect(n, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs&) { ++submitted; });
        ui.connect(n, SignalKind::FocusChanged, [this](UiContext&, NodeHandle, const SignalArgs&) { ++focus; });
    }
};

struct Gate {
    UiContext& ui;
    NodeHandle host;    ///< parent the builder adds to
    NodeHandle subject; ///< node of the gated class
    NodeHandle built;   ///< what the builder returned (the widget the user creates)
    UiTestDriver drv{ui};
    Vec2 centre(NodeHandle n) const {
        const Rect r = ui.rect(n);
        return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    }
    void click(NodeHandle n) { drv.clickAt(centre(n)); }
    void key(Key k, u8 mods = 0) { drv.key(k, mods); }
    void frames(int n = 2) {
        for (int i = 0; i < n; ++i) ui.update(1.f / 60.f);
    }
    void drag(Vec2 a, Vec2 b) {
        ui.pointerMove(a);
        ui.pointerDown(a);
        for (int i = 1; i <= 8; ++i) ui.pointerMove({a.x + (b.x - a.x) * f32(i) / 8.f, a.y + (b.y - a.y) * f32(i) / 8.f});
        ui.pointerUp(b);
        frames();
    }
};

struct Spec {
    const char* cls;
    /// Builds the widget under `host`; returns the node the user created (the subject is found by class under it).
    NodeHandle (*build)(UiContext& ui, NodeHandle host);
    /// Behaviour checks (null: passive widget).
    void (*behave)(Gate& g);
    bool painted = true;   ///< paints something of its own
    bool disables = false; ///< interactive: the disabled-state check applies
};

/// Rows [y0, y1) of the ink (pixels differing from `bg`) in columns [x0, x1); false when there is none.
[[maybe_unused]] bool inkRows(const Image& img, int x0, int x1, int ya, int yb, u32 bg, int& y0, int& y1) {
    y0 = 1 << 30;
    y1 = -1;
    for (int y = std::max(0, ya); y < std::min(img.height, yb); ++y)
        for (int x = std::max(0, x0); x < std::min(img.width, x1); ++x)
            if (img.pixels[std::size_t(y) * std::size_t(img.width) + std::size_t(x)] != bg) {
                y0 = std::min(y0, y);
                y1 = std::max(y1, y + 1);
            }
    return y1 > y0;
}

/// IME pre-edit drawn by a fallback face (CJK, taller than the UI font): the line edit shows it whole, on the same
/// baseline as a label of the same text, not clipped at the top of the field. At 1x and at a phone's DPI scale.
[[maybe_unused]] void checkCjkPreedit(f32 scale, f32 fontPx) {
    static auto base = fuse::text::Font::bundled();
    static std::vector<fuse::text::Font> chain = fuse::text::Font::bundledFallbacks();
    UI_CHECK(base.has_value());
    if (!base) return;
    std::vector<const fuse::text::Font*> ptrs;
    for (const auto& f : chain) ptrs.push_back(&f);
    base->setFallbacks(ptrs);
    const std::string kNihon = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"; // 日本語
    if (base->faceFor(U'日') == 0) {
        std::printf("  CJK pre-edit: no CJK fallback face (skipped)\n");
        return;
    }
    UiContext ui({.viewport = {320.f, 120.f}, .font = &*base});
    w::install(ui);
    ui.setScale(scale);
    ui.setCss(ui.root(), "padding", "16px");
    ui.setCss(ui.root(), "gap", "16px");
    char fs[32];
    std::snprintf(fs, sizeof fs, "%gpx", double(fontPx));
    const NodeHandle e = w::lineEdit(ui, ui.root(), "ab", "le");
    ui.setCss(e, "width", "240px");
    ui.setCss(e, "font-size", fs);
    const NodeHandle ref = w::label(ui, ui.root(), "ab" + kNihon, "ref"); // the same text, unclipped
    ui.setCss(ref, "font-size", fs);
    ui.update(1.f / 60.f);
    ui.setFocus(e);
    ui.textComposition(kNihon, i32(kNihon.size()));
    ui.update(1.f / 60.f);
    Image img;
    img.resize(int(std::ceil(ui.viewport().w * scale)), int(std::ceil(ui.viewport().h * scale)), 0xFF000000u);
    CpuRasterizer rast;
    rast.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    rast.render(ui.drawList(), img);
    if (const char* out = std::getenv("FUSE_UI_CJK_PREEDIT_PNG"))
        stbi_write_png(out, img.width, img.height, 4, img.pixels.data(), img.width * 4);
    auto dev = [&](f32 v) { return int(std::lround(v * scale)); };
    const Rect c = ui.contentRect(e), rr = ui.contentRect(ref);
    const f32 px = ui.computed(e).fontSize;
    const f32 abW = ui.measureString("ab", px), preW = ui.measureString(kNihon, px);
    const u32 bg = img.pixels[std::size_t(dev(c.y + 1.f)) * std::size_t(img.width) + std::size_t(dev(c.right() - 4.f))];
    const u32 bgRef = img.pixels[std::size_t(dev(rr.y)) * std::size_t(img.width) + std::size_t(dev(rr.right()) - 1)];
    // the pre-edit underline: the lowest row inked across the whole pre-edit span inside the border
    const Rect br = ui.rect(e);
    const f32 bw = ui.computed(e).borderWidth;
    const int top = dev(br.y + bw) + 1, bottom = dev(br.bottom() - bw) - 1;
    const int px0 = dev(c.x + abW) + 1, px1 = dev(c.x + abW + preW) - 2;
    auto fullRow = [&](int y) {
        for (int x = px0; x < px1; ++x)
            if (img.pixels[std::size_t(y) * std::size_t(img.width) + std::size_t(x)] == bg) return false;
        return true;
    };
    int under = -1; // top row of the bar (a 1 px rect covers several device rows when scaled)
    for (int y = bottom - 1; y >= top && under < 0; --y)
        if (fullRow(y)) under = y;
    while (under > top && fullRow(under - 1)) --under;
    // ink rows of "ab" and of the pre-edit glyphs (above the underline) in the field, and of the same glyphs in the label
    int fa0 = 0, fa1 = 0, fp0 = 0, fp1 = 0, ra0 = 0, ra1 = 0, rp0 = 0, rp1 = 0;
    const int m = dev(6.f);
    const bool ok = under > top && inkRows(img, dev(c.x), dev(c.x + abW), top, under, bg, fa0, fa1) &&
                    inkRows(img, px0, px1, top, under, bg, fp0, fp1) &&
                    inkRows(img, dev(rr.x), dev(rr.x + abW), dev(rr.y) - m, dev(rr.bottom()) + m, bgRef, ra0, ra1) &&
                    inkRows(img, px0 - dev(c.x) + dev(rr.x), px1 - dev(c.x) + dev(rr.x), dev(rr.y) - m, dev(rr.bottom()) + m,
                            bgRef, rp0, rp1);
    std::printf("  CJK pre-edit at %.2fx, %gpx: field rows %d..%d, 'ab' %d..%d, pre-edit %d..%d, underline %d; label 'ab' "
                "%d..%d, CJK %d..%d\n",
                double(scale), double(fontPx), top, bottom, fa0, fa1, fp0, fp1, under, ra0, ra1, rp0, rp1);
    UI_CHECK(ok);
    if (!ok) return;
    UI_CHECK(fp0 > top);                             // not cut at the top of the field
    UI_CHECK(fp0 - fa0 == rp0 - ra0);                // on the committed text's baseline, as in the label
    UI_CHECK(fp1 - fp0 == rp1 - rp0 && fp1 < under); // whole glyphs, clear of the underline
}

// ---- models shared by the model / view builders ----
w::StringTableModel& tableModel() {
    static w::StringTableModel m("Name\tKind\tSize");
    if (m.rows() == 0) {
        const char* rows[] = {"crate\tmesh\t12", "barrel\tmesh\t7", "torch\tlight\t1", "door\tprefab\t40", "rock\tmesh\t3",
                              "player\tprefab\t90", "sun\tlight\t1", "wall\tmesh\t22", "key\titem\t1", "chest\tprefab\t15"};
        for (const char* r : rows) m.addRow(r);
    }
    return m;
}

void checkValueKeys(Gate& g, Key up, Key down) {
    g.ui.setFocus(g.subject);
    const f32 v0 = g.ui.value(g.subject);
    g.key(up);
    const f32 v1 = g.ui.value(g.subject);
    g.key(down);
    g.key(down);
    const f32 v2 = g.ui.value(g.subject);
    std::printf("  keys: value %.3f -> %.3f -> %.3f\n", v0, v1, v2);
    UI_CHECK(v1 > v0 && v2 < v1);
}

const Spec kSpecs[] = {
    // ---- core ----
    {"panel", [](UiContext& ui, NodeHandle h) {
         const NodeHandle p = w::panel(ui, h, "subject");
         ui.addClass(p, "card");
         ui.setCss(p, "width", "200px");
         ui.setCss(p, "height", "80px");
         w::label(ui, p, "inside a panel");
         return p;
     }, nullptr},
    {"label", [](UiContext& ui, NodeHandle h) { return w::label(ui, h, "The quick brown fox", "subject"); }, nullptr},
    // ---- Tier A ----
    {"button", [](UiContext& ui, NodeHandle h) { return w::button(ui, h, "Press me", "subject"); },
     [](Gate& g) {
         Signals s;
         s.watch(g.ui, g.subject);
         g.click(g.subject);
         g.ui.setFocus(g.subject);
         g.key(Key::Space);
         g.key(Key::Enter);
         std::printf("  clicks: pointer + Space + Enter = %d\n", s.clicked);
         UI_CHECK(s.clicked == 3);
         // press on it, release outside: no click
         const Vec2 c = g.centre(g.subject);
         g.ui.pointerMove(c);
         g.ui.pointerDown(c);
         g.ui.pointerMove({c.x, kH - 4.f});
         g.ui.pointerUp({c.x, kH - 4.f});
         g.frames();
         UI_CHECK(s.clicked == 3);
     }, true, true},
    {"toolbutton", [](UiContext& ui, NodeHandle h) { return w::toolButton(ui, h, "Tool", "subject"); },
     [](Gate& g) {
         Signals s;
         s.watch(g.ui, g.subject);
         g.click(g.subject);
         UI_CHECK(s.clicked == 1);
     }, true, true},
#ifdef FUSE_UI_ICON_PACK // the build-time starter pack
    {"icon", [](UiContext& ui, NodeHandle h) { return w::icon(ui, h, "save", 24.f, "subject"); }, nullptr},
#else // no IconSet: nothing to draw
    {"icon", [](UiContext& ui, NodeHandle h) { return w::icon(ui, h, "save", 24.f, "subject"); }, nullptr, false},
#endif
    {"link", [](UiContext& ui, NodeHandle h) { return w::link(ui, h, "https://example.org", "subject"); },
     [](Gate& g) {
         Signals s;
         s.watch(g.ui, g.subject);
         g.click(g.subject);
         UI_CHECK(s.clicked == 1);
         g.drv.hover("subject");
         UI_CHECK(g.ui.cursor() == CursorShape::Hand);
     }, true, true},
    {"checkbox", [](UiContext& ui, NodeHandle h) { return w::checkBox(ui, h, "Enable shadows", false, "subject"); },
     [](Gate& g) {
         Signals s;
         s.watch(g.ui, g.subject);
         g.click(g.subject);
         UI_CHECK(g.ui.checked(g.subject) && s.toggled == 1);
         g.ui.setFocus(g.subject);
         g.key(Key::Space);
         UI_CHECK(!g.ui.checked(g.subject) && s.toggled == 2);
     }, true, true},
    {"radio", [](UiContext& ui, NodeHandle h) {
         const NodeHandle col = w::panel(ui, h, "group");
         const NodeHandle a = w::radioButton(ui, col, "Low", true, "subject");
         w::radioButton(ui, col, "High", false, "r2");
         return a;
     },
     [](Gate& g) {
         const NodeHandle r2 = g.ui.find("r2");
         g.click(r2);
         std::printf("  radio: first %d, second %d\n", g.ui.checked(g.subject), g.ui.checked(r2));
         UI_CHECK(g.ui.checked(r2) && !g.ui.checked(g.subject)); // exclusive within the parent
         g.click(g.subject);
         UI_CHECK(!g.ui.checked(r2) && g.ui.checked(g.subject));
     }, true, true},
    {"switch", [](UiContext& ui, NodeHandle h) { return w::toggleSwitch(ui, h, "Vsync", false, "subject"); },
     [](Gate& g) {
         Signals s;
         s.watch(g.ui, g.subject);
         g.click(g.subject);
         UI_CHECK(g.ui.checked(g.subject) && s.toggled == 1);
     }, true, true},
    {"slider", [](UiContext& ui, NodeHandle h) {
         const NodeHandle s = w::slider(ui, h, 0.f, 100.f, 50.f, 1.f, "subject");
         ui.setCss(s, "width", "200px");
         return s;
     },
     [](Gate& g) {
         checkValueKeys(g, Key::Right, Key::Left);
         const Rect r = g.ui.rect(g.subject);
         g.drag({r.x + r.w * 0.5f, r.y + r.h * 0.5f}, {r.x + r.w - 1.f, r.y + r.h * 0.5f});
         std::printf("  drag to the end: %.1f\n", g.ui.value(g.subject));
         UI_CHECK(g.ui.value(g.subject) >= 95.f);
     }, true, true},
    {"scrollbar", [](UiContext& ui, NodeHandle h) {
         const NodeHandle s = w::scrollBar(ui, h, false, "subject");
         ui.setCss(s, "width", "220px");
         return s;
     },
     [](Gate& g) {
         const f32 v0 = g.ui.value(g.subject);
         const Rect r = g.ui.rect(g.subject);
         g.drag({r.x + 6.f, r.y + r.h * 0.5f}, {r.x + r.w - 2.f, r.y + r.h * 0.5f});
         std::printf("  drag: value %.3f -> %.3f\n", v0, g.ui.value(g.subject));
         UI_CHECK(g.ui.value(g.subject) > v0);
     }, true, true},
    {"dial", [](UiContext& ui, NodeHandle h) { return w::dial(ui, h, 0.f, 1.f, 0.5f, "subject"); },
     [](Gate& g) { checkValueKeys(g, Key::Up, Key::Down); }, true, true},
    {"progressbar", [](UiContext& ui, NodeHandle h) {
         const NodeHandle p = w::progressBar(ui, h, 0.4f, "subject");
         ui.setCss(p, "width", "200px");
         return p;
     },
     [](Gate& g) { // the fill follows the value
         Image a, b;
         raster(g.ui, a);
         g.ui.setValue(g.subject, 0.9f);
         g.frames();
         raster(g.ui, b);
         const Rect r = g.ui.rect(g.subject);
         const int x = int(r.x + r.w * 0.7f), y = int(r.y + r.h * 0.5f);
         std::printf("  fill pixel at 70%%: 0x%08x -> 0x%08x\n", a.at(x, y), b.at(x, y));
         UI_CHECK(a.at(x, y) != b.at(x, y));
     }},
    {"spinbox", [](UiContext& ui, NodeHandle h) { return w::spinBox(ui, h, 0.f, 10.f, 5.f, 1.f, "subject"); },
     [](Gate& g) {
         checkValueKeys(g, Key::Up, Key::Down);
         g.ui.setValue(g.subject, 10.f);
         g.ui.setFocus(g.subject);
         g.key(Key::Up);
         UI_CHECK(g.ui.value(g.subject) == 10.f); // clamped to the range
     }, true, true},
    {"combobox", [](UiContext& ui, NodeHandle h) { return w::comboBox(ui, h, "Low\nMedium\nHigh", 0, "subject"); },
     [](Gate& g) {
         Signals s;
         s.watch(g.ui, g.subject);
         g.click(g.subject); // opens the popup list
         g.frames();
         g.key(Key::Down);
         g.key(Key::Enter);
         g.frames();
         std::printf("  picked '%.*s' (value %.0f), signals %d\n", int(w::comboText(g.ui, g.subject).size()),
                     w::comboText(g.ui, g.subject).data(), g.ui.value(g.subject), s.any());
         UI_CHECK(g.ui.value(g.subject) == 1.f && w::comboText(g.ui, g.subject) == "Medium" && s.any() >= 1);
     }, true, true},
    {"lineedit", [](UiContext& ui, NodeHandle h) {
         const NodeHandle e = w::lineEdit(ui, h, "", "subject");
         ui.setCss(e, "width", "200px");
         return e;
     },
     [](Gate& g) {
         Signals s;
         s.watch(g.ui, g.subject);
         g.click(g.subject);
         UI_CHECK(g.ui.focused() == g.subject && g.ui.wantsTextInput());
         g.drv.type("hello");
         g.key(Key::Backspace);
         g.drv.type("!");
         g.key(Key::Enter);
         std::printf("  text '%.*s', text changes %d, submitted %d\n", int(g.ui.text(g.subject).size()),
                     g.ui.text(g.subject).data(), s.text, s.submitted);
         UI_CHECK(g.ui.text(g.subject) == "hell!" && s.text >= 3 && s.submitted == 1);
         g.drv.hover("subject");
         UI_CHECK(g.ui.cursor() == CursorShape::Text);
#if !FUSE_UI_TEST_TEXT_COOKED // cooked CJK strikes are resampled: ink heights differ from the label's by a pixel
         for (const f32 sc : {1.f, 2.75f})
             for (const f32 fpx : {14.f, 20.f}) checkCjkPreedit(sc, fpx);
#endif
     }, true, true},
    {"image", [](UiContext& ui, NodeHandle h) { return w::image(ui, h, 0, 64.f, 48.f, "subject"); }, nullptr},
    {"separator", [](UiContext& ui, NodeHandle h) {
         const NodeHandle s = w::separator(ui, h, "subject");
         ui.setCss(s, "width", "200px");
         return s;
     }, nullptr},
    // ---- Tier B ----
    {"layer", [](UiContext& ui, NodeHandle h) {
         w::label(ui, h, "under the layer");
         w::popupMenu(ui, h, "Cut\nCopy\nPaste", {40.f, 40.f}, "pm"); // popups live in the overlay layer
         return w::overlayLayer(ui);
     }, nullptr, false},
    {"popup", [](UiContext& ui, NodeHandle h) {
         const NodeHandle b = w::button(ui, h, "owner", "owner");
         return w::popupMenu(ui, b, "Cut\nCopy\nPaste", {40.f, 60.f}, "pm");
     },
     [](Gate& g) { // Escape closes it and focus returns to the owner
         UI_CHECK(w::popupOpen(g.ui, g.built));
         g.key(Key::Escape);
         g.frames();
         UI_CHECK(!w::popupOpen(g.ui, g.built));
     }, false},
    {"menu", [](UiContext& ui, NodeHandle h) {
         const NodeHandle b = w::button(ui, h, "owner", "owner");
         return w::popupMenu(ui, b, "Cut\nCopy\n-\nPaste", {40.f, 60.f}, "pm");
     },
     [](Gate& g) { // keyboard: focus starts on Cut, Down -> Copy, Down skips the separator -> Paste (item 3)
         Signals s;
         s.watch(g.ui, g.ui.find("owner"));
         int picked = -1;
         g.ui.connect(g.ui.find("owner"), SignalKind::Submitted,
                      [&picked](UiContext&, NodeHandle, const SignalArgs& a) { picked = int(a.value); });
         g.key(Key::Down);
         g.key(Key::Down);
         g.key(Key::Enter);
         g.frames();
         std::printf("  picked item %d\n", picked);
         UI_CHECK(picked == 3 && s.submitted == 1 && !w::popupOpen(g.ui, g.built));
     }},
    {"menuitem", [](UiContext& ui, NodeHandle h) {
         const NodeHandle b = w::button(ui, h, "owner", "owner");
         return w::popupMenu(ui, b, "Cut\nCopy\nPaste", {40.f, 60.f}, "pm");
     },
     [](Gate& g) { // clicking an item picks it
         int picked = -1;
         g.ui.connect(g.ui.find("owner"), SignalKind::Submitted,
                      [&picked](UiContext&, NodeHandle, const SignalArgs& a) { picked = int(a.value); });
         g.drv.hover("owner");
         g.click(g.subject);
         g.frames();
         UI_CHECK(picked == 0);
     }},
    {"menubar", [](UiContext& ui, NodeHandle h) {
         const NodeHandle bar = w::menuBar(ui, h, "subject");
         w::menuBarMenu(ui, bar, "File", "New\nOpen\nSave", "file");
         w::menuBarMenu(ui, bar, "Edit", "Undo\nRedo", "edit");
         return bar;
     },
     [](Gate& g) { // a title opens its menu, picking re-emits on the title
         int picked = -1;
         const NodeHandle file = g.ui.find("file");
         g.ui.connect(file, SignalKind::Submitted, [&picked](UiContext&, NodeHandle, const SignalArgs& a) { picked = int(a.value); });
         g.click(file);
         g.frames();
         g.key(Key::Down); // from New to Open
         g.key(Key::Enter);
         g.frames();
         std::printf("  File menu picked %d\n", picked);
         UI_CHECK(picked == 1);
     }},
    {"menubutton", [](UiContext& ui, NodeHandle h) {
         const NodeHandle bar = w::menuBar(ui, h, "bar");
         return w::menuBarMenu(ui, bar, "View", "Zoom in\nZoom out", "subject");
     },
     [](Gate& g) {
         int picked = -1;
         g.ui.connect(g.subject, SignalKind::Submitted, [&picked](UiContext&, NodeHandle, const SignalArgs& a) { picked = int(a.value); });
         g.click(g.subject);
         g.frames();
         g.key(Key::Down);
         g.key(Key::Enter);
         g.frames();
         std::printf("  picked %d\n", picked);
         UI_CHECK(picked == 1);
     }, true, true},
    {"tooltip", [](UiContext& ui, NodeHandle h) {
         const NodeHandle b = w::button(ui, h, "Hover me", "owner");
         const NodeHandle t = w::setTooltip(ui, b, "A helpful tip", 0.2f);
         ui.update(1.f / 60.f);
         const Rect r = ui.rect(b);
         ui.pointerMove({r.x + r.w * 0.5f, r.y + r.h * 0.5f}); // hovered past the delay: the gate sees it shown
         for (int i = 0; i < 30; ++i) ui.update(1.f / 60.f);
         return t;
     },
     [](Gate& g) { // hidden on leave, shown again only after the delay
         const bool was = shown(g.ui, g.subject);
         g.ui.pointerMove({kW - 2.f, kH - 2.f});
         g.frames(3);
         const bool left = shown(g.ui, g.subject);
         g.drv.hover("owner");
         const bool early = shown(g.ui, g.subject);
         for (int i = 0; i < 30; ++i) g.ui.update(1.f / 60.f);
         std::printf("  tooltip shown %d, after leave %d, at once %d, after the delay %d\n", was, left, early,
                     shown(g.ui, g.subject));
         UI_CHECK(was && !left && !early && shown(g.ui, g.subject));
     }},
    {"listview", [](UiContext& ui, NodeHandle h) {
         const NodeHandle l = w::listView(ui, h, "Alpha\nBeta\nGamma\nDelta\nEpsilon\nZeta\nEta\nTheta", -1, "subject");
         ui.setCss(l, "width", "200px");
         ui.setCss(l, "height", "120px");
         return l;
     },
     [](Gate& g) {
         Signals s;
         s.watch(g.ui, g.subject);
         const Rect r = g.ui.rect(g.subject);
         g.drv.clickAt({r.x + 20.f, r.y + 6.f}); // the first row
         UI_CHECK(g.ui.value(g.subject) == 0.f);
         g.key(Key::Down);
         g.key(Key::Enter);
         std::printf("  selected %.0f, value changes %d, submitted %d\n", g.ui.value(g.subject), s.value, s.submitted);
         UI_CHECK(g.ui.value(g.subject) == 1.f && s.value == 2 && s.submitted == 1);
     }, true, true},
    {"treeview", [](UiContext& ui, NodeHandle h) {
         const NodeHandle t = w::treeView(ui, h, "World\n\tPlayer\n\t\tCamera\n\tLights\n\t\tSun\nUI", "subject");
         ui.setCss(t, "width", "200px");
         ui.setCss(t, "height", "140px");
         return t;
     },
     [](Gate& g) {
         Signals s;
         s.watch(g.ui, g.subject);
         const u32 rows0 = w::treeVisibleRows(g.ui, g.subject);
         w::setTreeExpanded(g.ui, g.subject, 0, false);
         g.frames();
         const u32 rows1 = w::treeVisibleRows(g.ui, g.subject);
         g.ui.setFocus(g.subject);
         g.ui.setValue(g.subject, 0.f);
         g.key(Key::Right); // expands the selected row again
         std::printf("  rows %u -> collapsed %u -> %u, toggles %d\n", rows0, rows1, w::treeVisibleRows(g.ui, g.subject), s.toggled);
         UI_CHECK(rows0 == 6 && rows1 == 2 && w::treeVisibleRows(g.ui, g.subject) == 6 && w::treeExpanded(g.ui, g.subject, 0));
     }, true, true},
    {"tabbar", [](UiContext& ui, NodeHandle h) { return w::tabBar(ui, h, "Scene\nGame\nConsole", 0, "subject"); },
     [](Gate& g) {
         Signals s;
         s.watch(g.ui, g.subject);
         g.ui.setFocus(g.subject);
         g.key(Key::Right);
         g.key(Key::Right);
         const f32 byKeys = g.ui.value(g.subject);
         const Rect r = g.ui.rect(g.subject);
         g.drv.clickAt({r.x + 6.f, r.y + r.h * 0.5f}); // the first tab
         std::printf("  tab by keys %.0f, after clicking the first %.0f, value changes %d\n", byKeys, g.ui.value(g.subject),
                     s.value);
         UI_CHECK(byKeys == 2.f && g.ui.value(g.subject) == 0.f && s.value == 3);
     }, true, true},
    {"splithandle", [](UiContext& ui, NodeHandle h) {
         const NodeHandle sp = w::splitter(ui, h, false, "split");
         ui.setCss(sp, "width", "300px");
         ui.setCss(sp, "height", "100px");
         w::label(ui, w::splitterPane(ui, sp, 120.f, "left"), "left");
         w::label(ui, w::splitterPane(ui, sp, 0.f, "right"), "right");
         return sp;
     },
     [](Gate& g) {
         const f32 w0 = g.ui.rect(g.ui.find("left")).w;
         const Vec2 c = g.centre(g.subject);
         g.ui.pointerMove(c);
         g.frames();
         UI_CHECK(g.ui.cursor() == CursorShape::ResizeEW);
         g.drag(c, {c.x + 40.f, c.y});
         const f32 w1 = g.ui.rect(g.ui.find("left")).w;
         std::printf("  left pane %.0f -> %.0f px, cursor %s\n", w0, w1, cursorShapeName(g.ui.cursor()));
         UI_CHECK(std::abs(w1 - w0 - 40.f) <= 1.f);
     }},
    {"docktitle", [](UiContext& ui, NodeHandle h) {
         const NodeHandle area = w::dockArea(ui, h, "area");
         ui.setCss(area, "width", "340px");
         ui.setCss(area, "height", "200px");
         w::label(ui, w::dockBody(ui, w::dockWidget(ui, area, "Outliner", w::DockSide::Left, "dock")), "rows");
         return area;
     },
     [](Gate& g) { // the title's close button hides the dock (Toggled false)
         const NodeHandle dock = g.ui.find("dock");
         int toggles = 0;
         bool open = true;
         g.ui.connect(dock, SignalKind::Toggled, [&](UiContext&, NodeHandle, const SignalArgs& a) { ++toggles; open = a.flag; });
         const Rect r = g.ui.rect(g.subject);
         g.drv.clickAt({r.x + r.w - 8.f, r.y + r.h * 0.5f});
         g.frames();
         std::printf("  close button: toggles %d, open %d, dock visible %d\n", toggles, open, shown(g.ui, dock));
         UI_CHECK(toggles == 1 && !open);
     }},
    {"tableview", [](UiContext& ui, NodeHandle h) {
         const NodeHandle t = w::tableView(ui, h, &tableModel().model(), "subject");
         ui.setCss(t, "width", "320px");
         ui.setCss(t, "height", "150px");
         return t;
     },
     [](Gate& g) {
         Signals s;
         s.watch(g.ui, g.subject);
         const Rect r = g.ui.rect(g.subject);
         g.drv.clickAt({r.x + 30.f, r.y + 40.f}); // a body row
         const f32 row = g.ui.value(g.subject);
         g.key(Key::Down);
         g.key(Key::Enter);
         g.drv.clickAt({r.x + 30.f, r.y + 8.f}); // the Name header sorts
         std::printf("  row %.0f -> %.0f, sorts %d, submitted %d\n", row, g.ui.value(g.subject), s.toggled, s.submitted);
         UI_CHECK(row >= 0.f && s.submitted == 1 && s.toggled == 1);
     }, true, true},
    // ---- Tier C / D ----
    {"gridview", [](UiContext& ui, NodeHandle h) {
         const NodeHandle gv = w::gridView(ui, h, &tableModel().model(), 72.f, 56.f, "subject");
         ui.setCss(gv, "width", "320px");
         ui.setCss(gv, "height", "150px");
         return gv;
     },
     [](Gate& g) {
         Signals s;
         s.watch(g.ui, g.subject);
         const Rect r = g.ui.rect(g.subject);
         g.drv.clickAt({r.x + 20.f, r.y + 20.f});
         g.key(Key::Right);
         g.key(Key::Down);
         g.key(Key::Enter);
         const u32 cols = w::gridColumns(g.ui, g.subject);
         std::printf("  columns %u, selected %.0f, submitted %d\n", cols, g.ui.value(g.subject), s.submitted);
         UI_CHECK(cols >= 2 && g.ui.value(g.subject) == f32(1 + cols) && s.submitted == 1);
     }, true, true},
    {"colorpicker", [](UiContext& ui, NodeHandle h) { return w::colorPicker(ui, h, Color::rgb(0x3080F0), "subject"); },
     [](Gate& g) {
         Signals s;
         s.watch(g.ui, g.subject);
         const Color c0 = w::colorPickerColor(g.ui, g.subject);
         const Rect r = g.ui.rect(g.subject);
         g.drv.clickAt({r.x + 10.f, r.y + 10.f}); // saturation / value square: top left = white
         const Color c1 = w::colorPickerColor(g.ui, g.subject);
         std::printf("  colour %.2f,%.2f,%.2f -> %.2f,%.2f,%.2f, text changes %d\n", c0.r, c0.g, c0.b, c1.r, c1.g, c1.b, s.text);
         UI_CHECK(s.text >= 1 && (c1.r != c0.r || c1.g != c0.g || c1.b != c0.b));
     }, true, true},
    {"calendar", [](UiContext& ui, NodeHandle h) { return w::calendar(ui, h, 2026, 11, 5, "subject"); },
     [](Gate& g) {
         g.ui.setFocus(g.subject);
         g.key(Key::Right);
         g.key(Key::Down);
         int y = 0, m = 0, d = 0;
         w::calendarDate(g.ui, g.subject, y, m, d);
         std::printf("  5 Nov + 1 day + 1 week = %04d-%02d-%02d\n", y, m, d);
         UI_CHECK(y == 2026 && m == 11 && d == 13);
         g.key(Key::PageDown);
         w::calendarDate(g.ui, g.subject, y, m, d);
         UI_CHECK(y == 2026 && m == 12);
     }, true, true},
    {"chart", [](UiContext& ui, NodeHandle h) {
         const NodeHandle c = w::chart(ui, h, w::ChartKind::Line, "subject");
         ui.setCss(c, "width", "320px");
         ui.setCss(c, "height", "160px");
         const f32 a[] = {1.f, 3.f, 2.f, 5.f, 4.f, 6.f}, b[] = {2.f, 2.5f, 1.f, 3.f, 3.5f, 2.f};
         w::chartSetSeries(ui, c, 0, "fps", a, Color::rgb(0x4FC3F7));
         w::chartSetSeries(ui, c, 1, "budget", b, Color::rgb(0xFF8A65));
         return c;
     },
     [](Gate& g) { // hover picks the nearest x
         UI_CHECK(w::chartSeriesCount(g.ui, g.subject) == 2);
         const Rect r = g.ui.rect(g.subject);
         g.ui.pointerMove({r.x + r.w * 0.5f, r.y + r.h * 0.5f});
         g.frames();
         const f32 near = g.ui.value(g.subject);
         g.ui.pointerMove({kW - 1.f, kH - 1.f});
         g.frames();
         std::printf("  hover x index %.0f, outside %.0f\n", near, g.ui.value(g.subject));
         UI_CHECK(near >= 2.f && near <= 3.f && g.ui.value(g.subject) == -1.f);
     }},
    {"kineticscroll", [](UiContext& ui, NodeHandle h) {
         const NodeHandle area = w::kineticScrollArea(ui, h, w::ScrollProfile::ios(), "subject");
         ui.setCss(area, "height", "120px");
         ui.setCss(area, "width", "200px");
         for (int i = 0; i < 30; ++i) w::label(ui, area, "row " + std::to_string(i));
         return area;
     },
     [](Gate& g) { // a fling keeps scrolling after release, then settles inside the range
         const Rect r = g.ui.rect(g.subject);
         const Vec2 a{r.x + r.w * 0.5f, r.y + r.h - 10.f};
         g.ui.setPointerKind(UiContext::PointerKind::Touch);
         g.ui.pointerMove(a);
         g.ui.pointerDown(a);
         for (int i = 1; i <= 6; ++i) {
             g.ui.pointerMove({a.x, a.y - f32(i) * 15.f});
             g.ui.update(1.f / 60.f);
         }
         g.ui.pointerUp({a.x, a.y - 90.f});
         const f32 atRelease = g.ui.scroll(g.subject).y;
         for (int i = 0; i < 240; ++i) g.ui.update(1.f / 60.f);
         const f32 settled = g.ui.scroll(g.subject).y;
         std::printf("  scroll at release %.1f, settled %.1f\n", atRelease, settled);
         UI_CHECK(atRelease > 0.f && settled > atRelease);
     }, false},
};

const Spec* findSpec(std::string_view cls) {
    for (const Spec& s : kSpecs)
        if (cls == s.cls) return &s;
    return nullptr;
}

NodeHandle findClass(const UiContext& ui, NodeHandle n, u32 cls) {
    if (!ui.alive(n)) return {};
    if (ui.classOf(n) == cls) return n;
    for (NodeHandle c = ui.firstChild(n); ui.alive(c); c = ui.nextSibling(c))
        if (const NodeHandle f = findClass(ui, c, cls)) return f;
    return {};
}
void subtree(const UiContext& ui, NodeHandle n, std::set<u32>& out) {
    out.insert(n.index);
    for (NodeHandle c = ui.firstChild(n); ui.alive(c); c = ui.nextSibling(c)) subtree(ui, c, out);
}
int countDiff(const Image& a, const Image& b, Rect r) {
    int n = 0;
    for (int y = std::max(0, int(r.y)); y < std::min(a.height, int(r.y + r.h)); ++y)
        for (int x = std::max(0, int(r.x)); x < std::min(a.width, int(r.x + r.w)); ++x) n += a.at(x, y) != b.at(x, y);
    return n;
}

std::vector<std::string> split(std::string_view s) {
    std::vector<std::string> out;
    std::stringstream ss{std::string(s)};
    std::string t;
    while (ss >> t) out.push_back(t);
    return out;
}

int gateCatalogue() {
    UiContext ui({.viewport = {kW, kH}, .font = &font()});
    w::install(ui);
    w::registerPlatformFeel(ui);
    const auto gates = split(FUSE_UI_WIDGET_GATES), d2 = split(FUSE_UI_WIDGET_D2_CLASSES);
    const std::set<std::string> gated(gates.begin(), gates.end()), d2set(d2.begin(), d2.end());
    u32 missing = 0;
    for (u32 i = 0; i < ui.registry().size(); ++i) {
        const std::string& name = ui.registry().get(i).name;
        if (name == "root") continue; // the context's root node, not a widget
        const bool here = gated.count(name) && findSpec(name), other = d2set.count(name) != 0;
        if (!here && !other) {
            std::fprintf(stderr, "widget class '%s' has no fuse_ui_widget_ gate\n", name.c_str());
            ++missing;
        }
    }
    for (const std::string& g : gates)
        if (!findSpec(g)) {
            std::fprintf(stderr, "gate '%s' has no spec in test_widget_gates.cpp\n", g.c_str());
            ++missing;
        }
    std::printf("catalogue: %u classes registered, %zu gated here, %zu in fuse_ui_tier_d2, %u missing\n",
                ui.registry().size(), gates.size(), d2.size(), missing);
    UI_CHECK(missing == 0);
    return finish("fuse_ui_widget_catalogue");
}

int gateWidget(const Spec& spec) {
    const std::string name = std::string("fuse_ui_widget_") + spec.cls;
    UiContext ui({.viewport = {kW, kH}, .font = &font()});
    w::install(ui);
    w::registerPlatformFeel(ui);
#ifdef FUSE_UI_ICON_PACK // icons have something to draw
    static auto icons = IconSet::fromFile(FUSE_UI_ICON_PACK);
    UI_CHECK(icons.has_value());
    if (icons) ui.setIcons(&*icons);
#endif
    const u32 cls = ui.registry().find(spec.cls);
    UI_CHECK(cls != 0xFFFFFFFFu);
    if (cls == 0xFFFFFFFFu) return finish(name.c_str());
    ui.setCss(ui.root(), "padding", "16px");
    ui.setCss(ui.root(), "gap", "8px");
    const NodeHandle before = w::button(ui, ui.root(), "Before", "before"); // Tab starts here
    const NodeHandle host = w::panel(ui, ui.root(), "host");
    Gate g{ui, host, {}, {}};
    g.built = spec.build(ui, host);
    ui.update(1.f / 60.f);
    g.subject = findClass(ui, ui.root(), cls);
    UI_CHECK(ui.alive(g.built) && ui.alive(g.subject));
    if (!ui.alive(g.subject)) return finish(name.c_str());
    const WidgetClass& wc = ui.registry().get(cls);

    // accessibility: the node is in the snapshot with its class role
    std::vector<A11ySnapshotNode> snap;
    ui.snapshotA11y(snap);
    bool inSnapshot = false;
    for (const A11ySnapshotNode& a : snap) inSnapshot |= a.node == g.subject && a.role == wc.role;
    std::printf("%s: role %s%s, focusable %d, interactive %d\n", spec.cls, a11yRoleName(wc.role),
                wc.role == A11yRole::None ? " (not exposed)" : (inSnapshot ? "" : " MISSING from the snapshot"), wc.focusable,
                wc.interactive);
    UI_CHECK(wc.role == A11yRole::None || inSnapshot);

    // layout + paint
    const Rect r = ui.rect(g.subject);
    std::printf("  rect %.0f,%.0f %.0fx%.0f\n", r.x, r.y, r.w, r.h);
    UI_CHECK(r.w > 0.f && r.h > 0.f);
    std::set<u32> nodes;
    subtree(ui, g.subject, nodes);
    u32 cmds = 0;
    for (const DrawCmd& c : ui.drawList().cmds()) cmds += nodes.count(c.node) ? 1u : 0u;
    Image shot, again;
    raster(ui, shot);
    raster(ui, again);
    UI_CHECK(shot.pixels == again.pixels); // deterministic
    if (spec.painted) {
        Image bare;
        ui.setCss(g.subject, "display", "none");
        ui.update(1.f / 60.f);
        raster(ui, bare);
        ui.setCss(g.subject, "display", "flex");
        ui.update(1.f / 60.f);
        const int px = countDiff(shot, bare, r);
        std::printf("  paint: %u draw commands, %d pixels\n", cmds, px);
        UI_CHECK(px > 0);
    }

    // golden (MINIMAL text: stb_truetype coverage; FULL / COOKED differ in glyph edges)
#if !FUSE_UI_TEST_TEXT_FULL && !FUSE_UI_TEST_TEXT_COOKED
    {
        const std::string path = std::string("golden/widgets/") + spec.cls + ".png";
        if (const char* u = std::getenv("FUSE_UI_UPDATE_GOLDEN"); u && *u == '1') {
            stbi_write_png(path.c_str(), shot.width, shot.height, 4, shot.pixels.data(), shot.width * 4);
            std::printf("  golden written: %s\n", path.c_str());
        } else {
            int gw = 0, gh = 0, gn = 0;
            unsigned char* ref = stbi_load(path.c_str(), &gw, &gh, &gn, 4);
            UI_CHECK(ref != nullptr);
            if (ref) {
                UI_CHECK(gw == shot.width && gh == shot.height);
                int bad = 0, maxd = 0;
                if (gw == shot.width && gh == shot.height) {
                    const auto* got = reinterpret_cast<const unsigned char*>(shot.pixels.data());
                    for (int i = 0; i < gw * gh; ++i) {
                        int d = 0;
                        for (int c = 0; c < 4; ++c) d = std::max(d, std::abs(int(got[i * 4 + c]) - int(ref[i * 4 + c])));
                        maxd = std::max(maxd, d);
                        bad += d > 3;
                    }
                }
                std::printf("  golden: %d pixels differ by > 3, max %d\n", bad, maxd);
                UI_CHECK(bad <= gw * gh / 200);
                if (bad > gw * gh / 200) {
                    const std::string out = std::string("widget_") + spec.cls + "_actual.png";
                    stbi_write_png(out.c_str(), shot.width, shot.height, 4, shot.pixels.data(), shot.width * 4);
                }
                stbi_image_free(ref);
            }
        }
    }
#endif

    // keyboard: Tab from the button before it reaches a focusable widget
    if (wc.focusable && shown(ui, g.subject) && !ui.disabled(g.subject)) {
        ui.setFocus(before);
        bool reached = false;
        for (int i = 0; i < 12 && !reached; ++i) {
            g.key(Key::Tab);
            reached = ui.focused() == g.subject;
        }
        std::printf("  Tab reaches it: %s\n", reached ? "yes" : "no");
        UI_CHECK(reached);
        ui.setFocus(before);
        ui.update(1.f / 60.f);
    }

    // disabled: looks different, ignores pointer + keys
    if (spec.disables) {
        Image enabledShot;
        raster(ui, enabledShot);
        ui.setDisabled(g.subject, true);
        ui.update(1.f / 60.f);
        Image disabledShot;
        raster(ui, disabledShot);
        Signals s;
        s.watch(ui, g.subject);
        const f32 v0 = ui.value(g.subject);
        const bool k0 = ui.checked(g.subject);
        g.click(g.subject);
        ui.setFocus(g.subject);
        g.key(Key::Space);
        g.key(Key::Enter);
        g.key(Key::Up);
        g.key(Key::Right);
        g.drv.type("x");
        const int px = countDiff(enabledShot, disabledShot, r);
        std::printf("  disabled: %d pixels change, signals %d, value %s, focus %s\n", px, s.any(),
                    ui.value(g.subject) == v0 && ui.checked(g.subject) == k0 ? "kept" : "CHANGED",
                    ui.focused() == g.subject ? "TAKEN" : "refused");
        UI_CHECK(px > 0 && s.any() == 0 && ui.value(g.subject) == v0 && ui.checked(g.subject) == k0);
        UI_CHECK(ui.focused() != g.subject);
        ui.setDisabled(g.subject, false);
        // close whatever the disabled clicks may not have opened, start the behaviour from a clean frame
        ui.pointerMove({kW - 1.f, kH - 1.f});
        ui.update(1.f / 60.f);
    }

    if (spec.behave) spec.behave(g);
    return finish(name.c_str());
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // keep the report in order with the CHECK lines on stderr
    std::string gate;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--gate=", 7) == 0) gate = argv[i] + 7;
        if (std::strcmp(argv[i], "--list") == 0) list = true;
    }
    if (list) {
        UiContext ui({.viewport = {kW, kH}, .font = &font()});
        w::install(ui);
        w::registerPlatformFeel(ui);
        for (u32 i = 0; i < ui.registry().size(); ++i) std::printf("%s\n", ui.registry().get(i).name.c_str());
        return 0;
    }
    if (gate.empty() || gate == "catalogue") return gateCatalogue();
    const Spec* s = findSpec(gate);
    if (!s) {
        std::fprintf(stderr, "unknown widget gate '%s'\n", gate.c_str());
        return 2;
    }
    return gateWidget(*s);
}
