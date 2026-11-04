// Icon gates (plan UI4: SVG / icon cooker), one executable per gate (FUSE_UI_ICON_GATE):
//   cook    fuse_ui_icon_cook: the cooker is deterministic (same bytes for any input order, and as the build-time
//           pack), the index round-trips (every entry's atlas rect holds exactly the trimmed rasterised SVG), the SVG
//           subset rasterises as specified (shapes, arcs, transforms, fill rules, opacity, currentColor), bad input
//           is rejected and corrupt packs fail to load (byte-flip fuzz) without crashing; resampled sizes track a
//           direct rasterisation; fuse_ui_doc records the icons a document uses. Multi-colour (layered palette,
//           tests/data/icons_color): the build-time pack is byte-identical, each icon's layer plan (colours, order,
//           currentColor / black-only icons stay one tintable layer) and every layer's atlas entry round-trips;
//           version 1 packs still load.
//   render  fuse_ui_icon_render: icons at 16 / 24 / 32 / 48 px (+ 40, resampled) in two tints and a disabled state on
//           the CPU rasteriser against tests/golden/icons.png (FUSE_UI_UPDATE_GOLDEN=1 rewrites; no text, so one
//           golden serves both text profiles); currentColor follows the theme, hover and disabled colours; buttons,
//           menu items, `.fuseui` and cooked style sheets carry icons. Multi-colour icons (16 / 24 / 32 / 40 / 48 px,
//           two tints, disabled) against tests/golden/icons_color.png: one Glyph per layer, currentColor layers take
//           the tint, fixed layers keep their colour.
#include "test_util.hpp"

#include "fuse/ui/icons.hpp"
#include "fuse/ui/icons/icon_cook.hpp"
#if FUSE_UI_ICON_HAVE_DOC
#include "fuse/ui/doc/cook.hpp"
#include "fuse/ui/doc/document.hpp"
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;
namespace ic = fuse::ui::icons;

namespace {
std::vector<u8> readBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
int ch(u32 p, int k) { return int((p >> (8 * k)) & 0xFFu); }

// Exercises the SVG subset on a 48 x 48 canvas (cooked at 48: 1 unit = 1 px).
constexpr const char* kFeatureSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" viewBox="0 0 48 48">
  <rect x="2" y="2" width="12" height="12" rx="3" fill="currentColor"/>
  <circle cx="24" cy="8" r="6" fill="#ff0000"/>
  <ellipse cx="40" cy="8" rx="6" ry="4" fill="currentColor" opacity="0.5"/>
  <line x1="2" y1="22" x2="14" y2="22" stroke="currentColor" stroke-width="4"/>
  <polyline points="18,18 24,26 30,18" fill="none" stroke="currentColor" stroke-width="2"/>
  <polygon points="34,18 46,18 40,28" fill="currentColor"/>
  <path fill-rule="evenodd" d="M2 32h14v14H2z M6 36h6v6H6z"/>
  <path d="M18 32h14v14H18z m4 4h6v6h-6z"/>
  <g transform="translate(34 32)"><path d="M0 7 A7 7 0 0 1 14 7 Z"/></g>
</svg>)svg";

int cookGate() {
    auto dir = ic::loadSvgDirectory(FUSE_UI_ICON_SVG_DIR);
    UI_CHECK(dir.has_value());
    if (!dir) return finish("fuse_ui_icon_cook");
    std::printf("  %zu SVGs in %s\n", dir->size(), FUSE_UI_ICON_SVG_DIR);
    UI_CHECK(dir->size() >= 30);
    // determinism: twice, and with the inputs reversed / shuffled
    ic::CookStats st;
    auto a = ic::cookIcons(*dir, {}, &st);
    UI_CHECK(a.has_value());
    if (!a) return finish("fuse_ui_icon_cook");
    std::printf("  pack: %u icons, atlas %dx%d, %zu bytes\n", st.icons, st.atlasWidth, st.atlasHeight, st.bytes);
    auto rev = *dir;
    std::reverse(rev.begin(), rev.end());
    std::shuffle(rev.begin() + 1, rev.end(), std::mt19937(7));
    auto b = ic::cookIcons(*dir, {}), c = ic::cookIcons(rev, {});
    UI_CHECK(b && *b == *a);
    UI_CHECK(c && *c == *a);
    // the build-time pack (fuse_ui_icon_cook tool over the same directory) is byte-identical
    const std::vector<u8> built = readBytes(FUSE_UI_ICON_PACK);
    UI_CHECK(!built.empty() && built == *a);

    // index round trip
    auto set = IconSet::fromBytes(*a);
    UI_CHECK(set.has_value());
    if (!set) return finish("fuse_ui_icon_cook");
    UI_CHECK_EQ(set->count(), u32(dir->size()));
    UI_CHECK(set->sizes().size() == 5 && set->sizes()[0] == 16 && set->sizes()[4] == 48);
    for (u32 i = 0; i < set->count(); ++i) {
        UI_CHECK(set->name(i) == (*dir)[i].name); // directory listing is name-sorted too
        UI_CHECK_EQ(set->find((*dir)[i].name), i32(i));
        UI_CHECK_EQ(set->findHash(hashId((*dir)[i].name)), i32(i));
        for (u32 k = 0; k < u32(set->sizes().size()); ++k) {
            const int size = set->sizes()[k];
            auto cov = ic::rasteriseSvg((*dir)[i].svg, size);
            UI_CHECK(cov.has_value());
            if (!cov) continue;
            const IconSet::Entry& e = set->entry(i, k);
            UI_CHECK(e.w > 0 && e.h > 0);
            std::vector<u8> cell;
            set->render(i, size, cell); // exact size: the cooked cell
            UI_CHECK(cell == *cov);
            IconBitmap bm;
            UI_CHECK(set->cooked(i, size, bm) && bm.w == e.w && bm.ox == e.ox);
        }
    }
    UI_CHECK(set->find("save") >= 0 && set->find("no-such-icon") < 0 && set->findHash(hashId("nope")) < 0);

    // SVG subset
    {
        auto cov = ic::rasteriseSvg(kFeatureSvg, 48);
        UI_CHECK(cov.has_value());
        if (cov) {
            auto at = [&](int x, int y) { return int((*cov)[std::size_t(y) * 48 + std::size_t(x)]); };
            UI_CHECK(at(8, 8) == 255 && at(2, 2) < 160);           // rounded rect: interior full, corner cut
            UI_CHECK(at(24, 8) == 255 && at(24, 1) == 0);          // circle (any fill colour = coverage)
            UI_CHECK(std::abs(at(40, 8) - 128) <= 3);              // opacity 0.5
            UI_CHECK(at(8, 21) == 255 && at(8, 25) == 0);          // stroked line, width 4
            UI_CHECK(at(21, 22) > 128 && at(24, 18) == 0);         // polyline stroke, unfilled
            UI_CHECK(at(40, 20) == 255 && at(35, 26) == 0);        // polygon
            UI_CHECK(at(3, 33) == 255 && at(9, 39) == 0);          // evenodd: hole
            UI_CHECK(at(25, 39) == 255);                           // nonzero, same winding: filled
            UI_CHECK(at(41, 34) == 255 && at(41, 43) == 0);        // g transform + arc: upper half disk
        }
        std::printf("  feature SVG: %s\n", cov ? "ok" : cov.error().c_str());
    }
    // resampling: a non-cooked size tracks a direct rasterisation
    {
        const i32 idx = set->find("settings");
        UI_CHECK(idx >= 0);
        for (int size : {12, 18, 28, 40, 64}) {
            std::vector<u8> cell;
            set->render(u32(idx), size, cell);
            auto direct = ic::rasteriseSvg((*dir)[std::size_t(idx)].svg, size);
            if (!direct) { UI_CHECK(false); continue; }
            double diff = 0.0, mass = 0.0, mass2 = 0.0;
            for (std::size_t p = 0; p < cell.size(); ++p) {
                diff += std::abs(int(cell[p]) - int((*direct)[p]));
                mass += cell[p];
                mass2 += (*direct)[p];
            }
            const double rel = diff / std::max(mass2, 1.0);
            std::printf("  resample %2d px: mean |diff| %.1f, coverage mass %.0f vs %.0f\n", size, diff / double(cell.size()), mass, mass2);
            UI_CHECK(rel < 0.35 && std::abs(mass - mass2) < 0.15 * mass2);
        }
    }
    // bad input
    UI_CHECK(!ic::cookIcons({{"a", "<svg width='8' height='8'><rect width='8' height='8'/></svg>"}, {"a", "<svg/>"}}));
    UI_CHECK(!ic::cookIcons({{"Bad Name", kFeatureSvg}}));
    UI_CHECK(!ic::cookIcons({{"empty", "<svg width='8' height='8'></svg>"}}));
    UI_CHECK(!ic::cookIcons({{"junk", "not an svg at all"}}));
    UI_CHECK(!ic::cookIcons({{"ok", kFeatureSvg}}, {.sizes = {0}}));
    UI_CHECK(ic::cookIcons({{"ok", kFeatureSvg}}, {.sizes = {7, 48}}).has_value());
    // corrupt packs: never crash, never load
    UI_CHECK(!IconSet::fromBytes(std::span<const u8>(a->data(), a->size() - 1)));
    UI_CHECK(!IconSet::fromBytes(std::span<const u8>(a->data(), 20)));
    {
        std::vector<u8> bad = *a;
        bad[0] = 'X';
        UI_CHECK(!IconSet::fromBytes(bad));
    }
    {
        std::mt19937 rng(1234);
        const std::size_t header = a->size() - std::size_t(set->atlasWidth()) * std::size_t(set->atlasHeight());
        int loaded = 0;
        for (int it = 0; it < 3000; ++it) {
            std::vector<u8> m = *a;
            const int flips = 1 + int(rng() % 4);
            for (int f = 0; f < flips; ++f) m[rng() % header] ^= u8(1u << (rng() % 8));
            if (it % 7 == 0) m.resize(rng() % m.size());
            auto s = IconSet::fromBytes(m);
            if (s) { // a flip that still parses must still be self-consistent
                ++loaded;
                std::vector<u8> cell;
                for (u32 i = 0; i < s->count(); ++i) s->render(i, 20, cell);
            }
        }
        std::printf("  fuzz: 3000 corrupted packs, %d still valid (in-range edits)\n", loaded);
    }
    // multi-colour packs (layered palette)
    {
        auto cdir = ic::loadSvgDirectory(FUSE_UI_ICON_COLOR_DIR);
        UI_CHECK(cdir.has_value());
        ic::CookStats cst;
        auto cp = cdir ? ic::cookIcons(*cdir, {}, &cst) : std::unexpected(std::string("no dir"));
        UI_CHECK(cp.has_value());
        if (cp) {
            UI_CHECK(readBytes(FUSE_UI_ICON_COLOR_PACK) == *cp); // the build-time pack (tool) is byte-identical
            std::printf("  colour pack: %u icons (%u multi-colour), atlas %dx%d, %zu bytes\n", cst.icons, cst.multiColor,
                        cst.atlasWidth, cst.atlasHeight, cst.bytes);
            UI_CHECK_EQ(cst.multiColor, 5u);
            auto cs = IconSet::fromBytes(*cp);
            UI_CHECK(cs.has_value());
            struct Want {
                const char* name;
                std::vector<u32> colors; // RGBA8, R low byte; 0 = currentColor
            };
            const Want wants[] = {
                {"alert-color", {0xFF2DC0FBu, 0xFF000000u}},             // black kept in a multi-colour icon
                {"badge-check", {0xFFE5881Eu, 0xFFFFFFFFu}},
                {"flag", {0u, 0xFFFFFFFFu, 0xFF2F2FD3u, 0xFFC06515u}},   // consecutive reds merged
                {"ink-square", {0u}},                                     // black only: monochrome ink
                {"sunset", {0xFF49A3FFu, 0u}},                            // gradient: mean stop colour
                {"traffic-light", {0u, 0xFF3539E5u, 0xFF35D8FDu, 0xFF47A043u}},
            };
            for (const Want& wnt : wants) {
                const i32 i = cs ? cs->find(wnt.name) : -1;
                UI_CHECK(i >= 0);
                if (i < 0) continue;
                UI_CHECK_EQ(cs->layerCount(u32(i)), u32(wnt.colors.size()));
                if (cs->layerCount(u32(i)) != wnt.colors.size()) continue;
                for (u32 l = 0; l < cs->layerCount(u32(i)); ++l) UI_CHECK_EQ(cs->layerColor(u32(i), l), wnt.colors[l]);
                UI_CHECK_EQ(cs->multiColor(u32(i)), wnt.colors.size() > 1 || wnt.colors[0] != 0u);
                const std::string& svg = (*cdir)[std::size_t(i)].svg;
                for (u32 k = 0; k < u32(cs->sizes().size()); ++k) {
                    const int size = cs->sizes()[k];
                    auto layers = ic::rasteriseSvgLayers(svg, size);
                    UI_CHECK(layers && layers->size() == wnt.colors.size());
                    if (!layers || layers->size() != wnt.colors.size()) continue;
                    for (u32 l = 0; l < u32(layers->size()); ++l) {
                        std::vector<u8> cell;
                        cs->render(u32(i), size, cell, l);
                        UI_CHECK(cell == (*layers)[l].coverage);
                        UI_CHECK_EQ((*layers)[l].color, wnt.colors[l]);
                    }
                    if (layers->size() == 1) { // monochrome: the whole coverage
                        auto all = ic::rasteriseSvg(svg, size);
                        UI_CHECK(all && *all == (*layers)[0].coverage);
                    }
                }
            }
        }
        // version 1 pack (no layer fields): one implicit currentColor layer
        std::vector<u8> v1;
        auto put = [&](const void* p, std::size_t n) { v1.insert(v1.end(), static_cast<const u8*>(p), static_cast<const u8*>(p) + n); };
        auto put16 = [&](u16 v) { put(&v, 2); };
        auto put32 = [&](u32 v) { put(&v, 4); };
        put("FICN", 4);
        put32(1);
        put16(4); put16(4);  // atlas 4 x 4
        put16(1); put16(4);  // one size: 4
        put32(1);
        put32(hashId("dot"));
        put16(3);
        put("dot", 3);
        for (u16 v : {u16(0), u16(0), u16(2), u16(2), u16(1), u16(1)}) put16(v);
        for (int k = 0; k < 16; ++k) v1.push_back(k < 2 || (k >= 4 && k < 6) ? u8(255) : u8(0));
        auto s1 = IconSet::fromBytes(v1);
        UI_CHECK(s1.has_value());
        if (s1) {
            IconBitmap bm;
            UI_CHECK(s1->layerCount(0) == 1 && s1->layerColor(0, 0) == 0u && !s1->multiColor(0));
            UI_CHECK(s1->cooked(0, 4, bm) && bm.w == 2 && bm.ox == 1 && bm.pixels[0] == 255);
        }
    }
#if FUSE_UI_ICON_HAVE_DOC
    {
        fuse::ui::doc::CookInfo info;
        auto cooked = fuse::ui::doc::cookDocument(R"({"fuseui": 1, "root": {"class": "panel", "children": [
            {"class": "button", "icon": "save", "text": "Save"}, {"class": "icon", "style": {"icon": "search"}},
            {"class": "button", "icon": "save"}]}})", &info);
        UI_CHECK(cooked.has_value());
        UI_CHECK(info.icons.size() == 2 && info.icons[0] == "save" && info.icons[1] == "search");
    }
#endif
    return finish("fuse_ui_icon_cook");
}

// ---- render ---------------------------------------------------------------------------------------------------------
constexpr const char* kNames[] = {"save", "folder-open", "search", "settings", "triangle-alert", "star"};
constexpr int kSizes[] = {16, 24, 32, 40, 48};

void buildScene(UiContext& ui) {
    (void)ui.addStyleSheet(R"css(
        .rows { padding: 12px; gap: 8px; }
        .row { flex-direction: row; gap: 6px; align-items: center; }
        .accent { color: #3d8bfd; }
        .tb:hover { color: #ffcc00; }
    )css");
    const NodeHandle rows = w::panel(ui, ui.root(), "rows");
    ui.addClass(rows, "rows");
    for (int size : kSizes) {
        const NodeHandle row = w::panel(ui, rows);
        ui.addClass(row, "row");
        for (const char* n : kNames) w::icon(ui, row, n, f32(size));
        for (const char* n : kNames) ui.addClass(w::icon(ui, row, n, f32(size)), "accent");
        const std::string id = "tb" + std::to_string(size);
        const NodeHandle tb = w::toolButton(ui, row, "", id);
        ui.addClass(tb, "tb");
        w::setIcon(ui, tb, "pencil", f32(size));
        const NodeHandle off = w::toolButton(ui, row, "", id + "off");
        w::setIcon(ui, off, "pencil", f32(size));
        ui.setDisabled(off, true);
    }
}

// Brightest pixel inside a node's rect (the icon's fully covered core carries the exact tint).
u32 brightest(const Image& img, Rect r) {
    u32 best = 0;
    int bestSum = -1;
    for (int y = int(r.y); y < int(r.bottom()); ++y)
        for (int x = int(r.x); x < int(r.right()); ++x) {
            const u32 p = img.at(x, y);
            const int s = ch(p, 0) + ch(p, 1) + ch(p, 2);
            if (s > bestSum) { bestSum = s; best = p; }
        }
    return best;
}
bool near(u32 p, u32 rgb, int tol = 2) {
    return std::abs(ch(p, 0) - int((rgb >> 16) & 0xFF)) <= tol && std::abs(ch(p, 1) - int((rgb >> 8) & 0xFF)) <= tol &&
           std::abs(ch(p, 2) - int(rgb & 0xFF)) <= tol;
}

int renderGate(int argc, char** argv) {
    auto set = IconSet::fromFile(FUSE_UI_ICON_PACK);
    UI_CHECK(set.has_value());
    if (!set) { std::fprintf(stderr, "%s\n", set.error().c_str()); return finish("fuse_ui_icon_render"); }
    const std::string golden = argc > 1 && std::string(argv[1]).rfind("--", 0) != 0 ? argv[1] : "golden/icons.png";

    UiContext ui({.viewport = {720, 260}, .font = &font()});
    w::install(ui);
    ui.setIcons(&*set);
    buildScene(ui);
    ui.update();
    Image img;
    render(ui, img);

    // every icon node is a Glyph command of its own node; sizes are the cell sizes asked for
    {
        u32 glyphs = 0;
        for (const DrawCmd& c : ui.drawList().cmds()) glyphs += c.op == DrawOp::Glyph;
        UI_CHECK_EQ(glyphs, u32(std::size(kSizes) * (2 * std::size(kNames) + 2)));
        const NodeHandle row0 = ui.firstChild(ui.find("rows"));
        UI_CHECK_RECT(ui.rect(ui.firstChild(row0)), 12, 16, 16, 16); // centred in a 24 px row (tool button + padding)
    }
    // tints: theme text colour, the .accent colour, the disabled colour (theme --text-dim): the Glyph fill at every
    // size, and the pixels of fully covered texels where strokes are >= 2 px (24 / 48 px)
    auto fillOf = [&](NodeHandle n) {
        for (const DrawCmd& c : ui.drawList().cmds())
            if (c.op == DrawOp::Glyph && c.node == n.index) return packPremultiplied(c.fill) & 0xFFFFFFu;
        return 0xFFFFFFFFu;
    };
    auto rgbHex = [](u32 hex) { return packPremultiplied(Color::rgb(hex)) & 0xFFFFFFu; };
    for (int size : kSizes) {
        const std::string id = "tb" + std::to_string(size);
        const NodeHandle tb = ui.find(id), off = ui.find(id + "off");
        const NodeHandle first = ui.firstChild(ui.parent(tb));
        NodeHandle accent = first;
        for (std::size_t k = 0; k < std::size(kNames); ++k) accent = ui.nextSibling(accent);
        UI_CHECK_EQ(fillOf(first), rgbHex(0xE6E6EB));
        UI_CHECK_EQ(fillOf(accent), rgbHex(0x3D8BFD));
        UI_CHECK_EQ(fillOf(tb), rgbHex(0xE6E6EB));
        UI_CHECK_EQ(fillOf(off), rgbHex(0x9A9CAB));
        if (size != 24 && size != 48) continue;
        UI_CHECK(near(brightest(img, ui.rect(first)), 0xE6E6EB));
        UI_CHECK(near(brightest(img, ui.rect(accent)), 0x3D8BFD));
        UI_CHECK(near(brightest(img, ui.rect(off)), 0x9A9CAB));
    }
    if (std::getenv("FUSE_UI_UPDATE_GOLDEN")) {
        stbi_write_png(golden.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("golden written: %s\n", golden.c_str());
    } else {
        int gw = 0, gh = 0, gn = 0;
        unsigned char* ref = stbi_load(golden.c_str(), &gw, &gh, &gn, 4);
        UI_CHECK(ref != nullptr);
        if (ref) {
            UI_CHECK(gw == img.width && gh == img.height);
            if (gw == img.width && gh == img.height) {
                int bad = 0, maxd = 0;
                for (int i = 0; i < gw * gh; ++i)
                    for (int k = 0; k < 4; ++k) {
                        const int d = std::abs(int(ref[i * 4 + k]) - ch(img.pixels[std::size_t(i)], k));
                        maxd = std::max(maxd, d);
                        if (d > 3) { ++bad; break; }
                    }
                std::printf("golden diff: %d pixels > 3, max channel diff %d\n", bad, maxd);
                UI_CHECK(bad <= gw * gh / 1000);
            }
            stbi_image_free(ref);
        }
    }
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]).rfind("--out=", 0) == 0)
            stbi_write_png(argv[i] + 6, img.width, img.height, 4, img.pixels.data(), img.width * 4);

    // hover recolours the icon (currentColor follows :hover)
    {
        UiTestDriver drv(ui);
        drv.hover("tb24");
        Image h;
        render(ui, h);
        UI_CHECK_EQ(fillOf(ui.find("tb24")), rgbHex(0xFFCC00));
        UI_CHECK_EQ(fillOf(ui.find("tb32")), rgbHex(0xE6E6EB));
        UI_CHECK(near(brightest(h, ui.rect(ui.find("tb24"))), 0xFFCC00));
    }
    // a steady frame re-uses the atlas entries (no new misses)
    {
        const u32 misses = ui.atlas().misses();
        ui.markAllDirty();
        ui.update();
        UI_CHECK_EQ(ui.atlas().misses(), misses);
    }
    // buttons: icon then text, measured as icon + gap + text; unknown icons take no space
    {
        UiContext u2({.viewport = {400, 200}, .font = &font()});
        w::install(u2);
        u2.setIcons(&*set);
        const NodeHandle plain = w::button(u2, u2.root(), "Save");
        const NodeHandle withIcon = w::button(u2, u2.root(), "Save");
        w::setIcon(u2, withIcon, "save");
        const NodeHandle unknown = w::button(u2, u2.root(), "Save");
        w::setIcon(u2, unknown, "no-such-icon");
        u2.setCss(u2.root(), "align-items", "start");
        u2.update();
        UI_CHECK_EQ(u2.rect(withIcon).w, u2.rect(plain).w + 16.f + UiContext::kIconGap);
        UI_CHECK_EQ(u2.rect(unknown).w, u2.rect(plain).w);
        UI_CHECK_EQ(u2.computed(withIcon).icon, hashId("save"));
        // the icon is drawn left of the label's first glyph
        f32 iconX = -1.f, textX = 1e9f;
        for (const DrawCmd& c : u2.drawList().cmds())
            if (c.op == DrawOp::Glyph && c.node == withIcon.index) {
                if (c.rect.h >= 12.f && iconX < 0.f) iconX = c.rect.x;
                else textX = std::min(textX, c.rect.x);
            }
        UI_CHECK(iconX >= 0.f && iconX < textX);
        // without a set: no icon, same layout as no icon
        u2.setIcons(nullptr);
        u2.update();
        UI_CHECK_EQ(u2.rect(withIcon).w, u2.rect(plain).w);
        u2.setIcons(&*set);
        // style sheets: `icon` / `icon-size` (quoted or not, `none` clears)
        UI_CHECK(u2.setCss(plain, "icon", "\"search\""));
        UI_CHECK(u2.setCss(plain, "icon-size", "24px"));
        u2.update();
        UI_CHECK(u2.computed(plain).icon == hashId("search") && u2.computed(plain).iconSize == 24.f);
        UI_CHECK(u2.setCss(plain, "icon", "none"));
        UI_CHECK(!u2.setCss(plain, "icon", "bad name!"));
        u2.update();
        UI_CHECK_EQ(u2.computed(plain).icon, 0u);
        // menu items: ":name: Label"
        const NodeHandle scope = w::popupMenu(u2, plain, ":save: Save\n:x: Close\nPlain", {10, 10});
        u2.update();
        const NodeHandle menu = u2.firstChild(scope);
        const NodeHandle i0 = u2.firstChild(menu), i1 = u2.nextSibling(i0), i2 = u2.nextSibling(i1);
        UI_CHECK(u2.text(i0) == "Save" && u2.computed(i0).icon == hashId("save"));
        UI_CHECK(u2.text(i1) == "Close" && u2.computed(i1).icon == hashId("x"));
        UI_CHECK(u2.text(i2) == "Plain" && u2.computed(i2).icon == 0u);
        bool menuIcon = false;
        for (const DrawCmd& c : u2.drawList().cmds()) menuIcon |= c.op == DrawOp::Glyph && c.node == i0.index && c.rect.h >= 12.f;
        UI_CHECK(menuIcon);
    }
    // multi-colour icons against tests/golden/icons_color.png
    {
        auto cs = IconSet::fromFile(FUSE_UI_ICON_COLOR_PACK);
        UI_CHECK(cs.has_value());
        if (cs) {
            UiContext uc({.viewport = {420, 240}, .font = &font()});
            w::install(uc);
            uc.setIcons(&*cs);
            (void)uc.addStyleSheet(R"css(
                .rows { padding: 10px; gap: 6px; background: #2a2d3a; }
                .row { flex-direction: row; gap: 6px; align-items: center; }
                .accent { color: #3d8bfd; }
            )css");
            const NodeHandle rows = w::panel(uc, uc.root(), "crows");
            uc.addClass(rows, "rows");
            std::vector<NodeHandle> plainIcons, accentIcons;
            for (int size : kSizes) {
                const NodeHandle row = w::panel(uc, rows);
                uc.addClass(row, "row");
                for (u32 i = 0; i < cs->count(); ++i) plainIcons.push_back(w::icon(uc, row, cs->name(i), f32(size)));
                for (u32 i = 0; i < cs->count(); ++i) {
                    accentIcons.push_back(w::icon(uc, row, cs->name(i), f32(size)));
                    uc.addClass(accentIcons.back(), "accent");
                }
                const NodeHandle off = w::toolButton(uc, row, "", "coff" + std::to_string(size));
                w::setIcon(uc, off, "traffic-light", f32(size));
                uc.setDisabled(off, true);
            }
            uc.update();
            Image ci;
            render(uc, ci);
            // one Glyph per non-empty layer; currentColor layers carry the tint, fixed layers their own colour
            auto fills = [&](NodeHandle n) {
                std::vector<u32> f;
                for (const DrawCmd& c : uc.drawList().cmds())
                    if (c.op == DrawOp::Glyph && c.node == n.index) f.push_back(packPremultiplied(c.fill));
                return f;
            };
            for (std::size_t k = 0; k < plainIcons.size(); ++k) {
                const u32 i = u32(k % cs->count());
                const std::vector<u32> a = fills(plainIcons[k]), b = fills(accentIcons[k]);
                UI_CHECK_EQ(a.size(), std::size_t(cs->layerCount(i)));
                UI_CHECK_EQ(b.size(), a.size());
                if (a.size() != cs->layerCount(i) || b.size() != a.size()) continue;
                for (u32 l = 0; l < cs->layerCount(i); ++l) {
                    const u32 lc = cs->layerColor(i, l);
                    if (lc == 0u) {
                        UI_CHECK_EQ(a[l], packPremultiplied(Color::rgb(0xE6E6EB)));
                        UI_CHECK_EQ(b[l], packPremultiplied(Color::rgb(0x3D8BFD)));
                    } else {
                        UI_CHECK_EQ(a[l], lc);
                        UI_CHECK_EQ(b[l], lc);
                    }
                }
            }
            {
                const std::vector<u32> off = fills(uc.find("coff24"));
                UI_CHECK(off.size() == 4 && off[0] == packPremultiplied(Color::rgb(0x9A9CAB)) && off[1] == 0xFF3539E5u);
            }
            const std::string cgolden = "golden/icons_color.png";
            if (std::getenv("FUSE_UI_UPDATE_GOLDEN")) {
                stbi_write_png(cgolden.c_str(), ci.width, ci.height, 4, ci.pixels.data(), ci.width * 4);
                std::printf("golden written: %s\n", cgolden.c_str());
            } else {
                int gw = 0, gh = 0, gn = 0;
                unsigned char* ref = stbi_load(cgolden.c_str(), &gw, &gh, &gn, 4);
                UI_CHECK(ref != nullptr);
                if (ref) {
                    UI_CHECK(gw == ci.width && gh == ci.height);
                    if (gw == ci.width && gh == ci.height) {
                        int bad = 0, maxd = 0;
                        for (int p = 0; p < gw * gh; ++p)
                            for (int k = 0; k < 4; ++k) {
                                const int d = std::abs(int(ref[p * 4 + k]) - ch(ci.pixels[std::size_t(p)], k));
                                maxd = std::max(maxd, d);
                                if (d > 3) { ++bad; break; }
                            }
                        std::printf("colour golden diff: %d pixels > 3, max channel diff %d\n", bad, maxd);
                        UI_CHECK(bad <= gw * gh / 1000);
                    }
                    stbi_image_free(ref);
                }
            }
            for (int i = 1; i < argc; ++i)
                if (std::string(argv[i]).rfind("--out-color=", 0) == 0)
                    stbi_write_png(argv[i] + 12, ci.width, ci.height, 4, ci.pixels.data(), ci.width * 4);
        }
    }
#if FUSE_UI_ICON_HAVE_DOC
    // .fuseui `icon` key and a cooked style sheet carrying the property
    {
        UiContext u3({.viewport = {400, 200}, .font = &font()});
        w::install(u3);
        u3.setIcons(&*set);
        auto doc = fuse::ui::doc::Document::load(u3, R"({"fuseui": 1, "root": {"class": "panel", "children": [
            {"class": "button", "id": "b", "icon": "save", "text": "Save"},
            {"class": "icon", "id": "i", "classes": ["big"]}]}})");
        UI_CHECK(doc.has_value());
        auto cooked = fuse::ui::doc::cookStyle(".big { icon: star; icon-size: 32px; }");
        UI_CHECK(cooked.has_value());
        if (cooked) {
            auto sheet = fuse::ui::doc::loadCookedStyle(*cooked);
            UI_CHECK(sheet.has_value());
            if (sheet) u3.addStyleSheet(std::move(*sheet));
        }
        u3.update();
        UI_CHECK_EQ(u3.computed(u3.find("b")).icon, hashId("save"));
        UI_CHECK(u3.computed(u3.find("i")).icon == hashId("star") && u3.rect(u3.find("i")).h == 32.f);
        UI_CHECK(!fuse::ui::doc::Document::load(u3, R"({"fuseui": 1, "root": {"class": "button", "icon": "a b"}})"));
    }
#endif
    return finish("fuse_ui_icon_render");
}
} // namespace

int main(int argc, char** argv) {
    const std::string gate = FUSE_UI_ICON_GATE;
    if (gate == "cook") return cookGate();
    return renderGate(argc, argv);
}
